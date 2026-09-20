#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ApiWorkDispatcher.h"
#include "Common/BlockingExecutors.h"
#include "Network/Socket.h"
#include "Poller/EventPoller.h"
#include "Util/SqliteConnection.h"
#include "WebApi.h"
#include "WebApiErrCode.h"

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

TEST(ApiWorkDispatcherTest, UsesSharedApiPool) {
    EXPECT_EQ(managerkit::BlockingExecutors::api().get(), getApiWorkExecutor().get());
}

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

namespace {

// Blocks worker tasks until released.
struct Gate {
    std::shared_ptr<std::promise<void>> open = std::make_shared<std::promise<void>>();
    std::shared_future<void> opened = open->get_future().share();
    void release() { open->set_value(); }
    std::function<void(Json::Value &)> blockingWork(std::atomic<int> *started = nullptr) {
        auto opened_copy = opened;
        return [opened_copy, started](Json::Value &) {
            if (started) {
                ++(*started);
            }
            opened_copy.wait_for(std::chrono::seconds(5));
        };
    }
};

bool waitFor(const std::function<bool()> &cond, int ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

} // namespace

TEST(ApiWorkDispatcherTest, RejectsOverInflightLimitWithoutRunningWork) {
    auto owner = ownerPoller();
    const ApiWorkLimit limit("test.inflight", 1, 0);
    Gate gate;
    std::atomic<int> started{0};
    ReplyProbe first;
    dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), first.invoker(owner),
                    gate.blockingWork(&started), nullptr, limit);
    ASSERT_TRUE(waitFor([&started]() { return started.load() == 1; }));
    EXPECT_EQ(1, apiWorkInflight("test.inflight"));

    ReplyProbe second;
    std::atomic<bool> second_ran{false};
    dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), second.invoker(owner),
                    [&second_ran](Json::Value &) { second_ran = true; }, nullptr, limit);
    ASSERT_TRUE(second.waitFirst());
    auto rejected = second.settle(50);
    ASSERT_EQ(1u, rejected.size());
    EXPECT_EQ(503, rejected[0].status);
    EXPECT_EQ(static_cast<int>(managerkit::ApiErrCode::CODE_SERVER_BUSY), rejected[0].body["code"].asInt());
    EXPECT_TRUE(rejected[0].on_owner);
    EXPECT_FALSE(second_ran.load());

    gate.release();
    ASSERT_TRUE(first.waitFirst());
    EXPECT_EQ(200, first.settle(50)[0].status);
    EXPECT_TRUE(waitFor([]() { return apiWorkInflight("test.inflight") == 0; }));
}

TEST(ApiWorkDispatcherTest, InflightSlotIsReleasedWhenWorkThrows) {
    auto owner = ownerPoller();
    const ApiWorkLimit limit("test.throw", 1, 0);
    for (int i = 0; i < 3; ++i) {
        ReplyProbe probe;
        dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probe.invoker(owner),
                        [](Json::Value &) { throw std::runtime_error("boom"); }, nullptr, limit);
        ASSERT_TRUE(probe.waitFirst()) << "round " << i;
        // Would be 503 from the second round on if the slot leaked.
        EXPECT_EQ(500, probe.settle(20)[0].status) << "round " << i;
        EXPECT_TRUE(waitFor([]() { return apiWorkInflight("test.throw") == 0; })) << "round " << i;
    }
}

