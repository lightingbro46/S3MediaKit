#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Extension/Resource.h"
#include "Local/MediaGapFillManager.h"
#include "Poller/EventPoller.h"
#include "Storage/DbStorage.h"
#include "Storage/LocalResourceStatusGap.h"
#include "Storage/MediaGapFillTask.h"
#include "Thread/WorkThreadPool.h"
#include "Util/mini.h"
#include "Util/util.h"

using namespace managerkit;
using namespace toolkit;

namespace {

bool waitFor(const std::function<bool()> &pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// Every WorkThreadPool and EventPollerPool poller runs a task promptly.
void expectPollersResponsive() {
    std::vector<EventPoller::Ptr> pollers;
    auto collect = [&](const TaskExecutor::Ptr &executor) {
        pollers.push_back(std::dynamic_pointer_cast<EventPoller>(executor));
    };
    WorkThreadPool::Instance().for_each(collect);
    EventPollerPool::Instance().for_each(collect);
    for (auto &poller : pollers) {
        ASSERT_TRUE(poller);
        auto done = std::make_shared<std::atomic<bool>>(false);
        poller->async([done]() { *done = true; }, false);
        EXPECT_TRUE(waitFor([done]() { return done->load(); }, 500)) << "poller blocked";
    }
}

class MediaGapFillThreadingTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        char tmpl[] = "/tmp/s3mk_gap_fill_thr_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        s_dir = dir;
        mINI::Instance()[Database::kDbSavePath] = s_dir;

        MediaGapFillTaskImpl repository;
        auto transaction = repository.getExecutor()->beginWriteTxn();
        transaction->execScript(
            "CREATE TABLE media_gap_fill_task ("
            "connection_id TEXT NOT NULL PRIMARY KEY, gap_id TEXT NOT NULL, session_id TEXT NOT NULL, "
            "device_id TEXT NOT NULL, stream_id TEXT NOT NULL, source_type TEXT NOT NULL, "
            "origin_url TEXT NOT NULL, source_begin_time INTEGER NOT NULL, "
            "segment_index INTEGER NOT NULL, disconnect_time INTEGER NOT NULL, reconnect_time INTEGER NOT NULL, "
            "begin_time INTEGER NOT NULL, end_time INTEGER NOT NULL, processed_up_to INTEGER NOT NULL, "
            "processing_start_time INTEGER NOT NULL, processing_end_time INTEGER NOT NULL, "
            "retry_count INTEGER NOT NULL, retry_limit INTEGER NOT NULL, "
            "next_retry_time INTEGER NOT NULL, last_attempt_time INTEGER NOT NULL, state TEXT NOT NULL, "
            "last_error TEXT NOT NULL, updated_time INTEGER NOT NULL);"
            "CREATE TABLE media_gap_fill_session ("
            "device_id TEXT NOT NULL, gap_id TEXT NOT NULL, session_id TEXT NOT NULL, "
            "source_type TEXT NOT NULL, disconnect_time INTEGER NOT NULL, reconnect_time INTEGER NOT NULL, "
            "discovery_state TEXT NOT NULL, control_state TEXT NOT NULL, discovery_attempts INTEGER NOT NULL, "
            "last_error TEXT NOT NULL, updated_time INTEGER NOT NULL, PRIMARY KEY(device_id, gap_id));"
            "CREATE TABLE local_resource_status_gap ("
            "resource_id TEXT NOT NULL, gap_id TEXT NOT NULL, disconnect_time INTEGER NOT NULL, "
            "reconnect_time INTEGER NOT NULL, disconnect_reason TEXT NOT NULL, state INTEGER NOT NULL, "
            "updated_time INTEGER NOT NULL, PRIMARY KEY(resource_id, gap_id));");
        transaction->commit();
    }

    static void TearDownTestSuite() {
        std::string cmd = "rm -rf '" + s_dir + "'";
        (void)std::system(cmd.c_str());
    }

    static std::string s_dir;
};

std::string MediaGapFillThreadingTest::s_dir;

} // namespace

