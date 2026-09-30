#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Common/BlockingExecutors.h"
#include "Poller/EventPoller.h"
#include "Util/util.h"

using namespace managerkit;
using namespace toolkit;

namespace {

struct RunInfo {
    std::string thread_name;
    bool on_poller = false;
};

RunInfo runOn(const TaskExecutor::Ptr &executor) {
    auto done = std::make_shared<std::promise<RunInfo>>();
    auto future = done->get_future();
    executor->async([done]() {
        RunInfo info;
        info.thread_name = getThreadName();
        info.on_poller = static_cast<bool>(EventPoller::getCurrentPoller());
        done->set_value(info);
    }, false);
    EXPECT_EQ(std::future_status::ready, future.wait_for(std::chrono::seconds(3)));
    return future.get();
}

} // namespace

TEST(BlockingExecutorsTest, PoolsAreSingletons) {
    EXPECT_EQ(BlockingExecutors::api().get(), BlockingExecutors::api().get());
    EXPECT_EQ(BlockingExecutors::device().get(), BlockingExecutors::device().get());
    EXPECT_EQ(BlockingExecutors::ffmpeg().get(), BlockingExecutors::ffmpeg().get());
    EXPECT_NE(BlockingExecutors::api().get(), BlockingExecutors::device().get());
    // A slow ffmpeg wait must not use up the threads that serve API requests.
    EXPECT_NE(BlockingExecutors::ffmpeg().get(), BlockingExecutors::api().get());
    EXPECT_NE(BlockingExecutors::ffmpeg().get(), BlockingExecutors::device().get());
    EXPECT_NE(BlockingExecutors::ffmpeg().get(), BlockingExecutors::discovery().get());

    EXPECT_EQ(BlockingExecutors::storage().get(), BlockingExecutors::storage().get());
    // A storage maintenance cycle walks every recorded file and can run for minutes: it must not
    // use up the threads that serve REST requests, nor the ones that talk to cameras.
    EXPECT_NE(BlockingExecutors::storage().get(), BlockingExecutors::api().get());
    EXPECT_NE(BlockingExecutors::storage().get(), BlockingExecutors::device().get());
    EXPECT_NE(BlockingExecutors::storage().get(), BlockingExecutors::discovery().get());
    EXPECT_NE(BlockingExecutors::storage().get(), BlockingExecutors::ffmpeg().get());

    EXPECT_EQ(BlockingExecutors::restore().get(), BlockingExecutors::restore().get());
    // A viewer waiting for a cold segment must not queue behind a maintenance cycle or a
    // re-encryption job, and a slow download must not hold REST threads.
    EXPECT_NE(BlockingExecutors::restore().get(), BlockingExecutors::api().get());
    EXPECT_NE(BlockingExecutors::restore().get(), BlockingExecutors::storage().get());
    EXPECT_NE(BlockingExecutors::restore().get(), BlockingExecutors::encryption().get());
    EXPECT_NE(BlockingExecutors::restore().get(), BlockingExecutors::ffmpeg().get());

    EXPECT_EQ(BlockingExecutors::tier().get(), BlockingExecutors::tier().get());
    EXPECT_EQ(BlockingExecutors::tierTransfer().get(), BlockingExecutors::tierTransfer().get());
    // A burst of uploads must not delay the pool health check, and neither may hold a viewer's
    // restore or the storage maintenance cycle.
    EXPECT_NE(BlockingExecutors::tier().get(), BlockingExecutors::tierTransfer().get());
    EXPECT_NE(BlockingExecutors::tier().get(), BlockingExecutors::restore().get());
    EXPECT_NE(BlockingExecutors::tier().get(), BlockingExecutors::storage().get());
    EXPECT_NE(BlockingExecutors::tierTransfer().get(), BlockingExecutors::restore().get());
    EXPECT_NE(BlockingExecutors::tierTransfer().get(), BlockingExecutors::api().get());

    // Every camera searching its SD card after an outage must not take the threads PTZ needs.
    EXPECT_EQ(BlockingExecutors::gapFill().get(), BlockingExecutors::gapFill().get());
    EXPECT_NE(BlockingExecutors::gapFill().get(), BlockingExecutors::device().get());
    EXPECT_NE(BlockingExecutors::gapFill().get(), BlockingExecutors::discovery().get());
    EXPECT_NE(BlockingExecutors::gapFill().get(), BlockingExecutors::api().get());
}

