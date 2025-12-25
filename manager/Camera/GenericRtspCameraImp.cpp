#include "GenericRtspCameraImp.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const CameraInfo& info, const std::unordered_map<int, StreamTuple>& stream_map, const CameraStatisticImp::Ptr& statistic)
    : GenericRtspCamera(info, stream_map), _statistic(statistic) {
    _poller = EventPollerPool::Instance().getPoller();
    statistic->setCameraInfo(info);
    statistic->setStreamTuples(stream_map);
}

GenericRtspCameraImp::~GenericRtspCameraImp() {
    stop();
}

void GenericRtspCameraImp::setCameraOption(const CameraOption& option) {
    if (equalCameraOption(const_cast<const CameraOption&>(_option), const_cast<const CameraOption&>(option))) {
        return; // No change
    }
    _option = option;
    saveCameraOption(option);
    
    if (!_option.enableActive) {
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
    regist();
}

void GenericRtspCameraImp::setupController() {
    if (!_controller) {
        _controller = std::make_shared<CameraController>(_poller);
        _controller->setOnControllerReady([this](bool connect, const std::string &status, bool enablePTZ) {
            auto strong_statistic = _statistic.lock();
            if (!strong_statistic) {
                WarnL << "Camera statistic has been released. Ignore device capabilities update";
                return;
            }
            strong_statistic->addDeviceCapabilities(connect, status, enablePTZ);
        });
        _controller->start();
    }
    auto info = getCameraInfo();
    _controller->setupController(info, _option);
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
    if (hasStreamTuple(PrimaryStream)) {
        auto tuple = getStreamTuple(PrimaryStream);
        _sink->setupMonitor(PrimaryStream, tuple, _option);
    }

    if (hasStreamTuple(SecondaryStream)) { 
        auto tuple = getStreamTuple(SecondaryStream);
        _sink->setupMonitor(SecondaryStream, tuple, _option);
    }

    onAllStreamReady();
}

void GenericRtspCameraImp::stop() {
    if (_controller) {
        _controller->stopController();
    }
    if (_sink) {
        if (hasStreamTuple(PrimaryStream)) {
            _sink->stopMonitor(PrimaryStream);
        }
        if (hasStreamTuple(SecondaryStream)) {
            _sink->stopMonitor(SecondaryStream);
        }
    }
}

void GenericRtspCameraImp::PTZMove(std::string &strDirect, int &speed, const std::function<void(const SockException &ex)> &cb) {
    //todo: only one session to control ptz
    if (_controller) {
        _controller->PTZMove(strDirect, speed, cb);
    } else {
        cb(SockException(Err_other, "Device controller is not ready"));
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

} // namespace managerkit
