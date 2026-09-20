#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "Control/SubnetScan.h"
#include "server/FFmpegSource.h"
#include "server/WebApiErrCode.h"

using namespace managerkit;
using namespace toolkit;

// SubnetScan probes RTSP URLs through FFmpegProbe (server/); subnet scans pass IPs. The stub keeps
// the real contract: `cb` runs on the calling thread, before probe() returns.
std::atomic<int> g_probe_calls{0};

void FFmpegProbe::probe(const std::string &play_url, float, const onProbe &cb) {
    ++g_probe_calls;
    ProbeInfo info;
    info.url = play_url;
    cb(false, "not available in unit test", info);
}

void FFmpegProbe::makeProbe(const std::string &play_url, float timeout_sec, const onProbe &cb,
                            const TaskExecutor::Ptr &executor) {
    auto _executor = executor ? executor : WorkThreadPool::Instance().getPoller();
    _executor->async([play_url, timeout_sec, cb]() { FFmpegProbe::probe(play_url, timeout_sec, cb); });
}

namespace {

SubnetScanOption rangeOption(int last_octet) {
    SubnetScanOption option;
    option.startIp = "10.10.0.1";
    option.endIp = "10.10.0." + std::to_string(last_octet);
    return option;
}

int lastOctet(const std::string &ip) {
    return std::stoi(ip.substr(ip.rfind('.') + 1));
}

// Fake probe: records concurrency, finds a device on every even IP.
struct ProbeRecorder {
    std::mutex mtx;
    std::condition_variable cv;
    std::set<std::string> probed;
    int running = 0;
    int max_running = 0;
    bool blocked = false;
    int sleep_ms = 10;

    SubnetScan::Probe probe() {
        auto self = this;
        return [self](const std::string &ip, const SubnetScan::onScan &cb) {
            {
                std::unique_lock<std::mutex> lck(self->mtx);
                self->probed.insert(ip);
                ++self->running;
                self->max_running = std::max(self->max_running, self->running);
                self->cv.notify_all();
                self->cv.wait(lck, [self]() { return !self->blocked; });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(self->sleep_ms));
            {
                std::lock_guard<std::mutex> lck(self->mtx);
                --self->running;
            }
            DeviceScanResult result;
            result.ip = ip;
            if (lastOctet(ip) % 2 == 0) {
                cb(SockException(Err_success), result);
            } else {
                cb(SockException(Err_other, "Device Not Found", ApiErrCode::CODE_DEVICE_NOT_FOUND), result);
            }
        };
    }

    bool waitRunning(int count, int ms = 3000) {
        std::unique_lock<std::mutex> lck(mtx);
        return cv.wait_for(lck, std::chrono::milliseconds(ms), [this, count]() { return running >= count; });
    }

    void release() {
        std::lock_guard<std::mutex> lck(mtx);
        blocked = false;
        cv.notify_all();
    }
};

} // namespace

TEST(SubnetScanTest, ScansAllIpsWithBoundedParallelism) {
    ProbeRecorder recorder;
    auto scanner = std::make_shared<SubnetScan>(rangeOption(20), 0, recorder.probe(), 3);
    auto finished = std::make_shared<std::promise<void>>();
    auto finished_future = finished->get_future();
    scanner->setOnFinished([finished]() { finished->set_value(); });

    bool accepted = false;
    scanner->makeScan("key", [&accepted](const SockException &ex) { accepted = !ex; });
    EXPECT_TRUE(accepted);
    ASSERT_EQ(std::future_status::ready, finished_future.wait_for(std::chrono::seconds(10)));

    auto status = scanner->status();
    EXPECT_TRUE(status.finished);
    EXPECT_FLOAT_EQ(100.0f, status.progress);
    EXPECT_EQ(20u, recorder.probed.size());
    EXPECT_LE(recorder.max_running, 3);
    EXPECT_GE(recorder.max_running, 2);
    ASSERT_EQ(10u, status.result.size());
    for (size_t i = 0; i < status.result.size(); ++i) {
        EXPECT_EQ(static_cast<int>(2 * (i + 1)), lastOctet(status.result[i].ip));
    }
}

