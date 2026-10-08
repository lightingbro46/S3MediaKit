#include "Common/BlockingExecutors.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "Thread/ThreadPool.h"
#include "Util/logger.h"
#include "Util/mini.h"
#include "Util/onceToken.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

namespace Blocking {
const string kApiThreads = "blocking.apiThreads";
const string kDeviceThreads = "blocking.deviceThreads";
const string kDiscoveryThreads = "blocking.discoveryThreads";
const string kStorageThreads = "blocking.storageThreads";
const string kFfmpegThreads = "blocking.ffmpegThreads";
const string kEncryptionThreads = "blocking.encryptionThreads";
const string kRestoreThreads = "blocking.restoreThreads";
const string kTierThreads = "blocking.tierThreads";
const string kTierTransferThreads = "blocking.tierTransferThreads";
const string kGapFillThreads = "blocking.gapFillThreads";
// Read for backward compatibility only; not registered, so fresh configs do not get it.
const string kLegacyApiWorkerThreads = "api.workerThreads";

static onceToken token([]() {
    mINI::Instance()[kApiThreads] = 0;
    mINI::Instance()[kDeviceThreads] = 32;
    mINI::Instance()[kDiscoveryThreads] = 8;
    mINI::Instance()[kStorageThreads] = 1;
    mINI::Instance()[kFfmpegThreads] = 8;
    mINI::Instance()[kRestoreThreads] = 8;
    mINI::Instance()[kTierThreads] = 3;
    mINI::Instance()[kTierTransferThreads] = 8;
    mINI::Instance()[kGapFillThreads] = 2;
});
} // namespace Blocking

namespace {

int readInt(const string &key) {
    auto &ini = mINI::Instance();
    auto it = ini.find(key);
    if (it == ini.end() || it->second.empty()) {
        return 0;
    }
    return it->second.as<int>();
}

// Per-pool counters: how long tasks waited in the queue and how long they ran. ThreadLoadCounter
// is one counter shared by every thread of a pool, so its load() means nothing for these pools.
struct PoolCounters {
    std::atomic<int> running { 0 };
    std::atomic<uint64_t> completed { 0 };
    // Since the last stats() call.
    std::atomic<uint64_t> window_tasks { 0 };
    std::atomic<uint64_t> window_wait_us { 0 };
    std::atomic<uint64_t> window_wait_max_us { 0 };
    std::atomic<uint64_t> window_busy_us { 0 };
    std::atomic<uint64_t> window_start_us { 0 };
};

class MeteredPool : public ThreadPool {
public:
    MeteredPool(int threads, const string &name)
        : ThreadPool(threads, ThreadPool::PRIORITY_NORMAL, true, false, name)
        , _counters(std::make_shared<PoolCounters>()) {
        _counters->window_start_us = getCurrentMicrosecond(true);
    }

    Task::Ptr async(TaskIn task, bool may_sync = true) override {
        return ThreadPool::async(wrap(std::move(task)), may_sync);
    }

    Task::Ptr async_first(TaskIn task, bool may_sync = true) override {
        return ThreadPool::async_first(wrap(std::move(task)), may_sync);
    }

    const std::shared_ptr<PoolCounters> &counters() const { return _counters; }

private:
    TaskIn wrap(TaskIn task) {
        // The counters, not the pool: a task can outlive a pool torn down at exit.
        auto counters = _counters;
        const uint64_t queued_at = getCurrentMicrosecond(true);
        return [counters, queued_at, task]() {
            const uint64_t started = getCurrentMicrosecond(true);
            const uint64_t wait = started > queued_at ? started - queued_at : 0;
            counters->window_wait_us += wait;
            uint64_t max_wait = counters->window_wait_max_us.load();
            while (wait > max_wait && !counters->window_wait_max_us.compare_exchange_weak(max_wait, wait)) {
            }
            ++counters->running;
            onceToken token(nullptr, [&]() {
                --counters->running;
                ++counters->completed;
                ++counters->window_tasks;
                const uint64_t now = getCurrentMicrosecond(true);
                counters->window_busy_us += now > started ? now - started : 0;
            });
            task();
        };
    }

    std::shared_ptr<PoolCounters> _counters;
};

struct PoolEntry {
    string name;
    int threads;
    std::weak_ptr<MeteredPool> pool;
};

std::mutex s_pools_mtx;
std::vector<PoolEntry> s_pools;

TaskExecutor::Ptr makePool(int threads, const string &name) {
    InfoL << name << " pool started with " << threads << " threads";
    auto pool = std::make_shared<MeteredPool>(threads, name);
    std::lock_guard<std::mutex> lock(s_pools_mtx);
    s_pools.push_back(PoolEntry { name, threads, pool });
    return pool;
}

} // namespace

