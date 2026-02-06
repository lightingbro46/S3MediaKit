#include "Util/onceToken.h"
#include "Util/util.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "server/FFmpegSource.h"
#include "server/WebApiErrCode.h"
#include "Camera/GenericRtspCamera.h"
#include "ext-plugin/onvif.h"
#include "SubnetScan.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace Searcher {
#define SUBNETSCAN_FIELD "searcher."
const string kTimeoutSec = SUBNETSCAN_FIELD"timeout_sec";
const string kDelayCloseSec = SUBNETSCAN_FIELD"delay_close_sec";

onceToken token([]() {
    // Default subnet scan timeout is 10 seconds
    mINI::Instance()[kTimeoutSec] = 10;
    // Default delay close time is 300 seconds
    mINI::Instance()[kDelayCloseSec] = 300;
});
} // namespace Searcher

namespace managerkit {

Json::Value toJsonValue(const DeviceScanResult &result) {
    Json::Value ret;
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

SubnetScan::SubnetScan(const SubnetScanOption &option, uint64_t timeout_ms)
    : _option(option), _timeout_ms(timeout_ms) {
    _created_at = time(nullptr);
}

SubnetScan::~SubnetScan() {
    DebugL;
}

void SubnetScan::discovery_device(string &address, int &port, bool &defaultPort, 
                                string &username, string &password, const onScan &cb, 
                                const toolkit::EventPoller::Ptr &poller) {
    DeviceScanResult ret;
    if (start_with(address, "rtsp://")) {
        // address is rtsp url
        string url = address;
        
        if (url.find('@') == string::npos && !username.empty() && !password.empty()) {
            url = UriUtils::replaceCredentials(url, username, password);
        }

        if (!defaultPort) {
            url = UriUtils::replacePort(url, port);
        }

        FFmpegProbe::makeProbe(url, 10, [=](bool success, const string &err_msg, const ProbeInfo &info) mutable {
            if (!success) {
                cb(SockException(Err_other, "Device Not Found", ApiErrCode::CODE_DEVICE_NOT_FOUND), ret);
            } else {
                ret.manufacturer = GENERIC_RTSP_CAMERA;
                ret.model = GENERIC_RTSP_CAMERA;
                ret.firmwareVersion = "";
                ret.serialNumber = "";
                ret.hardwareId = "";
                ret.macAddress = "";
                ret.isPtz = false;
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
                stream.url = UriUtils::replaceCredentials(url, "", "");
                ret.profiles.push_back(stream);
                cb(SockException(Err_success), ret);
            }
        }, poller);
    } else if (isIP(address.data())) {
        // address is ip
        string ip = address;
        if (defaultPort) {
            port = 80;
        }
        string ipAddress = StrPrinter << ip << ":" << port;
        auto onvif = std::make_shared<OnvifController>(ipAddress, username, password);
        if (!onvif->initControl()) {
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
        ret.address = "http:// " + ipAddress; 
        ret.ip = ip;
        ret.port = port;
        ret.webPortAuto = defaultPort;
        ret.isPtz = onvif->enablePTZ();
        //todo: check is new device or not
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
    } else {
        cb(SockException(Err_other, "Address must be ip or rtsp url", ApiErrCode::CODE_INVALID_ARGS), ret);
    }
}

void SubnetScan::makeScan(const std::string &key, const std::function<void(const SockException &)> &cb) {
    auto ip_range = SockUtil::get_ipv4_range(_option.startIp, _option.endIp);
    _total_ip = ip_range.size();

    if (_total_ip == 0) {
        cb(SockException(Err_other, "Invalid IP range", ApiErrCode::CODE_INVALID_IP_RANGE));
        return;
    }

    _processed_ip = 0;
    _finished = false;

    weak_ptr<SubnetScan> weak_self = shared_from_this();

    _poller = WorkThreadPool::Instance().getPoller();    
    _poller->async([weak_self, ip_range, cb]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto option = strong_self->_option;
        for (auto &ip : ip_range) {
            if (strong_self->_finished) {
                break;
            }
            strong_self->discovery_device(const_cast<std::string&>(ip),
                                        option.port,
                                        option.defaultPort,
                                        option.username,
                                        option.password,
                                        [weak_self, ip](const SockException &ex, const DeviceScanResult &data) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                if (!ex) {
                    strong_self->_result.push_back(std::move(data));
                    DebugL << "Found device at ip: " << ip;
                } else {
                    DebugL << "Scan device at ip " << ip << " failed: " << ex.what();
                }
                strong_self->_processed_ip++;
                strong_self->_progress = (static_cast<float>(strong_self->_processed_ip) / strong_self->_total_ip) * 100.0f;
                if (!strong_self->_finished) {
                    strong_self->_finished = (strong_self->_processed_ip == strong_self->_total_ip);
                }
            }, strong_self->_poller);
        }
        strong_self->closeAfterDelaySec();
    });

    cb(SockException(Err_success));
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