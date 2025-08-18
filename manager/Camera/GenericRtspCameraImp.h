#ifndef CAMERA_GENERICRTSPCAMERAIMP_H
#define CAMERA_GENERICRTSPCAMERAIMP_H

#include "GenericRtspCamera.h"
#include "StreamSink.h"

namespace managerkit {
    
class GenericRtspCameraImp : public GenericRtspCamera, public StreamSink {
public:
    using Ptr = std::shared_ptr<GenericRtspCameraImp>;

    GenericRtspCameraImp(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map);

    const CameraOption &getCameraOption() const { return _option; }

    void setCameraOption(const CameraOption &option);

    bool isEnabled() { return _enabled; }

    void onAllStreamReady() override;

private:
    bool _enabled = false;
    CameraOption _option;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERAIMP_H
