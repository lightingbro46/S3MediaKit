#include "GenericRtspCameraImp.h"
#include "server/WebApiErrCode.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const DeviceTuple& tuple, const std::unordered_map<int, StreamTuple>& stream_map, const CameraStatisticImp::Ptr& statistic)
    : _statistic(statistic) {
    _poller = EventPollerPool::Instance().getPoller();
    CHECK(_poller != nullptr, "Failed to get poller for GenericRtspCameraImp");
    _src = std::make_shared<GenericRtspCamera>(tuple, stream_map);
    statistic->setDeviceTuple(tuple);
    statistic->setStreamTuples(stream_map);
}

GenericRtspCameraImp::~GenericRtspCameraImp() {
    _exit = true;
    // stop camera if it's still running when destructing.
    // NOTE: stop() must run on the owner poller (stopMonitor/stopController assert isCurrentThread).
    // We must NOT use async([this]) here because 'this' becomes a dangling pointer after the
    // destructor returns, causing use-after-free. Use sync() so 'this' stays valid until stop() completes.
    if (_enabled.load()) {
        if (_poller->isCurrentThread()) {
            stop();
        } else {
            _poller->sync([this]() { stop(); });
        }
    }
}

void GenericRtspCameraImp::setCameraOption(const CameraOption& option) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setCameraOption in it's owner poller");
    if (_option == option) {
        return; // No change
    }
    _option = option;
    _enabled = option.enableActive;
    saveCameraOption(option);
    
    if (!_enabled.load()) {
        stop();
        return;
    }

    setupController();
    setupStreamSink();
    setupScheduler();
}

void GenericRtspCameraImp::onAllStreamReady() {
    if (_all_stream_ready || _exit.load()) {
        return;
    }
    _all_stream_ready = true;
    if (_src) {
        // set this as listener to handle device events
        _src->setListener(shared_from_this());
        // trigger device registration event
        _src->regist();
    }
    if (_controller) {
        _controller->setListener(shared_from_this());
    }
    if (_sink) {
        _sink->setListener(shared_from_this());
    }
}

void GenericRtspCameraImp::setupController() {
    if (!_controller) {
        _controller = std::make_shared<CameraController>(_src->getDeviceTuple(), _poller);
        _controller->createTimer();
        {
            // add user presets after controller constructor
            auto strong_statistic = _statistic.lock();
            auto params = strong_statistic->getParams();
            if (!params.device_stats.user_presets.empty()) {
                for (auto &it : params.device_stats.user_presets) {
                    auto p = it.second;
                    _controller->addUserPTZPreset(p.Token, p.Name, p.absPan, p.absTilt, p.absZoom);
                }
            }
            // add media profile config after controller constructor
            if (!params.device_stats.stream_settings.empty()) {
                for (auto &it : params.device_stats.stream_settings) {
                    auto token = it.first;
                    auto config = it.second;
                    _controller->addProfileConfig(token, config, false);
                }
            }
        }
    }
    _controller->setupController(_option);
}

void GenericRtspCameraImp::setupStreamSink() {
    if (!_sink) {
        _sink = std::make_shared<StreamSink>(_src->getDeviceTuple(),_poller);
        _sink->createTimer();
    }

    if (_src->hasStreamTuple(PrimaryStream)) {
        if (!_option.disablePrimaryStream) {
            auto tuple = _src->getStreamTuple(PrimaryStream);
            _sink->setupMonitor(PrimaryStream, tuple, _option);
        } else {
            _sink->stopMonitor(PrimaryStream);
        }
    }

    if (_src->hasStreamTuple(SecondaryStream)) {
        if (!_option.disableSecondaryStream) {
            auto tuple = _src->getStreamTuple(SecondaryStream);
            _sink->setupMonitor(SecondaryStream, tuple, _option);
        } else {
            _sink->stopMonitor(SecondaryStream);
        }
    }

    onAllStreamReady();
}

