#include "CameraManager.h"
#include "Util/util.h"
#include <algorithm>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(CameraManager)

CameraManager::CameraManager() {}

template <typename Pointer>
static bool equalStreamConfig(Pointer ptr, int type, unordered_map<int, StreamTuple> stream_map) {
    // if (stream_map.find(type) != stream_map.end()) {
    //     return ptr->hasStreamTuple(type) && equalStreamTuple(ptr->getStreamTuple(type), stream_map[type]);
    // } else {
    //     return !ptr->hasStreamTuple(type);
    // }
    if (ptr->hasStreamTuple(type)) {
        auto tuple = ptr->getStreamTuple(type);
        auto it = std::find_if(stream_map.begin(), stream_map.end(), [&](const pair<int, StreamTuple> &pair) { return pair.second.stream_id == tuple.stream_id; });
        if (it != stream_map.end()) {
            return equalStreamTuple(tuple, it->second);
        } else {
            return false;
        }
    } else {
        return stream_map.find(type) == stream_map.end();
    }
}

template <typename Pointer>
static bool equalCameraConfig(Pointer ptr, CameraInfo &info, unordered_map<int, StreamTuple> stream_map) {
    if (!equalCameraInfo(ptr->getCameraInfo(), info)) {
        return false;
    }

    if (!equalStreamConfig(ptr, PrimaryStream, stream_map)) {
        return false;
    }

    if (!equalStreamConfig(ptr, SecondaryStream, stream_map)) {
        return false;
    }
    
    return true;
}

bool CameraManager::addCamera(CameraInfo &info, CameraOption &option, unordered_map<int, StreamTuple> &stream_map) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    auto ret = DeviceSource::find(CAMERA_SCHEMA, info.vhost, info.device_id);
    if  (ret) {
        // device tuple already exist
        auto gc = dynamic_pointer_cast<GenericRtspCameraImp>(ret);
        if (gc) {
            if (equalCameraConfig(gc, info, stream_map)) {
                gc->setCameraOption(option);
                return true;
            }
            // delete ptr if config change
            _gcImp.erase(info.shortUrl());
        }   
    }

    // create new one
    auto imp = std::make_shared<GenericRtspCameraImp>(info, stream_map);
    imp->setCameraOption(option);
    _gcImp.emplace(info.shortUrl(), imp);
    return true;
}

bool CameraManager::delCamera(const string &key) {
    auto it_gc = _gcImp.find(key);
    if (it_gc != _gcImp.end()) {
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        GET_CONFIG(bool, enableVHost, General::kEnableVhost)
        if (enableVHost) {
            auto imp = it_gc->second;
            if (imp->getCameraInfo().vhost != mediaServerId) {
                if (imp->isEnabled()) {
                    //disable device in failover mode
                    CameraOption option = imp->getCameraOption();
                    option.enableActive = false;
                    imp->setCameraOption(option);
                }
                return true;
            }
        }
        // remove device out of list
        _gcImp.erase(key);
        return true;
    }
    return false;
}

void CameraManager::clear() {
    _gcImp.clear();
}

vector<string> CameraManager::getCameraKeys() {
    vector<string> ret;
    for (const auto &it : _gcImp ) {
        ret.push_back(it.first);
    }
    return ret;
}

} // namespace managerkit
