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
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Common/config.h"
#include "Control/SubnetScan.h"
#include "Util/NoticeCenter.h"
#include "server/FFmpegSource.h"
#include "server/WebApiErrCode.h"

using namespace managerkit;
using namespace toolkit;

// SubnetScan probes RTSP URLs through FFmpegProbe (server/); subnet scans pass IPs. The stub keeps
// the real contract: `cb` runs on the calling thread, before probe() returns.
std::atomic<int> g_probe_calls{0};
// Timeout of the last probe, and whether the stub plays an ffprobe that never answers (it then
// waits the whole timeout, as the kill watchdog would).
std::atomic<float> g_probe_timeout{0};
std::atomic<bool> g_probe_hangs{false};

void FFmpegProbe::probe(const std::string &play_url, float timeout_sec, const onProbe &cb) {
    ++g_probe_calls;
    g_probe_timeout = timeout_sec;
    if (g_probe_hangs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(timeout_sec * 1000)));
    }
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

// ── searcher.timeout_sec bounds the whole probe of one address ───────────────

namespace {

void setSearcherTimeout(float sec) {
    mINI::Instance()[Searcher::kTimeoutSec] = sec;
    NOTICE_EMIT(BroadcastReloadConfigArgs, mediakit::Broadcast::kBroadcastReloadConfig);
}

struct SearcherTimeoutGuard {
    explicit SearcherTimeoutGuard(float sec) { setSearcherTimeout(sec); }
    ~SearcherTimeoutGuard() {
        g_probe_hangs = false;
        setSearcherTimeout(10);
    }
};

// A TCP server on 127.0.0.1 that accepts every connection and never answers: an ONVIF camera
// that is up but stuck.
class SilentServer {
public:
    SilentServer() {
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(_fd, reinterpret_cast<sockaddr *>(&addr), &len);
        _port = ntohs(addr.sin_port);
        ::listen(_fd, 16);
        _thread = std::thread([this]() {
            for (;;) {
                int client = ::accept(_fd, nullptr, nullptr);
                if (client < 0) {
                    return;
                }
                std::lock_guard<std::mutex> lck(_mtx);
                _clients.push_back(client);
            }
        });
    }
    ~SilentServer() {
        ::shutdown(_fd, SHUT_RDWR);
        ::close(_fd);
        _thread.join();
        for (int client : _clients) {
            ::close(client);
        }
    }
    int port() const { return _port; }

private:
    int _fd = -1;
    int _port = 0;
    std::thread _thread;
    std::mutex _mtx;
    std::vector<int> _clients;
};

int64_t elapsedMs(const std::chrono::steady_clock::time_point &start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

TEST(SubnetScanTest, DiscoveryDeviceTimesOutOnSilentOnvifCamera) {
    SearcherTimeoutGuard guard(2);
    SilentServer server;

    SubnetScanOption option;
    option.defaultPort = false;
    option.port = server.port();
    option.username = "admin";
    option.password = "admin";

    bool called = false;
    const auto start = std::chrono::steady_clock::now();
    SubnetScan::discovery_device(option, "127.0.0.1",
        [&](const SockException &ex, const DeviceScanResult &data) {
            called = true;
            EXPECT_EQ(ApiErrCode::CODE_DEVICE_TIMEOUT, ex.getCustomCode());
            EXPECT_EQ("http://127.0.0.1:" + std::to_string(server.port()), data.address);
            EXPECT_NE(std::string::npos, data.err_msg.find("timeout"));
        });
    const int64_t took = elapsedMs(start);

    EXPECT_TRUE(called);
    // Without the deadline every SOAP request waits 10 s on its own.
    EXPECT_GE(took, 1500);
    EXPECT_LE(took, 2700);
}

TEST(SubnetScanTest, DiscoveryDeviceGivesFfprobeTheDeadlineMinusKillGrace) {
    SearcherTimeoutGuard guard(10);
    SubnetScanOption option;
    option.defaultPort = true;
    SubnetScan::discovery_device(option, "rtsp://10.10.0.1/stream",
        [](const SockException &ex, const DeviceScanResult &) {
            EXPECT_EQ(ApiErrCode::CODE_DEVICE_NOT_FOUND, ex.getCustomCode());
        });
    // 10 s, less the 2 s FFmpegProbe allows the kill of an ffprobe that overruns.
    EXPECT_LE(g_probe_timeout.load(), 8.0f);
    EXPECT_GE(g_probe_timeout.load(), 7.9f);
}

TEST(SubnetScanTest, DiscoveryDeviceReportsTimeoutWhenFfprobeOverruns) {
    SearcherTimeoutGuard guard(3);
    g_probe_hangs = true;
    SubnetScanOption option;
    option.defaultPort = true;

    bool called = false;
    SubnetScan::discovery_device(option, "rtsp://10.10.0.1/stream",
        [&](const SockException &ex, const DeviceScanResult &data) {
            called = true;
            EXPECT_EQ(ApiErrCode::CODE_DEVICE_TIMEOUT, ex.getCustomCode());
            EXPECT_NE(std::string::npos, data.err_msg.find("timeout"));
        });
    EXPECT_TRUE(called);
    // Never below 1 s, so a short limit can still probe something.
    EXPECT_FLOAT_EQ(1.0f, g_probe_timeout.load());
}
