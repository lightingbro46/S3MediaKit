#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "Common/BlockingExecutors.h"
#include "FFmpegSource.h"
#include "Poller/EventPoller.h"
#include "Storage/DbStorage.h"
#include "Storage/MigrationHistory.h"
#include "Util/mini.h"
#include "WebApiErrCode.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

// extractArchived/create: makeExtract() used to query the timefiles synchronously on the caller's
// thread (an HTTP request poller). The preparation (SQLite, disk, object storage) now runs on the
// api pool of BlockingExecutors, not on the caller nor on a WorkThreadPool poller.

namespace {

std::string g_db_dir;

class ExtractStartEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        char tmpl[] = "/tmp/s3mk_extract_start_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        g_db_dir = dir;
        mINI::Instance()[Database::kDbSavePath] = g_db_dir;
        // One api worker, so blockApiPool() holds the whole pool.
        mINI::Instance()[Blocking::kApiThreads] = 1;
        std::make_shared<MigrationHistoryImp>(Database::kMediaServerDb)->migrate(S3_MSERVER_MIGRATION_DIR);
    }

    void TearDown() override {
        if (!g_db_dir.empty()) {
            std::string cmd = "rm -rf '" + g_db_dir + "'";
            (void)std::system(cmd.c_str());
        }
    }
};

::testing::Environment *const g_env = ::testing::AddGlobalTestEnvironment(new ExtractStartEnvironment);

struct StartResult {
    SockException ex;
    std::string thread_name;
};

std::string currentThreadName() {
    char name[32] = { 0 };
    pthread_getname_np(pthread_self(), name, sizeof(name));
    return name;
}

// Blocks the (single-thread) api pool until the returned promise is set.
std::shared_ptr<std::promise<void>> blockApiPool() {
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    auto started = std::make_shared<std::promise<void>>();
    auto started_future = started->get_future();
    BlockingExecutors::api()->async([released, started]() {
        started->set_value();
        released.wait_for(std::chrono::seconds(5));
    });
    started_future.wait();
    return release;
}

FFmpegExtractor::Ptr makeExtractor(const EventPoller::Ptr &poller) {
    MediaTuple tuple = { DEFAULT_VHOST, "extract-start-no-such-camera", "", "" };
    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    ExtractOptions options = { now - 60, now, "start.mp4", "", "user", "user", "", false };
    return std::make_shared<FFmpegExtractor>(tuple, options, 500, poller);
}

} // namespace

TEST(ExtractStartTest, PreparesOnApiPoolNotOnCaller) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto extractor = makeExtractor(poller);
    auto release = blockApiPool();

    auto result = std::make_shared<std::promise<StartResult>>();
    auto result_future = result->get_future();
    std::atomic<int> calls{0};
    extractor->makeExtract("extract-start-key", g_db_dir + "/extract", [result, &calls](const SockException &ex) {
        if (++calls == 1) {
            StartResult r;
            r.ex = ex;
            r.thread_name = currentThreadName();
            result->set_value(r);
        }
    });
    // makeExtract returned while the api pool is still busy: nothing ran on this thread.
    EXPECT_EQ(0, calls.load());
    release->set_value();

    ASSERT_EQ(std::future_status::ready, result_future.wait_for(std::chrono::seconds(10)));
    auto r = result_future.get();
    EXPECT_EQ(0u, r.thread_name.find("api worker")) << r.thread_name;
    EXPECT_EQ(ApiErrCode::CODE_EXTRACT_SEGMENT_NO_DATA, r.ex.getCustomCode());
    auto status = extractor->status();
    EXPECT_TRUE(status.finished);
    EXPECT_FALSE(status.success);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(1, calls.load());
}

TEST(ExtractStartTest, CancelBeforePreparationStopsIt) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto extractor = makeExtractor(poller);
    auto release = blockApiPool();

    auto result = std::make_shared<std::promise<SockException>>();
    auto result_future = result->get_future();
    std::atomic<int> calls{0};
    extractor->makeExtract("extract-cancel-key", g_db_dir + "/extract", [result, &calls](const SockException &ex) {
        if (++calls == 1) {
            result->set_value(ex);
        }
    });
    extractor->cancel();
    release->set_value();

    ASSERT_EQ(std::future_status::ready, result_future.wait_for(std::chrono::seconds(10)));
    auto ex = result_future.get();
    EXPECT_TRUE(ex);
    EXPECT_EQ(ApiErrCode::CODE_EXTRACT_FAILED, ex.getCustomCode());
    // The preparation did not run: no "no data" completion was recorded.
    EXPECT_FALSE(extractor->status().finished);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(1, calls.load());
}
