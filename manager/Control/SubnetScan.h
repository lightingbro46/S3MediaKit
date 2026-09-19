#ifndef CONTROL_SUBNETSCAN_H
#define CONTROL_SUBNETSCAN_H

#include <deque>
#include <map>
#include <mutex>
#include "json/json.h"
#include "Network/Socket.h"
#include "Network/sockutil.h"
#include "Poller/Timer.h"
#include "Thread/WorkThreadPool.h"

namespace Searcher {
extern const std::string kTimeoutSec;
// Maximum IPs of one subnet scan probed at the same time (default 8).
extern const std::string kMaxParallel;
} // namespace Searcher

namespace managerkit {

struct SubnetScanOption {
    std::string startIp;
    std::string endIp;
    int port = 80;
    bool defaultPort = true;
    std::string username;
    std::string password;
};

struct DeviceScanResult {
    std::string manufacturer;
    std::string model;
    std::string firmwareVersion;
    std::string serialNumber;
    std::string hardwareId;
    std::string macAddress;
    bool isPtz = false;
    bool isAudioOutput = false;
    bool isImageFocus = false;
    bool isImageIris = false;
    bool isAudioInput = false;
    bool isRelayOutput = false;
    std::string ip;
    int port = 0;
    bool webPortAuto = true;
    std::string address;
    bool isNewDevice = true;
    struct StreamProfile {
        std::string vcodec;
        int width = 0;
        int height = 0;
        float fps = 0.0f;
        int bitrate = 0;
        std::string url;
    };
    std::vector<StreamProfile> profiles;
    std::string err_msg;
};

Json::Value toJsonValue(const DeviceScanResult &result);

class SubnetScan : public std::enable_shared_from_this<SubnetScan> {
public:
    using Ptr = std::shared_ptr<SubnetScan>;
    using onScan = std::function<void(const toolkit::SockException &, const DeviceScanResult &)>;
    // Probes one IP and calls `cb` exactly once (tests inject a fake; default: discovery_device).
    using Probe = std::function<void(const std::string &ip, const onScan &cb)>;

    struct Status {
        float progress = 0.0f;
        bool finished = false;
        std::vector<DeviceScanResult> result;
    };

    // max_parallel <= 0: config `searcher.max_parallel`.
    SubnetScan(const SubnetScanOption &option, uint64_t timeout_ms = 0, Probe probe = nullptr, int max_parallel = 0);
    ~SubnetScan();

    void setOnClose(const std::function<void()> &cb) { _onClose = std::move(cb); }
    // Called once, from a pool thread, when every IP was probed or the scan was cancelled and the
    // probes already running have returned. Set it before makeScan().
    void setOnFinished(const std::function<void()> &cb) { _onFinished = cb; }

    void makeScan(const std::string &key, const std::function<void(const toolkit::SockException &)> &cb);

    // Probes every IP of [startIp, endIp] on the `discovery` pool, at most max_parallel at a time,
    // and returns the devices found in IP order. Blocks the caller until the scan completes.
    static std::vector<DeviceScanResult> scanRange(const SubnetScanOption &option, Probe probe = nullptr, int max_parallel = 0);

    // Get scan progress (devices are listed in IP order)
    Status status() const;
    float progress() const { return status().progress; }
    bool finished() const { return status().finished; }
    std::vector<DeviceScanResult> result() const { return status().result; }
    void cancel();

public:
    static void discovery_device(std::string &address, int &port, bool &defaultPort, 
                                std::string &username, std::string &password, const onScan &cb, 
                                const toolkit::EventPoller::Ptr &poller = nullptr);

private:
    toolkit::Timer::Ptr _timer;
    toolkit::EventPoller::Ptr _poller;
    SubnetScanOption _option;
    std::function<void()> _onClose;
    std::function<void()> _onFinished;
    Probe _probe;
    int _max_parallel;
    uint64_t _created_at;
    // Guarded by _status_mtx.
    std::map<size_t, DeviceScanResult> _result; // keyed by IP index, so the result keeps IP order
    std::deque<std::pair<size_t, std::string>> _pending_ips;
    int _inflight = 0;
    bool _cancelled = false;
    bool _done = false;
    int _total_ip = 0;
    int _processed_ip = 0;
    bool _finished = false;
    float _progress = 0.0f;
    int _timeout_ms;
    mutable std::mutex _status_mtx;

private:
    // Starts probes while below _max_parallel; the last completion reports the scan finished.
    void startProbes();
    void onProbed(size_t index, const std::string &ip, const toolkit::SockException &ex, const DeviceScanResult &data);
    // create delay task to close process
    void closeAfterDelaySec();
    // Close
    bool close();
};

} // namespace managerkit

#endif // CONTROL_SUBNETSCAN_H
