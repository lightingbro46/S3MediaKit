#ifndef CAMERA_PUSHCAMERAMANAGER_H
#define CAMERA_PUSHCAMERAMANAGER_H

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "GenericPushCameraImp.h"

namespace managerkit {

// Cameras pushing their stream to this server (drone, mobile video push). The push ingest and the
// API creating them are not implemented yet.
class PushCameraManager : public std::enable_shared_from_this<PushCameraManager>, public ISavedDeviceLoader {
public:
    using Ptr = std::shared_ptr<PushCameraManager>;

    static PushCameraManager &Instance();

    // Creates or updates the push camera. false before the saved devices are loaded, or when the id
    // already belongs to a device of another type. Blocking (writes info.txt).
    bool addPushCamera(const DeviceTuple &tuple, const PushCameraOption &option, const std::unordered_map<int, StreamTuple> &stream_map);

    // Removes the push camera and its info.txt. Blocking.
    bool delPushCamera(const std::string &device_id);

    GenericPushCameraImp::Ptr getPushCamera(const std::string &device_id);

    std::vector<std::string> getPushCameraKeys();

    bool isReady();

    // ISavedDeviceLoader: saved push cameras from StatisticRecorder::loadAll()
    void addSaved(const DeviceStatisticBase::Ptr &stats) override;

    void onSavedLoaded() override;

private:
    PushCameraManager() = default;

    void addPushCamera(const PushCameraStatisticImp::Ptr &stats, const PushCameraOption &option);

private:
    std::mutex _mtx;
    bool _ready = false;
    std::unordered_map<std::string, GenericPushCameraImp::Ptr> _cameras;
};

} // namespace managerkit

#endif // CAMERA_PUSHCAMERAMANAGER_H
