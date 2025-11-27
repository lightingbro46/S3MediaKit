#ifndef CAMERA_GENERICRTSPCAMERAIMP_H
#define CAMERA_GENERICRTSPCAMERAIMP_H

#include "GenericRtspCamera.h"
#include "StreamSink.h"
#include "CameraController.h"
#include "Local/RecordStrategy.h"
#include "CameraStatistic.h"

namespace managerkit {
    
class GenericRtspCameraImp : public GenericRtspCamera, public StreamSink, public CameraController, public RecordStrategy , public CameraStatisticImp {
public:
    using Ptr = std::shared_ptr<GenericRtspCameraImp>;

    GenericRtspCameraImp(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map);

    void setCameraOptionImp(const CameraOption &option);

    bool isEnabled() { return _enabled.load(); }

    void stop();

private:
    void onAllStreamReady();

    void onStreamChange(int stream_type) override;

    void onRecordModeChange(RecordMode mode) override;

    void setupRecordStream(RecordMode mode);

    void stopRecordStream();

    void onSetCameraOption(const CameraOption &option);

    void onControllerReady() override; 

private:
    std::atomic<bool> _enabled {false};
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H
