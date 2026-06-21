#ifndef CONTROL_SUBNETSCAN_H
#define CONTROL_SUBNETSCAN_H

#include "Network/sockutil.h"
#include "Thread/WorkThreadPool.h"

namespace Searcher {
extern const std::string kTimeoutSec;
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

    SubnetScan(const SubnetScanOption &option, uint64_t timeout_ms = 0);
    ~SubnetScan();

    void setOnClose(const std::function<void()> &cb) { _onClose = std::move(cb); }

    void makeScan(const std::string &key, const std::function<void(const toolkit::SockException &)> &cb);

    // Get scan progress
    const float& progress() const { return _progress; }
    const bool& finished() const { return _finished; }
    const std::vector<DeviceScanResult>& result() const { return _result; }

public:
    static void discovery_device(std::string &address, int &port, bool &defaultPort, 
                                std::string &username, std::string &password, const onScan &cb, 
                                const toolkit::EventPoller::Ptr &poller = nullptr);

private:
    toolkit::Timer::Ptr _timer;
    toolkit::EventPoller::Ptr _poller;
    SubnetScanOption _option;
    std::function<void()> _onClose;
    uint64_t _created_at;
    std::vector<DeviceScanResult> _result;
    int _total_ip = 0;
    int _processed_ip = 0;
    bool _finished = false;
    float _progress = 0.0f;
    int _timeout_ms;

private:
    // create delay task to close process
    void closeAfterDelaySec();
    // Close
    bool close();
};

} // namespace managerkit

#endif // CONTROL_SUBNETSCAN_H