TEST(ApiWorkDispatcherTest, DropsRequestThatWaitedTooLongInQueue) {
    auto owner = ownerPoller();
    // Occupy every API worker so the limited request has to wait in the queue.
    const int threads = managerkit::BlockingExecutors::apiThreadCount();
    Gate gate;
    std::atomic<int> started{0};
    std::vector<std::unique_ptr<ReplyProbe>> blockers;
    for (int i = 0; i < threads; ++i) {
        blockers.emplace_back(new ReplyProbe());
        dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(),
                        blockers.back()->invoker(owner), gate.blockingWork(&started));
    }
    ASSERT_TRUE(waitFor([&started, threads]() { return started.load() == threads; }));

    ReplyProbe probe;
    std::atomic<bool> ran{false};
    dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probe.invoker(owner),
                    [&ran](Json::Value &) { ran = true; }, nullptr, ApiWorkLimit("test.queue", 0, 50));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    gate.release();

    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle(50);
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(503, replies[0].status);
    EXPECT_FALSE(ran.load());
    for (auto &blocker : blockers) {
        EXPECT_TRUE(blocker->waitFirst());
    }
}

TEST(ApiWorkDispatcherTest, ReadsGroupLimitsFromConfig) {
    auto snapshot = apiWorkLimit("sync.snapshot");
    EXPECT_EQ("sync.snapshot", snapshot.group);
    EXPECT_EQ(2, snapshot.max_inflight);
    EXPECT_EQ(8, apiWorkLimit("sync.changes").max_inflight);
    EXPECT_EQ(4, apiWorkLimit("device.discovery").max_inflight);
    auto unknown = apiWorkLimit("not.configured");
    EXPECT_EQ(0, unknown.max_inflight);
    EXPECT_EQ(0, unknown.max_queue_ms);
}

TEST(ApiWorkDispatcherTest, CallbackWorkRepliesOnceOnOwner) {
    auto owner = ownerPoller();
    ReplyProbe probe;
    auto invoker = probe.invoker(owner);
    std::atomic<bool> start_on_poller{true};
    runApiCallbackWork(owner, std::weak_ptr<SocketHelper>(), invoker,
        [invoker, &start_on_poller](const ApiReply &reply) {
            start_on_poller = static_cast<bool>(EventPoller::getCurrentPoller());
            reply([invoker]() { invoker(200, jsonHeader(), "{\"code\":0,\"data\":1}"); });
            // A second reply (e.g. a service calling back twice) is ignored.
            reply([invoker]() { invoker(500, jsonHeader(), "{\"code\":-1}"); });
        });
    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(200, replies[0].status);
    EXPECT_TRUE(replies[0].on_owner);
    EXPECT_FALSE(start_on_poller.load());
}

TEST(ApiWorkDispatcherTest, CallbackWorkReplyLaterFromAnotherThread) {
    auto owner = ownerPoller();
    ReplyProbe probe;
    auto invoker = probe.invoker(owner);
    std::thread later;
    runApiCallbackWork(owner, std::weak_ptr<SocketHelper>(), invoker,
        [invoker, &later](const ApiReply &reply) {
            // Like a remote peer query: the worker returns, the answer arrives on another thread.
            later = std::thread([reply, invoker]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                reply([invoker]() { invoker(200, jsonHeader(), "{\"code\":0}"); });
            });
        });
    ASSERT_TRUE(probe.waitFirst());
    later.join();
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(200, replies[0].status);
    EXPECT_TRUE(replies[0].on_owner);
}

TEST(ApiWorkDispatcherTest, CallbackWorkExceptionIsAnsweredOnlyWithoutReply) {
    auto owner = ownerPoller();
    {
        ReplyProbe probe;
        runApiCallbackWork(owner, std::weak_ptr<SocketHelper>(), probe.invoker(owner),
            [](const ApiReply &) { throw std::runtime_error("query failed"); });
        ASSERT_TRUE(probe.waitFirst());
        auto replies = probe.settle();
        ASSERT_EQ(1u, replies.size());
        EXPECT_EQ(500, replies[0].status);
        EXPECT_EQ("query failed", replies[0].body["msg"].asString());
    }
    {
        ReplyProbe probe;
        auto invoker = probe.invoker(owner);
        runApiCallbackWork(owner, std::weak_ptr<SocketHelper>(), invoker,
            [invoker](const ApiReply &reply) {
                reply([invoker]() { invoker(200, jsonHeader(), "{\"code\":0}"); });
                throw std::runtime_error("after reply");
            });
        ASSERT_TRUE(probe.waitFirst());
        auto replies = probe.settle();
        ASSERT_EQ(1u, replies.size());
        EXPECT_EQ(200, replies[0].status);
    }
}

