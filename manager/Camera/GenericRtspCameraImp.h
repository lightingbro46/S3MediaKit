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

    void setSyncMode(bool enable);

    const CameraOption& getCameraOption() const { return _option; } 

    CameraStatisticImp::Ptr getCameraStatisticImp();

    void PTZMove(const std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb);

    void ImageMoveControl(const std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb);
    
    void RelayOutputControl(const std::string &strDirect, const std::string &relayToken, const std::function<void(const toolkit::SockException &ex)> &cb);

    bool setupRecordEvent(RecordEventType type, bool start);

    void setupStreamRegist(int type, bool regist);

    void addUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb);

    void removeUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb);

    void PTZGotoPreset(const std::string &presetToken, bool isUserPreset, const std::function<void(const toolkit::SockException &ex)> &cb);

    void setMediaProfile(const std::string &profileToken, VideoEncoderConfig &config, const std::function<void(const toolkit::SockException &ex)> &cb);

    void getMediaProfile(const std::string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb);

    void getSDCardInfo(const std::function<void(const toolkit::SockException &ex, SDCardInformation &info)> &cb);

public:
    //////////////DeviceSourceEvent related interface implementation////////////////
    toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &sender) override { return _poller; }

    void onRecordModeChange(DeviceSource &sender, int archive_mode, bool start) override;

    void onImageQualityChange(DeviceSource &sender, int fps, int q) override;

    void onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) override;

    void onControllerReady(DeviceSource &sender, bool connect, const std::string &status, const toolkit::Any &data) override;

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
    std::atomic<bool> _exit { false };
    GenericRtspCamera::Ptr _src;
    CameraOption _option;
    CameraController::Ptr _controller;
    StreamSink::Ptr _sink;
    RecordScheduler::Ptr _scheduler;
    std::weak_ptr<CameraStatisticImp> _statistic;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H