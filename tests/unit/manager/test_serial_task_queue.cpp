#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Common/SerialTaskQueue.h"
#include "Thread/ThreadPool.h"

using namespace managerkit;
using namespace toolkit;

namespace {

TaskExecutor::Ptr makePool(int threads) {
    return std::make_shared<ThreadPool>(threads, ThreadPool::PRIORITY_NORMAL, true, false, "strand test");
}

// Resolves once `count` tasks have signalled.
struct Countdown {
    explicit Countdown(int count) : remaining(count) {}
    void signal() {
        if (--remaining == 0)
            done.set_value();
    }
    bool wait(int ms = 5000) {
        return done.get_future().wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
    }
    std::atomic<int> remaining;
    std::promise<void> done;
};

} // namespace

TEST(SerialTaskQueueTest, RunsTasksInOrderWithoutOverlap) {
    auto pool = makePool(4);
    auto queue = std::make_shared<SerialTaskQueue>(pool, "order");
    const int kTasks = 50;
    std::mutex mtx;
    std::vector<int> order;
    std::atomic<int> running{0};
    std::atomic<int> max_running{0};
    Countdown countdown(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        queue->post([i, &mtx, &order, &running, &max_running, &countdown]() {
            int now = ++running;
            int seen = max_running.load();
            while (now > seen && !max_running.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                std::lock_guard<std::mutex> lck(mtx);
                order.push_back(i);
            }
            --running;
            countdown.signal();
        });
    }
    ASSERT_TRUE(countdown.wait());
    EXPECT_EQ(1, max_running.load());
    ASSERT_EQ(static_cast<size_t>(kTasks), order.size());
    for (int i = 0; i < kTasks; ++i) {
        EXPECT_EQ(i, order[i]);
    }
}

TEST(SerialTaskQueueTest, ContinuesAfterThrowingTask) {
    auto pool = makePool(2);
    auto queue = std::make_shared<SerialTaskQueue>(pool, "throw");
    Countdown countdown(2);
    std::atomic<bool> ran_after{false};

    queue->post([&countdown]() {
        countdown.signal();
        throw std::runtime_error("device failure");
    });
    queue->post([]() { throw 42; });
    queue->post([&countdown, &ran_after]() {
        ran_after = true;
        countdown.signal();
    });
    ASSERT_TRUE(countdown.wait());
    EXPECT_TRUE(ran_after.load());
}

TEST(SerialTaskQueueTest, DifferentQueuesRunInParallel) {
    auto pool = makePool(2);
    auto first = std::make_shared<SerialTaskQueue>(pool, "first");
    auto second = std::make_shared<SerialTaskQueue>(pool, "second");
    // Each task waits for the other one to start: this only completes if they run concurrently.
    auto first_started = std::make_shared<std::promise<void>>();
    auto second_started = std::make_shared<std::promise<void>>();
    auto first_future = first_started->get_future().share();
    auto second_future = second_started->get_future().share();
    std::atomic<bool> first_saw{false};
    std::atomic<bool> second_saw{false};
    Countdown countdown(2);

    first->post([&, first_started, second_future]() {
        first_started->set_value();
        first_saw = second_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        countdown.signal();
    });
    second->post([&, second_started, first_future]() {
        second_started->set_value();
        second_saw = first_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        countdown.signal();
    });
    ASSERT_TRUE(countdown.wait());
    EXPECT_TRUE(first_saw.load());
    EXPECT_TRUE(second_saw.load());
}

TEST(SerialTaskQueueTest, PendingTasksKeepQueueAlive) {
    auto pool = makePool(1);
    Countdown countdown(3);
    {
        auto queue = std::make_shared<SerialTaskQueue>(pool, "alive");
        for (int i = 0; i < 3; ++i) {
            queue->post([&countdown]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                countdown.signal();
            });
        }
        // The owner drops its reference while tasks are still pending.
    }
    EXPECT_TRUE(countdown.wait());
}
