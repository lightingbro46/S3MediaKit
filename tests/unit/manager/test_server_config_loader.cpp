#include <gtest/gtest.h>
#include <deque>
#include <future>
#include <stdexcept>
#include <vector>
#include "ServerConfigLoader.h"
#include "Poller/EventPoller.h"
#include "Thread/ThreadPool.h"

using namespace toolkit;
using namespace std;

namespace {
class ManualExecutor : public TaskExecutor {
public:
    Task::Ptr async(TaskIn task, bool = true) override {
        auto result = make_shared<Task>(std::move(task));
        tasks.emplace_back(result);
        return result;
    }
    void drain() {
        while (!tasks.empty()) {
            auto task = tasks.front();
            tasks.pop_front();
            (*task)();
        }
    }
    deque<Task::Ptr> tasks;
};

struct State {
    uint64_t now = 0;
    int calls = 0;
    int qualified_calls = 0;
    bool ready = true;
    bool fail = false;
    vector<int> applied;
};

shared_ptr<ServerConfigLoader> makeLoader(const shared_ptr<ManualExecutor> &pool, const shared_ptr<State> &state) {
    return make_shared<ServerConfigLoader>(pool, [state](const Json::Value &config) {
        ++state->calls;
        if (state->fail) throw runtime_error("injected apply failure");
        state->applied.push_back(config.get("devices", 0).asInt());
        return state->ready;
    }, [state](bool qualified) {
        ++state->qualified_calls;
        return qualified;
    }, [state]() { return state->now; });
}
}

TEST(ServerConfigLoaderTest, ReappliesUnchangedConfigPeriodicallyAndIncludesAudio) {
    auto pool = make_shared<ManualExecutor>();
    auto state = make_shared<State>();
    auto loader = makeLoader(pool, state);
    Json::Value config;
    config["devices"] = 1;
    loader->submit(config, true, 1, 300000);
    pool->drain();
    loader->submit(config, true, 2, 300000);
    pool->drain();
    EXPECT_EQ(state->calls, 1);
    EXPECT_EQ(state->qualified_calls, 2);
    config["list_audio_file"][0]["id"] = "audio";
    loader->submit(config, true, 3, 300000);
    pool->drain();
    EXPECT_EQ(state->calls, 2);
    state->now = 300000;
    loader->submit(config, true, 4, 300000);
    pool->drain();
    EXPECT_EQ(state->calls, 3);
    loader->submit(config, true, 5, 0);
    pool->drain();
    EXPECT_EQ(state->calls, 4);
}

TEST(ServerConfigLoaderTest, RetriesNotReadyAndExceptionsAndInvalidatesOnDecommission) {
    auto pool = make_shared<ManualExecutor>();
    auto state = make_shared<State>();
    auto loader = makeLoader(pool, state);
    Json::Value config;
    state->ready = false;
    loader->submit(config, true, 1, 300000);
    pool->drain();
    state->ready = true;
    state->fail = true;
    loader->submit(config, true, 2, 300000);
    pool->drain();
    state->fail = false;
    loader->submit(config, true, 3, 300000);
    pool->drain();
    EXPECT_EQ(state->calls, 3);
    loader->submit(config, false, 4, 300000);
    pool->drain();
    loader->submit(config, true, 5, 300000);
    pool->drain();
    EXPECT_EQ(state->calls, 4);
}

TEST(ServerConfigLoaderTest, KeepsLatestPendingAndRejectsOutOfOrderResponses) {
    auto pool = make_shared<ManualExecutor>();
    auto state = make_shared<State>();
    auto loader = makeLoader(pool, state);
    Json::Value config;
    config["devices"] = 1;
    loader->submit(config, true, 1, 0);
    config["devices"] = 3;
    loader->submit(config, true, 3, 0);
    config["devices"] = 2;
    loader->submit(config, true, 2, 0);
    EXPECT_EQ(pool->tasks.size(), 1u);
    pool->drain();
    ASSERT_EQ(state->applied.size(), 1u);
    EXPECT_EQ(state->applied.front(), 3);
}

TEST(ServerConfigLoaderTest, CoalescesUpdatesArrivingDuringAnApply) {
    auto pool = make_shared<ManualExecutor>();
    auto calls = make_shared<vector<int>>();
    auto weak = make_shared<weak_ptr<ServerConfigLoader>>();
    auto loader = make_shared<ServerConfigLoader>(pool, [calls, weak](const Json::Value &config) {
        calls->push_back(config["devices"].asInt());
        if (calls->size() == 1) {
            Json::Value next;
            next["devices"] = 2;
            weak->lock()->submit(next, true, 2, 0);
            next["devices"] = 3;
            weak->lock()->submit(next, true, 3, 0);
        }
        return true;
    }, [](bool qualified) { return qualified; });
    *weak = loader;
    Json::Value config;
    config["devices"] = 1;
    loader->submit(config, true, 1, 0);
    pool->drain();
    EXPECT_EQ(*calls, (vector<int> {1, 3}));
}

TEST(ServerConfigLoaderTest, BlockedApplyDoesNotBlockSubmittingPoller) {
    auto pool = make_shared<ThreadPool>(1, ThreadPool::PRIORITY_NORMAL, true, false, "config test");
    auto started = make_shared<promise<bool>>();
    auto release = make_shared<promise<void>>();
    auto gate = release->get_future().share();
    auto done = make_shared<promise<void>>();
    auto future = done->get_future();
    auto loader = make_shared<ServerConfigLoader>(pool, [started, gate, done](const Json::Value &) {
        started->set_value(!EventPoller::getCurrentPoller());
        gate.wait_for(chrono::seconds(3));
        done->set_value();
        return true;
    }, [](bool qualified) { return qualified; });
    auto poller = EventPollerPool::Instance().getFirstPoller();
    poller->async([loader]() { loader->submit(Json::Value(), true, 1, 0); }, false);
    auto start_future = started->get_future();
    ASSERT_EQ(start_future.wait_for(chrono::seconds(2)), future_status::ready);
    EXPECT_TRUE(start_future.get());
    auto heartbeat = make_shared<promise<void>>();
    auto beat = heartbeat->get_future();
    poller->async([heartbeat]() { heartbeat->set_value(); }, false);
    EXPECT_EQ(beat.wait_for(chrono::seconds(1)), future_status::ready);
    release->set_value();
    EXPECT_EQ(future.wait_for(chrono::seconds(2)), future_status::ready);
}
