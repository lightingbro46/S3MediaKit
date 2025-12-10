#ifndef S3MANAGERKIT_CAMERAMANAGER_H
#define S3MANAGERKIT_CAMERAMANAGER_H

#include "GenericRtspCameraImp.h"

namespace managerkit {

class CameraManager : public std::enable_shared_from_this<CameraManager> {
public:
    using Ptr = std::shared_ptr<CameraManager>;

    static CameraManager& Instance();
    ~CameraManager() = default;

    bool addCamera(CameraInfo &info, CameraOption &option, std::unordered_map<int, StreamTuple> &stream_map);

    bool addCamera(CameraStatisticImp::Ptr &stats);

    bool delCamera(const std::string &key);

    void clear();

    std::vector<std::string> getCameraKeys();

    void loadSavedCameraInfo();

    void setReady(bool ready);

    bool isReady();

private:
    CameraManager();

    void onManager();

private:
    std::recursive_mutex _mtx;
    bool _ready = false; // ready for receive new camera
    toolkit::Timer::Ptr _timer;
    std::unordered_map<std::string, GenericRtspCameraImp::Ptr> _gcImp;
};

} // namespace managerkit



#endif // S3MANAGERKIT_CAMERAMANAGER_H