TEST(ApiWorkDispatcherTest, ApiResponseExceptionAnswersLikeReturnApiResponse) {
    auto owner = ownerPoller();
    auto session = std::make_shared<DummySession>(owner);
    std::weak_ptr<SocketHelper> weak_session = session;
    ReplyProbe probe;
    dispatchApiWork(owner, weak_session, jsonHeader(), successVal(), probe.invoker(owner),
        [](Json::Value &val) {
            val["data"]["flag"] = false;
            throw ApiResponseException(managerkit::ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
        });
    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(404, replies[0].status);
    EXPECT_EQ(static_cast<int>(managerkit::ApiErrCode::CODE_DEVICE_NOT_FOUND), replies[0].body["code"].asInt());
    EXPECT_EQ("Camera not found", replies[0].body["msg"].asString());
    EXPECT_FALSE(replies[0].body["data"]["flag"].asBool());
    EXPECT_TRUE(replies[0].body["data"].isMember("flag"));

    // Unlike ApiRetException, the connection is kept.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(session->getSock() != nullptr);
    EXPECT_FALSE(weak_session.expired());
}

TEST(ApiWorkDispatcherTest, ApiResponseExceptionUsesDefaultMessage) {
    auto owner = ownerPoller();
    ReplyProbe probe;
    runApiWork(owner, std::weak_ptr<SocketHelper>(),
        []() { throw ApiResponseException(managerkit::ApiErrCode::CODE_MSERVER_NOT_FOUND, ""); },
        [&probe, owner](const std::exception_ptr &error) {
            responseApiException(error, probe.invoker(owner), std::weak_ptr<SocketHelper>());
        });
    ASSERT_TRUE(probe.waitFirst());
    auto replies = probe.settle();
    ASSERT_EQ(1u, replies.size());
    EXPECT_EQ(404, replies[0].status);
    EXPECT_EQ(static_cast<int>(managerkit::ApiErrCode::CODE_MSERVER_NOT_FOUND), replies[0].body["code"].asInt());
    EXPECT_EQ(managerkit::getDefaultMessage(managerkit::ApiErrCode::CODE_MSERVER_NOT_FOUND), replies[0].body["msg"].asString());
}

TEST(ApiWorkDispatcherTest, SerialGroupRunsOneRequestAtATimeInOrder) {
    auto owner = ownerPoller();
    const int kRequests = 6;
    std::mutex mtx;
    std::vector<int> order;
    std::atomic<int> running{0};
    std::atomic<int> max_running{0};
    std::vector<std::unique_ptr<ReplyProbe>> probes;
    for (int i = 0; i < kRequests; ++i) {
        probes.emplace_back(new ReplyProbe());
        dispatchApiWork(owner, std::weak_ptr<SocketHelper>(), jsonHeader(), successVal(), probes.back()->invoker(owner),
            [i, &mtx, &order, &running, &max_running](Json::Value &val) {
                int now = ++running;
                int seen = max_running.load();
                while (now > seen && !max_running.compare_exchange_weak(seen, now)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                {
                    std::lock_guard<std::mutex> lck(mtx);
                    order.push_back(i);
                }
                --running;
                val["data"] = i;
            }, nullptr, apiWorkSerial("test.serial"));
    }
    for (int i = 0; i < kRequests; ++i) {
        ASSERT_TRUE(probes[i]->waitFirst()) << i;
        EXPECT_EQ(i, probes[i]->settle(0)[0].body["data"].asInt());
    }
    EXPECT_EQ(1, max_running.load());
    ASSERT_EQ(static_cast<size_t>(kRequests), order.size());
    for (int i = 0; i < kRequests; ++i) {
        EXPECT_EQ(i, order[i]);
    }
}
