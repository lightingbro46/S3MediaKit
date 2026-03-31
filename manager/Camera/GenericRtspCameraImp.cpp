#include "GenericRtspCameraImp.h"
#include "server/WebApiErrCode.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const DeviceTuple& tuple, const std::unordered_map<int, StreamTuple>& stream_map, const CameraStatisticImp::Ptr& statistic)
    : _statistic(statistic) {
    _poller = EventPollerPool::Instance().getPoller();
    _src = std::make_shared<GenericRtspCamera>(tuple, stream_map);
    statistic->setDeviceTuple(tuple);
    statistic->setStreamTuples(stream_map);
}

GenericRtspCameraImp::~GenericRtspCameraImp() {
    stop();
}

void GenericRtspCameraImp::setCameraOption(const CameraOption& option) {
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, option]() {
            self->setCameraOption(option);
        });
        return;
    }
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
    if (_all_stream_ready) {
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
    const mediakit::TranslationInfo *info = nullptr;
    if (data.is<mediakit::TranslationInfo>()) {
        info = &data.get<mediakit::TranslationInfo>();
    }
    strong_statistic->addStreamStatistic(type, live, status, info);
}

void GenericRtspCameraImp::onControllerReady(DeviceSource &sender, bool connect, const std::string &status, const toolkit::Any &data) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore controller ready event";
        return;
    }

    const DeviceCapabilities *caps = nullptr;
    if (data.is<DeviceCapabilities>()) {
        caps = &data.get<DeviceCapabilities>();
    }
    strong_statistic->addDeviceCapabilities(connect, status, caps);

    // update device capabilities if controller is connected
    if (connect && caps) {
        auto sender = _src;
        NOTICE_EMIT(BroadcastDeviceCapsChangedArgs, Broadcast::kBroadcastDeviceCapsChanged, *caps, *sender);
    }
}

void GenericRtspCameraImp::setupStreamRegist(int type, bool regist) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setupStreamRegist in it's owner poller");
    if (!_sink) {
        WarnL << "Stream sink for camera " << _src->getUrl() << " is not ready. Ignore setup stream regist request";
        return;
    }
    _sink->setStreamRegist(type, regist);
}

void GenericRtspCameraImp::addUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call addUserPreset in it's owner poller");
    if (!_controller) {
        WarnL << "Camera " << _src->getUrl() << " controller is not ready. Ignore add user preset request";
        cb(SockException(Err_other, "Camera controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }
    float pan = 0.0, tilt = 0.0, zoom = 0.0;
    if (!_controller->addUserPTZPreset(presetToken, presetName, pan, tilt, zoom, cb)) {
        return;
    }
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera " << _src->getUrl() << " statistic has been released. Ignore controller ready event";
        return;
    }
    strong_statistic->addUserPresets(presetToken, presetName, pan, tilt, zoom, true);
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

} // namespace managerkit
