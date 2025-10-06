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
static bool equalStreamConfig(Pointer ptr, int type, unordered_map<int, StreamTuple> &stream_map) {
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
static bool equalCameraConfig(Pointer ptr, CameraInfo &info, unordered_map<int, StreamTuple> &stream_map) {
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
    if (!_ready) {
        WarnL << "Camera manager has not been ready";
        return false;
    }

    auto it = _gcImp.find(info.shortUrl());
    if  (it != _gcImp.end()) {
        // device tuple already exist
        auto gc = it->second;
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
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!_ready) {
        WarnL << "Camera manager has not been ready";
        return false;
    }

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

void CameraManager::release(bool continuous) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    for (const auto &it : _gcImp) {
        auto ptr = it.second;
        ptr->stop();
    }
    _ready = continuous;
}

void CameraManager::clear(bool continuous) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    _gcImp.clear();
    _ready = continuous;
}

vector<string> CameraManager::getCameraKeys() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    vector<string> ret;
    for (const auto &it : _gcImp ) {
        ret.push_back(it.first);
    }
    return ret;
}

void CameraManager::loadSavedCameraInfo() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    // reset ticker to elapse time
    Ticker ticker;

    GET_CONFIG(string, mp4_save_path, Protocol::kMP4SavePath)
    GET_CONFIG(string, app_name, Record::kAppName)
    string record_path = File::absolutePath(app_name, mp4_save_path);

    auto invoker = [&](const string &path) {
        auto file = std::make_shared<FileRecorder<CameraStatistic, CameraStatisticHelper>>(path);
        if (!file->empty()) {
            CameraStatistic stats;
            if (file->load(stats)) {
                addCamera(stats.info, stats.option, stats.stream_map);
                DebugL << "Added saved info: " << stats.info.shortUrl();
                return;
            }
        }
        WarnL << "Saved file empty or invalid format: " << path << ". Ignore";
    };

    File::scanDir(record_path, [&](const string &path, bool isDir) {
        if (isDir) {
            auto saved_path = path + "/info.txt";
            if (File::fileExist(saved_path)) {
                DebugL << "Found saved file: " << saved_path << ". Loading camera info...";
                invoker(saved_path);
            }
        }
        return true;
    });
    InfoL << "Loaded all saved camera. Finished. " << formatDuration(ticker.elapsedTime()) << " elapsed";
}

} // namespace managerkit
