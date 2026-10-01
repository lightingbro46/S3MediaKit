#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Poller/EventPoller.h"
#include "Server/GlobalMonitor.h"
#include "Storage/DbStorage.h"
#include "Storage/SystemMetrics.h"
#include "Thread/WorkThreadPool.h"
#include "Util/mini.h"

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

class GlobalMonitorThreadingTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        char tmpl[] = "/tmp/s3mk_global_monitor_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        s_dir = dir;
        mINI::Instance()[Database::kDbSavePath] = s_dir;
        mINI::Instance()[GlobalMonitorConfig::kMetricsSampleIntervalSec] = 1;

        SystemMetricsImp repository;
        auto transaction = repository.getExecutor()->beginWriteTxn();
        transaction->execScript(
            "CREATE TABLE system_metrics ("
            "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, "
            "cpu_usage_pct REAL NOT NULL DEFAULT 0, cpu_proc_usage_pct REAL NOT NULL DEFAULT 0, "
            "cpu_cores INTEGER NOT NULL DEFAULT 0, ram_used INTEGER NOT NULL DEFAULT 0, "
            "ram_total INTEGER NOT NULL DEFAULT 0, ram_usage_pct REAL NOT NULL DEFAULT 0, "
            "reader_total INTEGER NOT NULL DEFAULT 0, reader_live INTEGER NOT NULL DEFAULT 0, "
            "reader_playback INTEGER NOT NULL DEFAULT 0, nets_json TEXT NOT NULL DEFAULT '[]', "
            "disks_json TEXT NOT NULL DEFAULT '[]');");
        transaction->commit();
    }

    static void TearDownTestSuite() {
        std::string cmd = "rm -rf '" + s_dir + "'";
        (void)std::system(cmd.c_str());
    }

    static std::string s_dir;
};

std::string GlobalMonitorThreadingTest::s_dir;

} // namespace

TEST_F(GlobalMonitorThreadingTest, MetricsInsertWaitsOnBlockingPoolNotPoller) {
    // The metrics timer runs on a network poller. Its SQLite insert used to run there too, so a
    // locked database stalled that poller (and every session on it) for busy_timeout (3 s) on
    // each sample.
    auto store = std::make_shared<SystemMetricsImp>();
    auto lock = store->getExecutor()->beginWriteTxn();

    GlobalMonitor::Instance().start();

    // Several samples (1 s apart) hit the locked database while every poller keeps answering.
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
    while (std::chrono::steady_clock::now() < until) {
        expectPollersResponsive();
        if (HasFailure()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    lock->commit();
    lock.reset();
    EXPECT_TRUE(waitFor([store]() { return !store->findByTimeRange(0, 0).empty(); }, 8000))
        << "no metric sample stored after the database was released";
}
