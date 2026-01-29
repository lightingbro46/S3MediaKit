#ifndef CAMERA_GENERICRTSPCAMERAIMP_H
#define CAMERA_GENERICRTSPCAMERAIMP_H

#include "GenericRtspCamera.h"
#include "CameraController.h"
#include "StreamSink.h"
#include "Local/StatisticRecorder.h"
#include "Extension/RecordPolicy.h"

namespace managerkit {

class GenericRtspCameraImp final : public GenericRtspCamera {
public:
    using Ptr = std::shared_ptr<GenericRtspCameraImp>;

    GenericRtspCameraImp(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map, const CameraStatisticImp::Ptr &statistic);

    ~GenericRtspCameraImp() override;

    bool isEnabled() const {
        return _enabled.load();
    }

    void setCameraOption(const CameraOption &option);

    const CameraOption& getCameraOption() const {
        return _option;
    }

    void stop();

    void PTZMove(std::string &strDirect, int &speed, const std::function<void(const toolkit::SockException &ex)> &cb);

    CameraStatisticImp::Ptr getCameraStatisticImp();

    void onMotionDetected(bool bActive, uint64_t pre_ms);

private:
    void onAllStreamReady();

    void setupController();

    void setupStreamSink();

    void saveCameraOption(const CameraOption &option);

private:
    toolkit::EventPoller::Ptr _poller;
    bool _all_stream_ready = false;
    std::atomic<bool> _enabled { false };
    CameraOption _option;
    CameraController::Ptr _controller;
    // RecordingController::Ptr _recording_controller;
    StreamSink::Ptr _sink;
    std::weak_ptr<CameraStatisticImp> _statistic;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H