void GenericRtspCameraImp::setupScheduler() {
    if (_scheduler) {
        auto profile = _scheduler->getProfile();
        if (profile == _option.recordSchedules) {
            DebugL << "Record scheduler for camera " << _src->getUrl() << " already setup with the same profile. Ignore setup scheduler request";
            return;
        }
        _scheduler.reset();
    }
    _scheduler = RecordScheduler::create(_src->getDeviceTuple(), _option.recordSchedules, _poller);
    _scheduler->setListener(shared_from_this());
}

void GenericRtspCameraImp::stop() {
    if (_controller) {
        _controller->stopController();
    }
    if (_sink) {
        if (_src->hasStreamTuple(PrimaryStream)) {
            _sink->stopMonitor(PrimaryStream);
        }
        if (_src->hasStreamTuple(SecondaryStream)) {
            _sink->stopMonitor(SecondaryStream);
        }
    }
    if (_scheduler) {
        _scheduler->stopTimer();
    }

    onAllStreamReady();
}

void GenericRtspCameraImp::PTZMove(const std::string &strDirect, int speed, const std::function<void(const SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call PTZMove in it's owner poller");
    if (!_controller) {
        cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    _controller->PTZMove(strDirect, speed, cb);
}

CameraStatisticImp::Ptr GenericRtspCameraImp::getCameraStatisticImp() {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore get statistic request";
        return nullptr;
    }
    return strong_statistic;
}

void GenericRtspCameraImp::saveCameraOption(const CameraOption &option) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore camera option save";
        return;
    }
    strong_statistic->setCameraOption(option);
}   

void GenericRtspCameraImp::onRecordModeChange(DeviceSource &sender, int archive_mode, bool start) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call onRecordModeChange in it's owner poller");
    if (!_sink) {
        WarnL << "Stream sink for camera " << _src->getUrl() << " is not ready. Ignore setup record mode request";
        return;
    }
    _sink->setupRecord(archive_mode, start);
}

void GenericRtspCameraImp::onImageQualityChange(DeviceSource &sender, int fps, int q) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call onImageQualityChange in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore setup image quality request";
        return;
    }
    auto quality = static_cast<ImageQuality>(q);
    // InfoL << "Camera " << _src->getUrl() << " image quality changed: fps=" << fps << ", q=" << getImageQualityString(quality);
    // todo:
    // _controller->setupImageQuality(fps, quality);
}

bool GenericRtspCameraImp::setupRecordEvent(RecordEventType type, bool start) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setupRecordEvent in it's owner poller");
    if (!_scheduler) {
        WarnL << "Record scheduler for camera " << _src->getUrl() << " is not ready. Ignore setup record event request";
        return false;
    }
    return _scheduler->setupRecordEvent(type, start);
}

void GenericRtspCameraImp::onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore stream ready event";
        return;
    }
    // get current stream live status from statistic, because stream source may trigger onStreamReady with the same status when sink setup monitor or scheduler setup record, we only want to emit event when stream status changed
    auto params = strong_statistic->getParams();
    bool current_stream_live = params.sinfo_map.count(type) > 0 ? params.sinfo_map[type].live : false;
    {
        const mediakit::TranslationInfo *info = nullptr;
        if (data.is<mediakit::TranslationInfo>()) {
            info = &data.get<mediakit::TranslationInfo>();
        }
        strong_statistic->addStreamStatistic(type, live, status, info);
    }
    if (_option.emitStreamStatusChangeEvent && live != current_stream_live) {
        // only emit event when stream status changed
        std::weak_ptr<GenericRtspCameraImp> weak_self = shared_from_this();
        WorkThreadPool::Instance().getPoller()->async([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            auto &src = *strong_self->_src;
            NOTICE_EMIT(BroadcastDeviceStatsChangedArgs, Broadcast::kBroadcastDeviceStatsChanged, src);
        });
    }
}

