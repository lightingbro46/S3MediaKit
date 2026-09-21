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

// V4: an interactive device command (PTZ) must not wait behind the maintenance work already queued
// for the same camera.
TEST(SerialTaskQueueTest, PostFrontRunsBeforeQueuedTasks) {
    auto pool = makePool(2);
    auto queue = std::make_shared<SerialTaskQueue>(pool, "front");
    auto gate = std::make_shared<std::promise<void>>();
    auto gate_future = gate->get_future().share();
    std::mutex mtx;
    std::vector<std::string> order;
    Countdown countdown(5); // the blocker, 3 background tasks and the priority one

    // Occupies the queue so everything below is really waiting in the deque. Waiting for it to have
    // *started* matters: until it is dequeued it sits in the deque itself, where postFront would
    // jump ahead of it too.
    auto started = std::make_shared<std::promise<void>>();
    queue->post([&, gate_future, started]() {
        started->set_value();
        gate_future.wait();
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("blocker");
        }
        countdown.signal();
    });
    ASSERT_EQ(std::future_status::ready, started->get_future().wait_for(std::chrono::seconds(5)));
    for (int i = 0; i < 3; ++i) {
        queue->post([&, i]() {
            {
                std::lock_guard<std::mutex> lck(mtx);
                order.push_back("background" + std::to_string(i));
            }
            countdown.signal();
        });
    }
    queue->postFront([&]() {
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("priority");
        }
        countdown.signal();
    });
    gate->set_value();
    ASSERT_TRUE(countdown.wait());

    std::lock_guard<std::mutex> lck(mtx);
    ASSERT_EQ(5u, order.size());
    // The task already running is never interrupted; the priority one goes right after it, and the
    // background tasks keep their own order behind it.
    EXPECT_EQ("blocker", order[0]);
    EXPECT_EQ("priority", order[1]);
    EXPECT_EQ("background0", order[2]);
    EXPECT_EQ("background1", order[3]);
    EXPECT_EQ("background2", order[4]);
}

// The real bottleneck is the FIFO queue of the shared pool: 300 cameras may each have a maintenance
// turn waiting there, so jumping only the camera's own deque would not help. The interactive queue
// must also be handed to the pool with async_first.
TEST(SerialTaskQueueTest, PostFrontJumpsTheSharedPoolQueue) {
    auto pool = makePool(1); // one thread, so the pool queue order is observable
    auto blocking = std::make_shared<SerialTaskQueue>(pool, "blocking");
    auto background = std::make_shared<SerialTaskQueue>(pool, "background");
    auto interactive = std::make_shared<SerialTaskQueue>(pool, "interactive");
    auto gate = std::make_shared<std::promise<void>>();
    auto gate_future = gate->get_future().share();
    std::mutex mtx;
    std::vector<std::string> order;
    Countdown countdown(3);

    // Holds the single pool thread while the two turns below line up behind it in the POOL queue.
    // It has to be running already: a turn still queued on the pool would be jumped as well.
    auto started = std::make_shared<std::promise<void>>();
    blocking->post([&, gate_future, started]() {
        started->set_value();
        gate_future.wait();
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("blocker");
        }
        countdown.signal();
    });
    ASSERT_EQ(std::future_status::ready, started->get_future().wait_for(std::chrono::seconds(5)));
    // Another camera's maintenance turn, queued on the pool with a plain async.
    background->post([&]() {
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("background");
        }
        countdown.signal();
    });
    // A third camera's interactive command: async_first puts its turn ahead of the one above.
    interactive->postFront([&]() {
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("interactive");
        }
        countdown.signal();
    });
    gate->set_value();
    ASSERT_TRUE(countdown.wait());

    std::lock_guard<std::mutex> lck(mtx);
    ASSERT_EQ(3u, order.size());
    EXPECT_EQ("blocker", order[0]);
    EXPECT_EQ("interactive", order[1]);
    EXPECT_EQ("background", order[2]);
}

// Regression guard: postFront must not turn a burst of interactive commands into LIFO. Two PTZ
// commands a user sent in a row have to reach the camera in that order — reversing them leaves the
// camera wherever the older command pointed.
TEST(SerialTaskQueueTest, PriorityTasksKeepTheirOwnOrder) {
    auto pool = makePool(2);
    auto queue = std::make_shared<SerialTaskQueue>(pool, "priority order");
    auto gate = std::make_shared<std::promise<void>>();
    auto gate_future = gate->get_future().share();
    std::mutex mtx;
    std::vector<std::string> order;
    Countdown countdown(5); // the blocker, 1 background task and 3 priority ones

    auto started = std::make_shared<std::promise<void>>();
    queue->post([&, gate_future, started]() {
        started->set_value();
        gate_future.wait();
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("blocker");
        }
        countdown.signal();
    });
    ASSERT_EQ(std::future_status::ready, started->get_future().wait_for(std::chrono::seconds(5)));

    queue->post([&]() {
        {
            std::lock_guard<std::mutex> lck(mtx);
            order.push_back("background");
        }
        countdown.signal();
    });
    for (int i = 0; i < 3; ++i) {
        queue->postFront([&, i]() {
            {
                std::lock_guard<std::mutex> lck(mtx);
                order.push_back("priority" + std::to_string(i));
            }
            countdown.signal();
        });
    }
    gate->set_value();
    ASSERT_TRUE(countdown.wait());

    std::lock_guard<std::mutex> lck(mtx);
    ASSERT_EQ(5u, order.size());
    EXPECT_EQ("blocker", order[0]);
    EXPECT_EQ("priority0", order[1]);
    EXPECT_EQ("priority1", order[2]);
    EXPECT_EQ("priority2", order[3]);
    EXPECT_EQ("background", order[4]);
}
