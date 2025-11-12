#include "CameraManager.h"
#include "Util/util.h"
#include "Thread/WorkThreadPool.h"
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

bool CameraManager::addCamera(CameraInfo &info, CameraOption &option, unordered_map<int, StreamTuple> &stream_map, bool force) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!isReady() && !force) {
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
            // stop device before remove old one to store last media file if config change
            gc->stop();

            // set delay task to remove old one and create new one
            weak_ptr<CameraManager> weak_self = shared_from_this();
            EventPollerPool::Instance().getPoller()->doDelayTask(3000, [weak_self, info, stream_map, option]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return false;
                }
                // remove old one
                strong_self->_gcImp.erase(info.shortUrl());
                // create new one
                auto imp = std::make_shared<GenericRtspCameraImp>(info, stream_map);
                imp->setCameraOption(option);
                strong_self->_gcImp.emplace(info.shortUrl(), imp);
                return false;
            });

            return true;
        }
    }

    // create new one
    auto imp = std::make_shared<GenericRtspCameraImp>(info, stream_map);
    imp->setCameraOption(option);
    _gcImp.emplace(info.shortUrl(), imp);
    return true;
}

bool CameraManager::delCamera(const string &key, bool force) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!isReady() && !force) {
        TraceL << "Camera manager has not been ready";
        return false;
    }

    auto it = _gcImp.find(key);
    if (it != _gcImp.end()) {
        auto imp = it->second;
        auto option = imp->getCameraOption();
        auto params = imp->getParams();
        size_t total_storage_size = 0;
        total_storage_size += params.bm.recordAverageSizeB;
        for (const auto &it : params.storage_map) {
            total_storage_size += it.second.archiveIndexRecordCount > 0 ? it.second.archiveSizeB : 0;
        }

        if (total_storage_size > 0 || imp->isEnabled()) {
            // device still have data in storage or enable active
            // In cases of camera deletion, camera relocation, or camera failover returning to the main server, the failover mode is always enabled.
            DebugL << "Device still have remain data: " << format_bytes_human_readable(total_storage_size) << " or enable active: " << imp->isEnabled()
                   << ". Enable failover mode";
            option.enableFailover = true;
            option.enableActive = false;
            imp->setCameraOption(option);
        } else {
            // device do not have any data in storage and disable active
            // remove saved file before
            DebugL << "Device have no data and disable active. Remove device out of list";
            imp->remove();
            // remove device out of list 
            _gcImp.erase(key);
        }

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
    std::weak_ptr<CameraManager> weak_self = shared_from_this();

    WorkThreadPool::Instance().getExecutor()->async([weak_self]() { 
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
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
                    strong_self->addCamera(stats.info, stats.option, stats.stream_map, true);
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
                    DebugL << "Found saved file: " << saved_path << ". Loading...";
                    invoker(saved_path);
                }
            }
            return true;
        });
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