TEST_F(MediaGapFillThreadingTest, DiscoveryRunsOnGapFillPoolAndLeavesPollersFree) {
    // A camera answering its recording search slowly used to hold a WorkThreadPool poller
    // (SyncManager, ESC writes...) for the whole SOAP round trip.
    struct State {
        std::mutex mtx;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        bool done = false;
        std::string thread_name;
        bool on_poller = true;
    };
    auto st = std::make_shared<State>();

    MediaGapFillManager::SourceProvider provider;
    provider.discover = [st](const LocalResourceStatusGap &) {
        std::unique_lock<std::mutex> lck(st->mtx);
        st->entered = true;
        st->thread_name = getThreadName();
        st->on_poller = EventPoller::getCurrentPoller() != nullptr;
        st->cv.notify_all();
        st->cv.wait(lck, [st]() { return st->release; });
        st->done = true;
        return MediaGapDiscoveryResult{};
    };
    provider.start = [](const MediaGapFillRequest &, const MediaGapFillManager::ClosedCallback &) {
        return true;
    };
    auto &manager = MediaGapFillManager::Instance();
    ASSERT_TRUE(manager.registerSource("threading-test", provider));

    LocalResourceStatusGap gap;
    gap.resource_id = "camera-threading";
    gap.gap_id = "gap-threading";
    gap.disconnect_time = 1000;
    gap.reconnect_time = 1100;
    gap.updated_time = 1100;
    gap.state = 1;
    ASSERT_TRUE(LocalResourceStatusGapImp().addOrUpdate(gap));

    manager.onDeviceStateChanged("camera-threading", true, "threading-test");
    {
        std::unique_lock<std::mutex> lck(st->mtx);
        ASSERT_TRUE(st->cv.wait_for(lck, std::chrono::seconds(5), [st]() { return st->entered; }));
    }
    EXPECT_FALSE(st->on_poller) << st->thread_name;
    EXPECT_EQ(0u, st->thread_name.find("gapfill ")) << st->thread_name;

    expectPollersResponsive();

    {
        std::lock_guard<std::mutex> lck(st->mtx);
        st->release = true;
    }
    st->cv.notify_all();
    EXPECT_TRUE(waitFor([st]() {
        std::lock_guard<std::mutex> lck(st->mtx);
        return st->done;
    }));
    manager.unregisterSource("threading-test");
}

TEST_F(MediaGapFillThreadingTest, CloseFromPlayerPollerIsPersistedOffIt) {
    // The player close callback runs on the player's poller; the result is still persisted.
    auto closed_holder = std::make_shared<MediaGapFillManager::ClosedCallback>();
    auto started = std::make_shared<std::atomic<bool>>(false);
    MediaGapFillManager::SourceProvider provider;
    provider.discover = [](const LocalResourceStatusGap &) { return MediaGapDiscoveryResult{}; };
    provider.start = [closed_holder, started](const MediaGapFillRequest &,
                                              const MediaGapFillManager::ClosedCallback &closed) {
        *closed_holder = closed;
        *started = true;
        return true;
    };
    auto &manager = MediaGapFillManager::Instance();
    ASSERT_TRUE(manager.registerSource("close-test", provider));

    MediaGapFillRequest request;
    request.id = "close-task";
    request.source_type = "close-test";
    request.gap_id = "gap-close";
    request.session_id = "sess-2000";
    request.device_id = "camera-close";
    request.stream_id = "main";
    request.origin_url = "mock://recording";
    request.source_begin_time = 2000;
    request.disconnect_time = 2000;
    request.reconnect_time = 2020;
    request.begin_time = 2000;
    request.end_time = 2020;
    ASSERT_TRUE(manager.enqueue(request));
    ASSERT_TRUE(waitFor([started]() { return started->load(); }));

    auto poller = EventPollerPool::Instance().getPoller();
    poller->async([closed_holder]() { (*closed_holder)(true, 2020, "eof"); }, false);

    MediaGapFillTaskImpl repository;
    EXPECT_TRUE(waitFor([&]() {
        auto rows = repository.findById("close-task");
        return !rows.empty() && rows.front().state == "Success";
    }));
    manager.unregisterSource("close-test");
}

TEST_F(MediaGapFillThreadingTest, LockedDatabaseCostsEachStatusWriteOneBusyTimeout) {
    // Status gap writes run on each camera's ESC queue (api() threads). A process-wide mutex held
    // across the SQLite wait used to chain them: N cameras reconnecting against a locked database
    // waited N x busy_timeout (3 s) and held N api() threads that long.
    LocalResourceStatusGapImp repository;
    auto lock = repository.getExecutor()->beginWriteTxn();

    const int kWriters = 4;
    std::vector<std::thread> writers;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([i]() {
            ResourceStatusManager::Instance().onDisconnected("locked-res-" + std::to_string(i), "test", 1000);
        });
    }
    for (auto &writer : writers) {
        writer.join();
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    lock.reset();

    // Concurrent waits end together after one busy_timeout; chained ones would take 4 x 3 s.
    EXPECT_LT(elapsed_ms, 6000) << "status gap writes waited behind each other";

    // With the database free again the writes go through.
    EXPECT_TRUE(ResourceStatusManager::Instance().onDisconnected("locked-res-0", "test", 1000));
    EXPECT_EQ(1u, repository.findByResourceIdAndTimeRange("locked-res-0").size());
}
