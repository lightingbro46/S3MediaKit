#ifndef CAMERA_GENERICRTSPCAMERAIMP_H
#define CAMERA_GENERICRTSPCAMERAIMP_H

#include "GenericRtspCamera.h"
#include "StreamSink.h"
#include "CameraController.h"
#include "Local/RecordStrategy.h"

namespace managerkit {
    
class GenericRtspCameraImp : public GenericRtspCamera, public StreamSink, public CameraController, public RecordStrategy {
public:
    using Ptr = std::shared_ptr<GenericRtspCameraImp>;

    GenericRtspCameraImp(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map);

    const CameraOption &getCameraOption() const { return _option; }

    void setCameraOption(const CameraOption &option);

    bool isEnabled() { return _enabled; }

private:
    void onAllStreamReady() override;

    void onRecordModeChange(RecordMode mode) override;

    void setupRecordStream(RecordMode mode);

    void stopRecordStream();

private:
    bool _enabled = false;
    CameraOption _option;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H
