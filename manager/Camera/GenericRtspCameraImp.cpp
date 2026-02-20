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
}

void GenericRtspCameraImp::setupController() {
    if (!_controller) {
        _controller = std::make_shared<CameraController>(_poller);
        _controller->setOnControllerReady([this](bool connect, const std::string &status, const DeviceCapabilities *caps) {
            auto strong_statistic = _statistic.lock();
            if (!strong_statistic) {
                WarnL << "Camera statistic has been released. Ignore device capabilities update";
                return;
            }
            strong_statistic->addDeviceCapabilities(connect, status, caps);

            // update device capabilities if controller is connected
            if (connect) {
                auto sender = _src;
                NOTICE_EMIT(BroadcastDeviceCapsChangedArgs, Broadcast::kBroadcastDeviceCapsChanged, *caps, *sender);
            }
        });
        _controller->start();
    }
    _controller->setupController(_option);
}

void GenericRtspCameraImp::setupStreamSink() {
    if (!_sink) {
        _sink = std::make_shared<StreamSink>(_poller);
        _sink->setOnStreamUpdate([this](int type, bool live, const std::string &status, const mediakit::TranslationInfo *info) {
            auto strong_statistic = _statistic.lock();
            if (!strong_statistic) {
                WarnL << "Camera statistic has been released. Ignore stream statistics update";
                return;
            }
            strong_statistic->addStreamStatistic(type, live, status, info);
        });
        _sink->start();
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

    onAllStreamReady();
}

void GenericRtspCameraImp::PTZMove(std::string &strDirect, int speed, const std::function<void(const SockException &ex)> &cb) {
    if (_controller) {
        _controller->PTZMove(strDirect, speed, cb);
    } else {
        cb(SockException(Err_other, "Device controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
    }
}

CameraStatisticImp::Ptr GenericRtspCameraImp::getCameraStatisticImp() {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera statistic has been released. Ignore get statistic request";
        return nullptr;
    }
    return strong_statistic;
}

void GenericRtspCameraImp::saveCameraOption(const CameraOption &option) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Camera statistic has been released. Ignore camera option save";
        return;
    }
    strong_statistic->setCameraOption(option);
}   

// void GenericRtspCameraImp::onMotionDetected(bool bActive, uint64_t pre_ms) {
//     if (_recorder) {
//         _recorder->onRecordEvent(bActive, pre_ms);
//     }
// }

// void GenericRtspCameraImp::setupRecorder() {
//     if (!_recorder) {
//         _recorder = std::make_shared<RecordingController>(_poller);
//         _recorder->setOnRecordModeChange([this](int type, bool start, bool archive, int backtime_ms) {
//             DebugL << "Recording mode changed to " << (start ? "start" : "stop") << ", archive: " << archive << ", backtime_ms: " << backtime_ms;
//         });
//         _recorder->start();
//     }
//     _recorder->setScheduleStr(_option.recordSchedules);
// }

} // namespace managerkit
