#ifndef S3MANAGERKIT_CAMERAMANAGER_H
#define S3MANAGERKIT_CAMERAMANAGER_H

#include "GenericRtspCameraImp.h"

#define DEFAULT_GENERIC_RTSP "GENERIC-RTSP"
#define DEFAULT_ONVIF_CAMERA "ONVIF-CAMERA"

namespace managerkit {

class CameraManager {
public:
    using Ptr = std::shared_ptr<CameraManager>;

    static CameraManager& Instance();
    ~CameraManager() = default;

    bool addCamera(CameraInfo &info, CameraOption &option, std::unordered_map<int, StreamTuple> &stream_map);

    bool delCamera(const std::string &key);

    void clear();

    std::vector<std::string> getCameraKeys();

private:
    CameraManager();

    void onManager();

private:
    std::recursive_mutex _mtx;
    toolkit::Timer::Ptr _timer;
    std::unordered_map<std::string, GenericRtspCameraImp::Ptr> _gcImp;
};

} // namespace managerkit



#endif // S3MANAGERKIT_CAMERAMANAGER_H