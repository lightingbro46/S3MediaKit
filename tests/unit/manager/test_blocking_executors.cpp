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
}

TEST(BlockingExecutorsTest, DevicePoolRunsConfiguredNumberOfTasksInParallel) {
    const int threads = BlockingExecutors::deviceThreadCount();
    EXPECT_EQ(4, threads);
    EXPECT_GE(BlockingExecutors::apiThreadCount(), 2);
    EXPECT_EQ(4, BlockingExecutors::ffmpegThreadCount());
    // One thread on purpose: it also keeps two maintenance cycles from ever running at once.
    EXPECT_EQ(1, BlockingExecutors::storageThreadCount());

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
