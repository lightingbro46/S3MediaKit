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

    bool isEnabled() { return _enabled; }

    void stop();

private:
    void onAllStreamReady() override;

    void onStreamChange(int stream_type) override;

    void onRecordModeChange(RecordMode mode) override;

    void setupRecordStream(RecordMode mode, const CameraOption &option);

    void stopRecordStream();

    void onSetCameraOption(const CameraOption &option) override;

private:
    bool _enabled = false;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H
