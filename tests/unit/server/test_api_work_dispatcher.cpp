#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "ApiWorkDispatcher.h"
#include "Network/Socket.h"
#include "Poller/EventPoller.h"
#include "Util/SqliteConnection.h"
#include "WebApi.h"

using namespace toolkit;
using namespace mediakit;

namespace {

struct Reply {
    int status = 0;
    Json::Value body;
    bool on_owner = false;
};

// Collects responses; `count` lets tests check exactly-once delivery.
struct ReplyProbe {
    std::mutex mtx;
    std::vector<Reply> replies;
    std::promise<void> first;
    std::atomic<bool> first_set{false};

    HttpSession::HttpResponseInvoker invoker(const EventPoller::Ptr &owner) {
        auto self = this;
        return HttpSession::HttpResponseInvoker(
            [self, owner](int code, const StrCaseMap &, const std::string &body) {
                Reply r;
                r.status = code;
                Json::Reader().parse(body, r.body);
                r.on_owner = owner->isCurrentThread();
                {
                    std::lock_guard<std::mutex> lck(self->mtx);
                    self->replies.push_back(r);
                }
                if (!self->first_set.exchange(true)) {
                    self->first.set_value();
                }
            });
    }

    bool waitFirst(int ms = 3000) {
        return first.get_future().wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
    }

    std::vector<Reply> settle(int ms = 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        std::lock_guard<std::mutex> lck(mtx);
        return replies;
    }
};

class DummySession : public SocketHelper {
public:
    explicit DummySession(const EventPoller::Ptr &poller) : SocketHelper(Socket::createSocket(poller, false)) {}
    void onRecv(const Buffer::Ptr &) override {}
    void onError(const SockException &) override {}
    void onManager() override {}
};

EventPoller::Ptr ownerPoller() {
    return EventPollerPool::Instance().getFirstPoller();
}

HttpSession::KeyValue jsonHeader() {
    HttpSession::KeyValue header;
    header["Content-Type"] = "application/json";
    return header;
}

Json::Value successVal() {
    Json::Value val;
    val["code"] = API::Success;
    return val;
}

} // namespace

TEST(ApiWorkDispatcherTest, RunsWorkOffPollerAndRepliesOnOwner) {
    auto owner = ownerPoller();
    ReplyProbe probe;
    std::atomic<bool> work_on_poller{true};
    std::atomic<bool> completion_on_owner{false};

    dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probe.invoker(owner),
        [&work_on_poller](Json::Value &val) {
            // API workers are plain threads, never an EventPoller.
            work_on_poller = static_cast<bool>(EventPoller::getCurrentPoller());
            val["data"] = 42;
        },
        [&completion_on_owner, owner](Json::Value &val) {
            completion_on_owner = owner->isCurrentThread();
            val["completed"] = true;
        });

    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_FALSE(work_on_poller);
    EXPECT_TRUE(completion_on_owner);
    EXPECT_TRUE(replies[0].on_owner);
    EXPECT_EQ(200, replies[0].status);
    EXPECT_EQ(API::Success, replies[0].body["code"].asInt());
    EXPECT_EQ(42, replies[0].body["data"].asInt());
    EXPECT_TRUE(replies[0].body["completed"].asBool());
}

TEST(ApiWorkDispatcherTest, MapsExceptionsLikeSynchronousHandlers) {
    struct Case {
        std::function<void()> thrower;
        int status;
        int code;
    };
    std::vector<Case> cases = {
        { [] { throw std::runtime_error("boom"); }, 500, API::Exception },
        { [] { throw InvalidArgsException("bad args"); }, 400, API::InvalidArgs },
        { [] { throw AuthException("no auth"); }, 401, API::AuthFailed },
        { [] { throw SqliteException("SELECT 1", "database is locked"); }, 500, API::SqlFailed },
    };

    auto owner = ownerPoller();
    for (size_t i = 0; i < cases.size(); ++i) {
        ReplyProbe probe;
        auto thrower = cases[i].thrower;
        dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probe.invoker(owner),
                        [thrower](Json::Value &) { thrower(); });
        ASSERT_TRUE(probe.waitFirst()) << "case " << i;
        auto replies = probe.settle(50);
        ASSERT_EQ(1u, replies.size()) << "case " << i;
        EXPECT_EQ(cases[i].status, replies[0].status) << "case " << i;
        EXPECT_EQ(cases[i].code, replies[0].body["code"].asInt()) << "case " << i;
        EXPECT_TRUE(replies[0].on_owner) << "case " << i;
    }
}

TEST(ApiWorkDispatcherTest, CompletionFailureIsReportedOnce) {
    auto owner = ownerPoller();
    ReplyProbe probe;
    dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probe.invoker(owner),
        [](Json::Value &val) { val["data"] = 1; },
        [](Json::Value &) { throw std::runtime_error("completion failed"); });

    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(500, replies[0].status);
    EXPECT_EQ(API::Exception, replies[0].body["code"].asInt());
    EXPECT_EQ("completion failed", replies[0].body["msg"].asString());
}

TEST(ApiWorkDispatcherTest, SkipsWorkWhenSessionIsGone) {
    auto owner = ownerPoller();
    std::weak_ptr<SocketHelper> weak_session;
    {
        auto session = std::make_shared<DummySession>(owner);
        weak_session = session;
    }
    ASSERT_TRUE(weak_session.expired());

    ReplyProbe probe;
    std::atomic<bool> work_ran{false};
    dispatchApiWork(owner, weak_session, jsonHeader(), successVal(), probe.invoker(owner),
                    [&work_ran](Json::Value &) { work_ran = true; });

    EXPECT_FALSE(probe.waitFirst(300));
    EXPECT_FALSE(work_ran);
}

TEST(ApiWorkDispatcherTest, ApiRetExceptionShutsDownLiveSession) {
    auto owner = ownerPoller();
    auto session = std::make_shared<DummySession>(owner);
    ReplyProbe probe;
    dispatchApiWork(owner, session, jsonHeader(), successVal(), probe.invoker(owner),
                    [](Json::Value &) { throw InvalidArgsException("bad args"); });
    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(400, replies[0].status);

    // Session released after the shutdown task ran on its poller; must not crash.
    std::weak_ptr<SocketHelper> weak_session = session;
    session.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(weak_session.expired());
}

TEST(ApiWorkDispatcherTest, SlowWorkDoesNotBlockOwnerPoller) {
    auto owner = ownerPoller();
    ReplyProbe probe;
    dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probe.invoker(owner),
                    [](Json::Value &) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); });

    // While the work sleeps, the owner poller must still run its own tasks promptly.
    std::promise<void> ran;
    auto start = std::chrono::steady_clock::now();
    owner->async([&ran]() { ran.set_value(); }, false);
    ASSERT_EQ(std::future_status::ready, ran.get_future().wait_for(std::chrono::milliseconds(200)));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(200));

    ASSERT_TRUE(probe.waitFirst());
    EXPECT_EQ(1u, probe.settle().size());
}
