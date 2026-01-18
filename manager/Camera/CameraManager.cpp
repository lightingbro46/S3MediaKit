#include <algorithm>
#include "CameraManager.h"
#include "Util/util.h"
#include "Thread/WorkThreadPool.h"
#include "Local/StatisticRecorder.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(CameraManager)

CameraManager::CameraManager() {}

template <typename Pointer>
static bool equalStreamConfig(Pointer ptr, int type, unordered_map<int, StreamTuple> &stream_map) {
    if (stream_map.find(type) != stream_map.end()) {
        return ptr->hasStreamTuple(type) && ptr->getStreamTuple(type) == stream_map[type];
    } else {
        return !ptr->hasStreamTuple(type);
    }
}

template <typename Pointer>
static bool equalCameraConfig(Pointer ptr, CameraInfo &info, unordered_map<int, StreamTuple> &stream_map) {
    auto _info = ptr->getCameraInfo();
    if (!(_info == info)) {
        return false;
    }
    return ptr->getCameraInfo() == info && 
        equalStreamConfig(ptr, PrimaryStream, stream_map) &&
        equalStreamConfig(ptr, SecondaryStream, stream_map);
}

bool CameraManager::addCamera(CameraInfo &info, CameraOption &option, unordered_map<int, StreamTuple> &stream_map) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!isReady()) {
        TraceL << "Camera manager has not been ready";
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
            // configuration changed, remove old one
            _gcImp.erase(info.shortUrl());
        }
    }

    // create new one
    auto stats_imp = StatisticRecorder::Instance().getRecorder(info.device_id);
    auto imp = std::make_shared<GenericRtspCameraImp>(info, stream_map, stats_imp);
    imp->setCameraOption(option);
    _gcImp.emplace(info.shortUrl(), imp);
    return true;
}

bool CameraManager::addCamera(CameraStatisticImp::Ptr &stats) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    auto params = stats->getParams();
    auto info = params.info;
    auto stream_map = params.stream_map;
    auto option = params.option;
    auto it = _gcImp.find(info.shortUrl());
    if  (it != _gcImp.end()) {
        WarnL << "Camera " << info.shortUrl() << " already exist. Ignore add camera from statistics";
        return false;
    }

    // create new one
    auto imp = std::make_shared<GenericRtspCameraImp>(info, stream_map, stats);
    imp->setCameraOption(option);
    _gcImp.emplace(info.shortUrl(), imp);
    return true;
}

bool CameraManager::delCamera(const string &key) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!isReady()) {
        TraceL << "Camera manager has not been ready";
        return false;
    }

    //todo: check device active status
    auto it = _gcImp.find(key);
    if (it != _gcImp.end()) {
        auto imp = it->second;
        auto option = imp->getCameraOption();
        if (imp->isEnabled()) {
            // Device still enable active
            // Note: In cases of camera deletion, camera relocation, or camera failover returning to the main server, the failover mode is always enabled first.
            DebugL << "Device " << key << " is active: " << imp->isEnabled() << ". Enable failover mode";
            option.enableFailover = true;
            option.enableActive = false;
            imp->setCameraOption(option);
        } else {
            // Device disable active, check whether to keep device in list
            auto stats_imp = imp->getCameraStatisticImp();
            if (stats_imp) {
                if (option.enableFailover) {
                    DebugL << "Device " << key << " is not active and failover mode is enabled. Check storage data to decide whether to keep device";
                    auto params = stats_imp->getParams();
                    size_t total_archive_size = 0;
                    for (const auto &it : params.storage_map) {
                        total_archive_size += it.second.archiveSizeB;
                    }
                    if (total_archive_size > 0) {
                        DebugL << "Device " << key << " has archived data: " << format_bytes_human_readable(total_archive_size) << " bytes. Keep device in list";
                        return false;
                    }
                }
            }
           
            // Device disable active and disable failover mode, remove it out of list
            DebugL << "Device " << key << " is not active and has no archived data. Remove device out of list";
            stats_imp->remove();
            _gcImp.erase(key);
        }
        return true;
    }
    return false;
}

void CameraManager::clear() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    _gcImp.clear();
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
    std::weak_ptr<CameraManager> weak_self = shared_from_this();

    WorkThreadPool::Instance().getExecutor()->async([weak_self]() { 
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        // reset ticker to elapse time
        Ticker ticker;
    
        auto invoker = [weak_self](CameraStatisticImp::Ptr &stats) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->addCamera(stats);
            DebugL << "Added saved info: " << stats->getParams().info.shortUrl();
            return;
        };

        StatisticRecorder::Instance().loadSavedCameraStatistics(invoker);

        InfoL << "Loaded all saved camera. Finished. " << formatDuration(ticker.elapsedTime()) << " elapsed";
        strong_self->setReady(true);
    });
}

void CameraManager::setReady(bool ready) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    _ready = ready;
}

bool CameraManager::isReady() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    return _ready;
}

} // namespace managerkit