TEST(BlockingExecutorsTest, ThreadsAreNamedAndAreNotPollers) {
    auto api = runOn(BlockingExecutors::api());
    EXPECT_EQ(0u, api.thread_name.find("api worker")) << api.thread_name;
    EXPECT_FALSE(api.on_poller);

    auto device = runOn(BlockingExecutors::device());
    EXPECT_EQ(0u, device.thread_name.find("device worker")) << device.thread_name;
    EXPECT_FALSE(device.on_poller);

    auto ffmpeg = runOn(BlockingExecutors::ffmpeg());
    EXPECT_EQ(0u, ffmpeg.thread_name.find("ffmpeg worker")) << ffmpeg.thread_name;
    EXPECT_FALSE(ffmpeg.on_poller);

    // The storage maintenance cycle used to run on a `work poller N` thread, where its blocking
    // scans and deletes held up every timer and ordered queue of the process.
    auto storage = runOn(BlockingExecutors::storage());
    EXPECT_EQ(0u, storage.thread_name.find("storage worker")) << storage.thread_name;
    EXPECT_FALSE(storage.on_poller);

    // Object storage downloads for restore used to run on a `work poller N` thread.
    auto restore = runOn(BlockingExecutors::restore());
    EXPECT_EQ(0u, restore.thread_name.find("restore ")) << restore.thread_name;
    EXPECT_FALSE(restore.on_poller);

    // Tier cycles and transfers used to run on `work poller N` threads.
    auto tier = runOn(BlockingExecutors::tier());
    EXPECT_EQ(0u, tier.thread_name.find("tier ")) << tier.thread_name;
    EXPECT_FALSE(tier.on_poller);
    auto transfer = runOn(BlockingExecutors::tierTransfer());
    EXPECT_EQ(0u, transfer.thread_name.find("tier xfer ")) << transfer.thread_name;
    EXPECT_FALSE(transfer.on_poller);

    // SD-card gap fill used to search the camera on a WorkThreadPool poller.
    auto gap_fill = runOn(BlockingExecutors::gapFill());
    EXPECT_EQ(0u, gap_fill.thread_name.find("gapfill ")) << gap_fill.thread_name;
    EXPECT_FALSE(gap_fill.on_poller);
}

TEST(BlockingExecutorsTest, DevicePoolRunsConfiguredNumberOfTasksInParallel) {
    const int threads = BlockingExecutors::deviceThreadCount();
    EXPECT_EQ(4, threads);
    EXPECT_GE(BlockingExecutors::apiThreadCount(), 2);
    EXPECT_EQ(4, BlockingExecutors::ffmpegThreadCount());
    // One thread on purpose: it also keeps two maintenance cycles from ever running at once.
    EXPECT_EQ(1, BlockingExecutors::storageThreadCount());
    // Matches the storage.auto_restore_max_concurrent default.
    EXPECT_EQ(5, BlockingExecutors::restoreThreadCount());
    // One thread each for the tiering cycle, backfill and health check.
    EXPECT_EQ(3, BlockingExecutors::tierThreadCount());
    // Matches the storage.tiering_transfer_max_concurrent default.
    EXPECT_EQ(4, BlockingExecutors::tierTransferThreadCount());
    EXPECT_EQ(2, BlockingExecutors::gapFillThreadCount());

    // Every thread blocks until all of them have started: this only completes if the pool really
    // runs `threads` tasks at the same time.
    std::mutex mtx;
    std::set<std::string> names;
    auto all_started = std::make_shared<std::promise<void>>();
    auto started_future = all_started->get_future().share();
    std::vector<std::future<void>> finished;
    for (int i = 0; i < threads; ++i) {
        auto done = std::make_shared<std::promise<void>>();
        finished.push_back(done->get_future());
        BlockingExecutors::device()->async([&mtx, &names, threads, all_started, started_future, done]() {
            {
                std::lock_guard<std::mutex> lck(mtx);
                names.insert(getThreadName());
                if (static_cast<int>(names.size()) == threads) {
                    all_started->set_value();
                }
            }
            started_future.wait_for(std::chrono::seconds(3));
            done->set_value();
        }, false);
    }
    for (auto &f : finished) {
        ASSERT_EQ(std::future_status::ready, f.wait_for(std::chrono::seconds(5)));
    }
    EXPECT_EQ(static_cast<size_t>(threads), names.size());
}
