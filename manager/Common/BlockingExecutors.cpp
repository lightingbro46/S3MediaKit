#include "Common/BlockingExecutors.h"

#include <algorithm>
#include <memory>
#include <thread>
#include "Thread/ThreadPool.h"
#include "Util/logger.h"
#include "Util/mini.h"
#include "Util/onceToken.h"

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
// Read for backward compatibility only; not registered, so fresh configs do not get it.
const string kLegacyApiWorkerThreads = "api.workerThreads";

static onceToken token([]() {
    mINI::Instance()[kApiThreads] = 0;
    mINI::Instance()[kDeviceThreads] = 4;
    mINI::Instance()[kDiscoveryThreads] = 8;
    mINI::Instance()[kStorageThreads] = 1;
    mINI::Instance()[kFfmpegThreads] = 4;
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

TaskExecutor::Ptr makePool(int threads, const string &name) {
    InfoL << name << " pool started with " << threads << " threads";
    return std::make_shared<ThreadPool>(threads, ThreadPool::PRIORITY_NORMAL, true, false, name);
}

} // namespace

int BlockingExecutors::apiThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kApiThreads);
        if (threads <= 0) {
            threads = readInt(Blocking::kLegacyApiWorkerThreads);
        }
        if (threads <= 0) {
            threads = max(2, static_cast<int>(thread::hardware_concurrency() / 2));
        }
        return threads;
    }();
    return s_threads;
}

int BlockingExecutors::deviceThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kDeviceThreads);
        return threads > 0 ? threads : 4;
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
        return threads > 0 ? threads : 4;
    }();
    return s_threads;
}

int BlockingExecutors::encryptionThreadCount() {
    static int s_threads = []() {
        int threads = readInt(Blocking::kEncryptionThreads);
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

TaskExecutor::Ptr BlockingExecutors::ffmpeg() {
    static TaskExecutor::Ptr s_executor = makePool(ffmpegThreadCount(), "ffmpeg worker");
    return s_executor;
}

} // namespace managerkit
