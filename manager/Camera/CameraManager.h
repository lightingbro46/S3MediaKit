#ifndef S3MANAGERKIT_CAMERAMANAGER_H
#define S3MANAGERKIT_CAMERAMANAGER_H

#include "GenericRtspCameraImp.h"

namespace managerkit {

class CameraManager : public std::enable_shared_from_this<CameraManager> {
public:
    using Ptr = std::shared_ptr<CameraManager>;

    static CameraManager& Instance();
    ~CameraManager() = default;

    bool addCamera(DeviceTuple &tuple, CameraOption &option, std::unordered_map<int, StreamTuple> &stream_map);

    bool addCamera(CameraStatisticImp::Ptr &stats);

    bool delCamera(const std::string &key);

    void clear();

    std::vector<std::string> getCameraKeys();

    void loadSavedCameraInfo();

    void setReady(bool ready);

    bool isReady();

    // Collect an online/offline change, reported with the others after camera.deviceStatusReportDelaySec
    void notifyStatusChanged(const DeviceSource::Ptr &src);

private:
    CameraManager();

    void flushStatusChanged();

    void onManager();

private:
    std::recursive_mutex _mtx;
    bool _ready = false; // ready for receive new camera
    toolkit::Timer::Ptr _timer;
    std::unordered_map<std::string, GenericRtspCameraImp::Ptr> _gcImp;
    // Separate from _mtx: delCamera holds _mtx across blocking ESC calls
    std::mutex _status_mtx;
    bool _status_flush_scheduled = false;
    std::unordered_map<std::string, std::weak_ptr<DeviceSource>> _pending_status;
};

} // namespace managerkit



#endif // S3MANAGERKIT_CAMERAMANAGER_H