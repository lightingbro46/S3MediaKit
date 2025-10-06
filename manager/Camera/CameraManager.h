#ifndef S3MANAGERKIT_CAMERAMANAGER_H
#define S3MANAGERKIT_CAMERAMANAGER_H

#include "GenericRtspCameraImp.h"

namespace managerkit {

class CameraManager {
public:
    using Ptr = std::shared_ptr<CameraManager>;

    static CameraManager& Instance();
    ~CameraManager() = default;

    bool addCamera(CameraInfo &info, CameraOption &option, std::unordered_map<int, StreamTuple> &stream_map);

    bool delCamera(const std::string &key);

    void release(bool continuous = false);

    void clear(bool continuous = false);

    std::vector<std::string> getCameraKeys();

    void loadSavedCameraInfo();

private:
    CameraManager();

    void onManager();

private:
    std::recursive_mutex _mtx;
    bool _ready = true; // ready for receive new camera
    toolkit::Timer::Ptr _timer;
    std::unordered_map<std::string, GenericRtspCameraImp::Ptr> _gcImp;
};

} // namespace managerkit



#endif // S3MANAGERKIT_CAMERAMANAGER_H