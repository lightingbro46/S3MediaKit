#ifndef CAMERA_GENERICRTSPCAMERAIMP_H
#define CAMERA_GENERICRTSPCAMERAIMP_H

#include "GenericRtspCamera.h"
#include "CameraController.h"
#include "StreamSink.h"
#include "Local/StatisticRecorder.h"
#include "RecordPolicy.h"

namespace managerkit {

class GenericRtspCameraImp : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<GenericRtspCameraImp> {
public:
    using Ptr = std::shared_ptr<GenericRtspCameraImp>;

    GenericRtspCameraImp(const DeviceTuple &tuple, const std::unordered_map<int, StreamTuple> &stream_map, const CameraStatisticImp::Ptr &statistic);

    ~GenericRtspCameraImp();

    bool isEnabled() const { return _enabled.load(); }

    GenericRtspCamera::Ptr getCameraSource() const { return _src; }

    void setCameraOption(const CameraOption &option);

    const CameraOption& getCameraOption() const { return _option; } 

    CameraStatisticImp::Ptr getCameraStatisticImp();

    void PTZMove(std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb);

    bool setupRecordEvent(RecordEventType type, bool start);

public:
    //////////////DeviceSourceEvent related interface implementation////////////////
    toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &sender) override { return _poller; }

    void onRecordModeChange(DeviceSource &sender, int archive_mode, bool start) override;

    void onImageQualityChange(DeviceSource &sender, int fps, int q) override;

private:
    void onAllStreamReady();

    void setupController();

    void setupStreamSink();

    void setupScheduler();

    void saveCameraOption(const CameraOption &option);

    void stop();

private:
    toolkit::EventPoller::Ptr _poller;
    bool _all_stream_ready = false;
    std::atomic<bool> _enabled { false };
    GenericRtspCamera::Ptr _src;
    CameraOption _option;
    CameraController::Ptr _controller;
    StreamSink::Ptr _sink;
    RecordScheduler::Ptr _scheduler;
    std::weak_ptr<CameraStatisticImp> _statistic;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H