int BlockingExecutors::apiThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kApiThreads);
        if (threads <= 0) {
            threads = readInt(Blocking::kLegacyApiWorkerThreads);
        }
        if (threads <= 0) {
            // Not tied to the core count: the work is mostly SQLite, whose single writer gains
            // nothing from more threads, and 600 ESC queues plus REST must fit on a small box too.
            threads = 16;
        }
        return threads;
    }();
    return s_threads;
}

int BlockingExecutors::deviceThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kDeviceThreads);
        return threads > 0 ? threads : 32;
    }();
    return s_threads;
}

int BlockingExecutors::discoveryThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kDiscoveryThreads);
        return threads > 0 ? threads : 8;
    }();
    return s_threads;
}

int BlockingExecutors::storageThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kStorageThreads);
        return threads > 0 ? threads : 1;
    }();
    return s_threads;
}

int BlockingExecutors::ffmpegThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kFfmpegThreads);
        return threads > 0 ? threads : 8;
    }();
    return s_threads;
}

int BlockingExecutors::encryptionThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kEncryptionThreads);
        return threads > 0 ? threads : 4;
    }();
    return s_threads;
}

int BlockingExecutors::restoreThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kRestoreThreads);
        return threads > 0 ? threads : 8;
    }();
    return s_threads;
}

int BlockingExecutors::tierThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kTierThreads);
        return threads > 0 ? threads : 3;
    }();
    return s_threads;
}

int BlockingExecutors::tierTransferThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kTierTransferThreads);
        return threads > 0 ? threads : 8;
    }();
    return s_threads;
}

int BlockingExecutors::gapFillThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kGapFillThreads);
        return threads > 0 ? threads : 2;
    }();
    return s_threads;
}

TaskExecutor::Ptr BlockingExecutors::api() {
    static TaskExecutor::Ptr s_executor = makePool(apiThreadCount(), "api worker");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::device() {
    static TaskExecutor::Ptr s_executor = makePool(deviceThreadCount(), "device worker");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::discovery() {
    static TaskExecutor::Ptr s_executor = makePool(discoveryThreadCount(), "discovery");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::storage() {
    static TaskExecutor::Ptr s_executor = makePool(storageThreadCount(), "storage worker");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::encryption() {
    static TaskExecutor::Ptr s_executor = makePool(encryptionThreadCount(), "encryption worker");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::restore() {
    static TaskExecutor::Ptr s_executor = makePool(restoreThreadCount(), "restore");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::tier() {
    static TaskExecutor::Ptr s_executor = makePool(tierThreadCount(), "tier");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::tierTransfer() {
    static TaskExecutor::Ptr s_executor = makePool(tierTransferThreadCount(), "tier xfer");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::gapFill() {
    static TaskExecutor::Ptr s_executor = makePool(gapFillThreadCount(), "gapfill");
    return s_executor;
}

TaskExecutor::Ptr BlockingExecutors::ffmpeg() {
    static TaskExecutor::Ptr s_executor = makePool(ffmpegThreadCount(), "ffmpeg worker");
    return s_executor;
}

std::vector<BlockingExecutors::PoolStats> BlockingExecutors::stats() {
    std::vector<PoolStats> ret;
    const uint64_t now = getCurrentMicrosecond(true);
    std::lock_guard<std::mutex> lock(s_pools_mtx);
    for (const auto &entry : s_pools) {
        auto pool = entry.pool.lock();
        if (!pool) {
            continue;
        }
        auto &c = pool->counters();
        PoolStats st;
        st.name = entry.name;
        st.threads = entry.threads;
        st.queued = pool->size();
        st.running = c->running.load();
        st.completed = c->completed.load();
        const uint64_t tasks = c->window_tasks.exchange(0);
        const uint64_t wait = c->window_wait_us.exchange(0);
        st.window_tasks = tasks;
        st.window_wait_avg_ms = tasks ? wait / tasks / 1000.0 : 0;
        st.window_wait_max_ms = c->window_wait_max_us.exchange(0) / 1000.0;
        const uint64_t busy = c->window_busy_us.exchange(0);
        const uint64_t start = c->window_start_us.exchange(now);
        const uint64_t span = now > start ? now - start : 0;
        // Share of the pool's thread-time spent running tasks over the window.
        st.window_busy_pct = span && entry.threads > 0 ? 100.0 * busy / (double(span) * entry.threads) : 0;
        ret.push_back(st);
    }
    return ret;
}

} // namespace managerkit
