#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include "Util/onceToken.h"
#include "Util/util.h"
#include "Common/BlockingExecutors.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "server/FFmpegSource.h"
#include "server/WebApiErrCode.h"
#include "Camera/GenericRtspCamera.h"
#include "Control/OnvifDeviceControl.h"
#include "SubnetScan.h"
#include "Speaker/SpeakerController.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace Searcher {
#define SUBNETSCAN_FIELD "searcher."
const string kTimeoutSec = SUBNETSCAN_FIELD"timeout_sec";
const string kDelayCloseSec = SUBNETSCAN_FIELD"delay_close_sec";
const string kMaxParallel = SUBNETSCAN_FIELD"max_parallel";

onceToken token([]() {
    // Time limit of probing one address (discovery, or one IP of a subnet scan): 10 seconds
    mINI::Instance()[kTimeoutSec] = 10;
    // Default delay close time is 300 seconds
    mINI::Instance()[kDelayCloseSec] = 300;
    // IPs of one scan probed at the same time (on the shared `discovery` pool)
    mINI::Instance()[kMaxParallel] = 8;
});
} // namespace Searcher

namespace managerkit {

Json::Value toJsonValue(const DeviceScanResult &result) {
    Json::Value ret;
    if (!result.err_msg.empty()) {
        ret["address"] = result.address;
        ret["err_msg"] = result.err_msg;
        return ret;
    }
    ret["manufacturer"] = result.manufacturer;
    ret["model"] = result.model;
    ret["firmwareVersion"] = result.firmwareVersion;
    ret["serialNumber"] = result.serialNumber;
    ret["hardwareId"] = result.hardwareId;
    ret["macAddress"] = result.macAddress;
    ret["ip"] = result.ip;
    ret["port"] = result.port;
    ret["webPortAuto"] = result.webPortAuto;
    ret["address"] = result.address;
    ret["isPtz"] = result.isPtz;
    ret["isNewDevice"] = result.isNewDevice;
    ret["profiles"] = Json::arrayValue;
    for (const auto &it : result.profiles) {
        Json::Value stream;
        stream["vcodec"] = it.vcodec;
        stream["width"] = it.width;
        stream["height"] = it.height;
        stream["fps"] = it.fps;
        stream["bitrate"] = it.bitrate;
        stream["url"] = it.url;
        ret["profiles"].append(stream);
    }
    return ret;
}

SubnetScan::SubnetScan(const SubnetScanOption &option, uint64_t timeout_ms, Probe probe, int max_parallel)
    : _option(option), _probe(std::move(probe)), _max_parallel(max_parallel), _timeout_ms(timeout_ms) {
    _created_at = time(nullptr);
    if (_max_parallel <= 0) {
        GET_CONFIG(int, config_parallel, Searcher::kMaxParallel);
        _max_parallel = config_parallel > 0 ? config_parallel : 8;
    }
    if (!_probe) {
        SubnetScanOption opt = option;
        // startProbes() already runs this on the discovery pool, so the blocking form is called
        // directly: one hop, and _max_parallel keeps bounding the probes actually running.
        _probe = [opt](const string &ip, const onScan &cb) { discovery_device(opt, ip, cb); };
    }
}

SubnetScan::~SubnetScan() {
    DebugL;
}

void SubnetScan::discovery_device(const SubnetScanOption &option, const string &address, const onScan &cb) {
    const string &username = option.username;
    const string &password = option.password;
    const bool defaultPort = option.defaultPort;
    const auto type        = option.type;
    DeviceScanResult ret;

    // One deadline for the whole probe of this address, whichever branch it takes.
    GET_CONFIG(float, config_timeout_sec, Searcher::kTimeoutSec);
    const float timeout_sec = config_timeout_sec > 0 ? config_timeout_sec : 10;
    const auto steady_now_ms = []() -> uint64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const uint64_t deadline_ms = steady_now_ms() + static_cast<uint64_t>(timeout_sec * 1000);
    const auto remaining_ms = [&]() -> uint64_t {
        const uint64_t now = steady_now_ms();
        return now < deadline_ms ? deadline_ms - now : 0;
    };
    const string timeout_msg = StrPrinter << "Discovery timeout after " << timeout_sec << " s";
    if (start_with(address, "rtsp://")) {
        if (type != DeviceType::Camera) {
            cb(SockException(Err_other, "RTSP URL only supports camera", ApiErrCode::CODE_INVALID_ARGS), ret);
            return;
        }
        // address is rtsp url
        string url = address;
        
        if (url.find('@') == string::npos && !username.empty() && !password.empty()) {
            url = UriUtils::replaceCredentials(url, username, password);
        }

        if (!defaultPort) {
            url = UriUtils::replacePort(url, option.port);
        }

        // FFmpegProbe kills an ffprobe that overruns its timeout with process->kill(2000): leave
        // that grace inside the deadline, but always give ffprobe at least 1 s.
        static const float kKillGraceSec = 2;
        const float probe_sec = std::max(1.0f, remaining_ms() / 1000.0f - kKillGraceSec);
        const uint64_t probe_start_ms = steady_now_ms();
        // Synchronous: cb runs before probe() returns, like the ONVIF branch below.
        FFmpegProbe::probe(url, probe_sec, [&](bool success, const string &err_msg, const ProbeInfo &info) {
            if (!success) {
                ret.address = url;
                ret.err_msg = err_msg;
                // Killed by the watchdog at probe_sec (the timer may fire a few ms early).
                if (steady_now_ms() - probe_start_ms + 50 >= static_cast<uint64_t>(probe_sec * 1000)) {
                    ret.err_msg = timeout_msg;
                    cb(SockException(Err_timeout, "Device Timeout", ApiErrCode::CODE_DEVICE_TIMEOUT), ret);
                    return;
                }
                cb(SockException(Err_other, "Device Not Found", ApiErrCode::CODE_DEVICE_NOT_FOUND), ret);
            } else {
                ret.manufacturer = GENERIC_RTSP_CAMERA;
                ret.model = GENERIC_RTSP_CAMERA;
                ret.firmwareVersion = "";
                ret.serialNumber = "";
                ret.hardwareId = "";
                ret.macAddress = "";
                ret.ip = "";
                ret.port = 0;
                ret.webPortAuto = true;
                ret.address = "";
                ret.isNewDevice = true;
                // add stream profile as primary stream
                DeviceScanResult::StreamProfile stream;
                stream.vcodec = info.vcodec;
                stream.width = info.width;
                stream.height = info.height;
                stream.fps = info.fps;
                stream.bitrate = info.bitrate;
                // note: we don't replace the credentials in the url here, 
                // because the user may want to use the credentials in the url to access the stream
                // stream.url = UriUtils::replaceCredentials(url, "", "");
                stream.url = url;
                ret.profiles.push_back(stream);
                cb(SockException(Err_success), ret);
            }
        });
    } else if (isIP(address.data())) {
        if (type == DeviceType::Camera) {
            // address is ip
            const string &ip = address;
            const int port = defaultPort ? 80 : option.port;
            string ipAddress = StrPrinter << ip << ":" << port;
            auto onvif = std::make_shared<OnvifControl>(ipAddress, username, password);
            onvif->setDeadline(deadline_ms);
            const bool connected = onvif->connect();
            // A request cut by the deadline may have left the device half read (profiles or
            // stream URIs missing) even when connect() succeeded: report the timeout instead.
            if (onvif->deadlineExceeded()) {
                ret.address = "http://" + ipAddress;
                ret.err_msg = timeout_msg;
                cb(SockException(Err_timeout, "Device Timeout", ApiErrCode::CODE_DEVICE_TIMEOUT), ret);
                return;
            }
            if (!connected) {
                ret.address = "http://" + ipAddress;
                ret.err_msg = onvif->getSoapErrMsg();
                cb(SockException(Err_other, "Device Not Found", ApiErrCode::CODE_DEVICE_NOT_FOUND), ret);
                return;
            }

            auto info = onvif->getDeviceInfo();
            ret.manufacturer = info.manufacturer;
            ret.model = info.model;
            ret.firmwareVersion = info.firmwareVersion;
            ret.serialNumber = info.serialNumber;
            ret.hardwareId = info.hardwareId;
            ret.macAddress = info.macAddress;
            ret.address = "http://" + ipAddress;
            ret.ip = ip;
            ret.port = port;
            ret.webPortAuto = defaultPort;
            ret.isPtz = onvif->enablePTZ();
            ret.isAudioOutput = onvif->enableAudioOutput();
            ret.isAudioInput = onvif->enableAudioInput();
            ret.isImageFocus = onvif->enableFocus();
            ret.isImageIris = onvif->enableIris();
            ret.isRelayOutput = onvif->enableRelayOutput();
            // todo: check is new device or not
            ret.isNewDevice = true;
            auto profiles = onvif->selectStreamUrls();
            for (const auto &it : profiles) {
                DeviceScanResult::StreamProfile stream;
                stream.vcodec = it.vcodec;
                stream.width = it.width;
                stream.height = it.height;
                stream.fps = it.fps;
                stream.bitrate = it.bitrate;
                stream.url = UriUtils::replaceIp(it.url, ip);
                ret.profiles.push_back(stream);
            }
            cb(SockException(Err_success), ret);
        } else if (type == DeviceType::Speaker) {
            // address is ip
            const string &ip = address;
            const int port = defaultPort ? 80 : option.port;
            string ipAddress = StrPrinter << ip << ":" << port;

            // The vendor probe answers on an HTTP poller; wait for it here so that, like the ONVIF
            // branch, cb runs on the calling thread before this returns.
            struct ProbeResult {
                std::mutex mtx;
                std::condition_variable cv;
                bool done = false;
                SockException ex;
                IDevice::Ptr device;
            };
            auto result = std::make_shared<ProbeResult>();
            SpeakerController::probe(ip, port, username, password,
                [result](const SockException& ex, IDevice::Ptr device) {
                    std::lock_guard<std::mutex> lock(result->mtx);
                    result->ex = ex;
                    result->device = std::move(device);
                    result->done = true;
                    result->cv.notify_all();
                });
            SockException ex;
            IDevice::Ptr device;
            {
                // A probe still running after the deadline goes on in the background and is then
                // dropped: its callback only holds `result`.
                std::unique_lock<std::mutex> lock(result->mtx);
                if (!result->cv.wait_for(lock, std::chrono::milliseconds(remaining_ms()), [&result]() { return result->done; })) {
                    ret.address = "http://" + ipAddress;
                    ret.err_msg = timeout_msg;
                    lock.unlock();
                    cb(SockException(Err_timeout, "Device Timeout", ApiErrCode::CODE_DEVICE_TIMEOUT), ret);
                    return;
                }
                ex = result->ex;
                device = std::move(result->device);
            }
            if (ex) {
                ret.address = "http://" + ipAddress;
                ret.err_msg = ex.what();
                cb(SockException(Err_other, "Device Not Found", ApiErrCode::CODE_DEVICE_NOT_FOUND), ret);
                return;
            }
            auto info = device->getDeviceInfo();
            ret.manufacturer = info.manufacturer;
            ret.model = info.model;
            ret.firmwareVersion = info.firmwareVersion;
            ret.serialNumber = info.serialNumber;
            ret.hardwareId = info.hardwareId;
            ret.macAddress = info.macAddress;
            ret.address = "http://" + ipAddress;
            ret.ip = ip;
            ret.port = port;
            ret.webPortAuto = defaultPort;
            ret.isPtz = false;
            ret.isAudioOutput = false;
            ret.isAudioInput = false;
            ret.isImageFocus = false;
            ret.isImageIris = false;
            ret.isRelayOutput = false;
            ret.isNewDevice = true;
            cb(SockException(Err_success), ret);
        } else {
            cb(SockException(Err_other, "Device is not supported", ApiErrCode::CODE_INVALID_ARGS), ret);
        }
    } else {
        cb(SockException(Err_other, "Address must be ip or rtsp url", ApiErrCode::CODE_INVALID_ARGS), ret);
    }
}

void SubnetScan::makeScan(const std::string &key, const std::function<void(const SockException &)> &cb) {
    auto ip_range = SockUtil::get_ipv4_range(_option.startIp, _option.endIp);
    if (ip_range.empty()) {
        cb(SockException(Err_other, "Invalid IP range", ApiErrCode::CODE_INVALID_IP_RANGE));
        return;
    }
    {
        lock_guard<mutex> lock(_status_mtx);
        _total_ip = ip_range.size();
        _processed_ip = 0;
        _finished = false;
        _cancelled = false;
        _done = false;
        _progress = 0.0f;
        _result.clear();
        _pending_ips.clear();
        for (size_t i = 0; i < ip_range.size(); ++i) {
            _pending_ips.emplace_back(i, ip_range[i]);
        }
    }

    // Only the close timer runs on this poller; probes run on the `discovery` pool.
    _poller = WorkThreadPool::Instance().getPoller();
    startProbes();
    cb(SockException(Err_success));
}

void SubnetScan::startProbes() {
    std::vector<std::pair<size_t, std::string>> to_start;
    {
        lock_guard<mutex> lock(_status_mtx);
        while (!_cancelled && _inflight < _max_parallel && !_pending_ips.empty()) {
            to_start.push_back(_pending_ips.front());
            _pending_ips.pop_front();
            ++_inflight;
        }
    }
    weak_ptr<SubnetScan> weak_self = shared_from_this();
    for (const auto &item : to_start) {
        const size_t index = item.first;
        const string ip = item.second;
        BlockingExecutors::discovery()->async([weak_self, index, ip]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            // This is the blocking thread the probe needs: discovery_device answers synchronously
            // for every address kind, and the callback runs exactly once.
            strong_self->_probe(ip, [weak_self, index, ip](const SockException &ex, const DeviceScanResult &data) {
                auto strong_self = weak_self.lock();
                if (strong_self) {
                    strong_self->onProbed(index, ip, ex, data);
                }
            });
        }, false);
    }
}