TEST(SubnetScanTest, CancelSkipsIpsNotStarted) {
    ProbeRecorder recorder;
    recorder.blocked = true;
    auto scanner = std::make_shared<SubnetScan>(rangeOption(20), 0, recorder.probe(), 2);
    auto finished = std::make_shared<std::promise<void>>();
    auto finished_future = finished->get_future();
    scanner->setOnFinished([finished]() { finished->set_value(); });
    std::atomic<bool> closed{false};
    scanner->setOnClose([&closed]() { closed = true; });

    scanner->makeScan("key", [](const SockException &) {});
    ASSERT_TRUE(recorder.waitRunning(2));
    scanner->cancel();
    EXPECT_TRUE(closed.load());
    EXPECT_TRUE(scanner->finished());
    recorder.release();

    ASSERT_EQ(std::future_status::ready, finished_future.wait_for(std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(2u, recorder.probed.size());
    EXPECT_LT(scanner->progress(), 100.0f);
}

TEST(SubnetScanTest, ProgressIncreasesWhileScanning) {
    ProbeRecorder recorder;
    recorder.sleep_ms = 30;
    auto scanner = std::make_shared<SubnetScan>(rangeOption(12), 0, recorder.probe(), 2);
    auto finished = std::make_shared<std::promise<void>>();
    auto finished_future = finished->get_future();
    scanner->setOnFinished([finished]() { finished->set_value(); });

    scanner->makeScan("key", [](const SockException &) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    float mid = scanner->progress();
    EXPECT_GT(mid, 0.0f);
    EXPECT_LT(mid, 100.0f);
    EXPECT_FALSE(scanner->finished());
    ASSERT_EQ(std::future_status::ready, finished_future.wait_for(std::chrono::seconds(10)));
    EXPECT_FLOAT_EQ(100.0f, scanner->progress());
}

TEST(SubnetScanTest, ScanRangeBlocksAndKeepsIpOrder) {
    ProbeRecorder recorder;
    auto devices = SubnetScan::scanRange(rangeOption(9), recorder.probe(), 4);
    EXPECT_EQ(9u, recorder.probed.size());
    EXPECT_LE(recorder.max_running, 4);
    ASSERT_EQ(4u, devices.size());
    EXPECT_EQ(2, lastOctet(devices[0].ip));
    EXPECT_EQ(4, lastOctet(devices[1].ip));
    EXPECT_EQ(6, lastOctet(devices[2].ip));
    EXPECT_EQ(8, lastOctet(devices[3].ip));
}

// The handler of /media/mserver/device/discovery must not branch on the address to pick a thread:
// discovery_device answers on the calling thread for every address kind.
TEST(SubnetScanTest, DiscoveryDeviceAnswersBeforeReturningForRtspUrl) {
    SubnetScanOption option;
    option.defaultPort = true;

    const int calls_before = g_probe_calls.load();
    bool called = false;
    bool called_before_return = false;
    std::thread::id cb_thread;
    SubnetScan::discovery_device(option, "rtsp://10.10.0.1/stream",
        [&](const SockException &ex, const DeviceScanResult &data) {
            called = true;
            cb_thread = std::this_thread::get_id();
            EXPECT_TRUE(static_cast<bool>(ex));
            EXPECT_EQ(ApiErrCode::CODE_DEVICE_NOT_FOUND, ex.getCustomCode());
            EXPECT_FALSE(data.err_msg.empty());
        });
    called_before_return = called;

    EXPECT_TRUE(called_before_return);
    EXPECT_EQ(std::this_thread::get_id(), cb_thread);
    EXPECT_EQ(calls_before + 1, g_probe_calls.load());
}

TEST(SubnetScanTest, DiscoveryDeviceRejectsAddressThatIsNeitherIpNorRtsp) {
    SubnetScanOption option;
    bool called = false;
    SubnetScan::discovery_device(option, "not-an-address",
        [&](const SockException &ex, const DeviceScanResult &) {
            called = true;
            EXPECT_EQ(ApiErrCode::CODE_INVALID_ARGS, ex.getCustomCode());
        });
    EXPECT_TRUE(called);
}