void GenericRtspCameraImp::onControllerReady(DeviceSource &sender, bool connect, const std::string &status, const toolkit::Any &data) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore controller ready event";
        return;
    }

    // Snapshot state before update to detect changes
    auto old_params = strong_statistic->getParams();
    bool prev_connect = old_params.device_stats.connect;
    DeviceCapabilities prev_caps = old_params.device_stats.device_caps;

    const DeviceCapabilities *caps = nullptr;
    if (data.is<DeviceCapabilities>()) {
        caps = &data.get<DeviceCapabilities>();
    }
    strong_statistic->addDeviceCapabilities(connect, status, caps);

    // Emit only on status change (false → true or true → false) or when caps actually changed
    bool is_restart = prev_connect != connect;
    bool caps_changed = caps && (*caps != prev_caps);
    if (!is_restart && !caps_changed) {
        return;
    }

    std::weak_ptr<GenericRtspCameraImp> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto &src = *strong_self->_src;
        auto strong_statistic = strong_self->_statistic.lock();
        if (strong_statistic) {
            auto params = strong_statistic->getParams();
            NOTICE_EMIT(BroadcastDeviceCapsChangedArgs, Broadcast::kBroadcastDeviceCapsChanged, params.device_stats.device_caps, src);
        }
    });
}

void GenericRtspCameraImp::setupStreamRegist(int type, bool regist) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setupStreamRegist in it's owner poller");
    if (!_sink) {
        WarnL << "Stream sink for camera " << _src->getUrl() << " is not ready. Ignore setup stream regist request";
        return;
    }
    bool event_active = _scheduler ? _scheduler->isEventActive() : false;
    _sink->setStreamRegist(type, regist, event_active);
}

void GenericRtspCameraImp::addUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call addUserPreset in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore add user preset request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    auto statistic_weak = _statistic;
    auto url = _src->getUrl();
    _controller->addUserPTZPreset(presetToken, presetName,
        [cb, statistic_weak, presetToken, presetName, url](const SockException &ex, float pan, float tilt, float zoom) {
            if (ex) { cb(ex); return; }
            auto strong_statistic = statistic_weak.lock();
            if (!strong_statistic) {
                WarnL << "Camera " << url << " statistic has been released. Ignore preset added event";
            } else {
                strong_statistic->addUserPresets(presetToken, presetName, pan, tilt, zoom, true);
            }
            cb(ex);
        });
}

void GenericRtspCameraImp::removeUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call removeUserPreset in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore remove user preset request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    if (!_controller->removeUserPTZPreset(presetToken, presetName, cb)) {
        return;
    }
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore controller ready event";
        return;
    }
    strong_statistic->addUserPresets(presetToken, presetName, 0.0, 0.0, 0.0, false);
}

void GenericRtspCameraImp::PTZGotoPreset(const std::string &presetToken, bool isUserPreset, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call PTZGotoPreset in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore goto preset request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    _controller->PTZGotoPreset(presetToken, isUserPreset, cb);
}

void GenericRtspCameraImp::setMediaProfile(const std::string &profileToken, VideoEncoderConfig &config, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setMediaProfile in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore set media profile request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    _controller->setMediaProfileAsync(profileToken, config, cb);
}

void GenericRtspCameraImp::getMediaProfile(const std::string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call getMediaProfile in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore get media profile request";
        VideoEncoderConfig config;
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE), config);
        return;
    }
    _controller->getMediaProfileAsync(profileToken, cb);
}

void GenericRtspCameraImp::ImageMoveControl(const std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call ImageMoveControl in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore image move control request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    _controller->ImageMoveControl(strDirect, speed, cb);
}

void GenericRtspCameraImp::RelayOutputControl(const std::string &strDirect, const std::string &relayToken, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call RelayOutputControl in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore relay output control request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    _controller->RelayOutputControl(strDirect, relayToken, cb);
}

} // namespace managerkit