void SubnetScan::onProbed(size_t index, const std::string &ip, const SockException &ex, const DeviceScanResult &data) {
    if (!ex) {
        DebugL << "Found device at ip: " << ip;
    } else {
        DebugL << "Scan device at ip " << ip << " failed: " << ex.what();
    }
    bool all_done = false;
    {
        lock_guard<mutex> lock(_status_mtx);
        --_inflight;
        if (!ex) {
            _result[index] = data;
        }
        _processed_ip++;
        _progress = (static_cast<float>(_processed_ip) / _total_ip) * 100.0f;
        if (!_finished) {
            _finished = (_processed_ip == _total_ip);
        }
        if (!_done && _inflight == 0 && (_pending_ips.empty() || _cancelled)) {
            _done = true;
            all_done = true;
        }
    }
    if (!all_done) {
        startProbes();
        return;
    }
    if (_onFinished) {
        _onFinished();
    }
    if (_poller) {
        closeAfterDelaySec();
    }
}

std::vector<DeviceScanResult> SubnetScan::scanRange(const SubnetScanOption &option, Probe probe, int max_parallel) {
    auto scanner = std::make_shared<SubnetScan>(option, 0, std::move(probe), max_parallel);
    auto finished = std::make_shared<std::promise<void>>();
    auto finished_future = finished->get_future();
    scanner->setOnFinished([finished]() { finished->set_value(); });
    auto ip_range = SockUtil::get_ipv4_range(option.startIp, option.endIp);
    if (ip_range.empty()) {
        return {};
    }
    {
        lock_guard<mutex> lock(scanner->_status_mtx);
        scanner->_total_ip = ip_range.size();
        for (size_t i = 0; i < ip_range.size(); ++i) {
            scanner->_pending_ips.emplace_back(i, ip_range[i]);
        }
    }
    // No _poller: nothing to close, the scanner dies with this call.
    scanner->startProbes();
    finished_future.wait();
    return scanner->result();
}

SubnetScan::Status SubnetScan::status() const {
    lock_guard<mutex> lock(_status_mtx);
    Status ret;
    ret.progress = _progress;
    ret.finished = _finished;
    for (const auto &item : _result) {
        ret.result.push_back(item.second);
    }
    return ret;
}

void SubnetScan::cancel() {
    {
        lock_guard<mutex> lock(_status_mtx);
        // IPs not started yet are dropped; probes already running finish on their own.
        _finished = true;
        _cancelled = true;
        _pending_ips.clear();
    }
    close();
}

void SubnetScan::closeAfterDelaySec() {
    GET_CONFIG(int, delay_close_sec, Searcher::kDelayCloseSec);
    weak_ptr<SubnetScan> weakSelf = shared_from_this();
    _poller->doDelayTask(
        (uint64_t)(delay_close_sec * 1000),
        [weakSelf]() {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                // Self has been destroyed
                return 0;
            }
            strongSelf->close();
            return 0;
        }
    );
}

bool SubnetScan::close() {
    if (_onClose) {
        _onClose();
    }
    return true;
}

} // namespace managerkit