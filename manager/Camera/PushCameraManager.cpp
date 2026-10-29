#include "PushCameraManager.h"
#include "Util/util.h"
#include "Local/StatisticRecorder.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

INSTANCE_IMP(PushCameraManager)

bool PushCameraManager::addPushCamera(const DeviceTuple &tuple, const PushCameraOption &option, const unordered_map<int, StreamTuple> &stream_map) {
    if (!isReady()) {
        TraceL << "Push camera manager has not been ready";
        return false;
    }
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _cameras.find(tuple.device_id);
        if (it != _cameras.end()) {
            auto src = it->second->getCameraSource();
            if (equalDeviceTuple(src->getDeviceTuple(), tuple) && src->getStreamMap() == stream_map) {
                auto imp = it->second;
                imp->getOwnerPoller(DeviceSource::NullDeviceSource())->async([imp, option]() { imp->setPushCameraOption(option); });
                return true;
            }
            // configuration changed, remove old one
            _cameras.erase(it);
        }
    }
    auto stats = StatisticRecorder::Instance().getPushCameraRecorder(tuple.device_id, true);
    if (!stats) {
        return false;
    }
    auto imp = std::make_shared<GenericPushCameraImp>(tuple, stream_map, stats);
    imp->getOwnerPoller(DeviceSource::NullDeviceSource())->async([imp, option]() { imp->setPushCameraOption(option); });
    std::lock_guard<std::mutex> lck(_mtx);
    _cameras[tuple.device_id] = imp;
    return true;
}

void PushCameraManager::addPushCamera(const PushCameraStatisticImp::Ptr &stats, const PushCameraOption &option) {
    auto params = stats->getParams();
    auto imp = std::make_shared<GenericPushCameraImp>(params.tuple, params.stream_map, stats);
    imp->getOwnerPoller(DeviceSource::NullDeviceSource())->async([imp, option]() { imp->setPushCameraOption(option); });
    std::lock_guard<std::mutex> lck(_mtx);
    _cameras[params.tuple.device_id] = imp;
}

bool PushCameraManager::delPushCamera(const string &device_id) {
    GenericPushCameraImp::Ptr imp;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        auto it = _cameras.find(device_id);
        if (it == _cameras.end()) {
            return false;
        }
        imp = it->second;
        _cameras.erase(it);
    }
    if (auto stats = imp->getPushCameraStatisticImp()) {
        stats->remove();
    }
    return true;
}

GenericPushCameraImp::Ptr PushCameraManager::getPushCamera(const string &device_id) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto it = _cameras.find(device_id);
    return it == _cameras.end() ? nullptr : it->second;
}

vector<string> PushCameraManager::getPushCameraKeys() {
    std::lock_guard<std::mutex> lck(_mtx);
    vector<string> ret;
    for (const auto &it : _cameras) {
        ret.push_back(it.first);
    }
    return ret;
}

bool PushCameraManager::isReady() {
    std::lock_guard<std::mutex> lck(_mtx);
    return _ready;
}

void PushCameraManager::addSaved(const DeviceStatisticBase::Ptr &stats) {
    auto camera = std::dynamic_pointer_cast<PushCameraStatisticImp>(stats);
    if (!camera) {
        return;
    }
    addPushCamera(camera, camera->getParams().option);
    DebugL << "Added saved push camera: " << camera->deviceId();
}

void PushCameraManager::onSavedLoaded() {
    InfoL << "Loaded all saved push camera. Finished";
    std::lock_guard<std::mutex> lck(_mtx);
    _ready = true;
}

} // namespace managerkit
