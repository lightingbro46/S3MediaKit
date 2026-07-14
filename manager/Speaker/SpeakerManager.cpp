#include "SpeakerManager.h"
#include "Local/StatisticRecorder.h"
#include "Server/ResourceMonitor.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(SpeakerManager)

SpeakerManager::SpeakerManager() {}

bool SpeakerManager::addSpeaker(DeviceTuple &tuple, SpeakerOption &option) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!isReady()) {
        TraceL << "Camera manager has not been ready";
        return false;
    }

    auto it = _gcImp.find(tuple.shortUrl());
    if  (it != _gcImp.end()) {
        // device tuple already exist
        auto gc = it->second;
        if (gc) {
            if (equalDeviceTuple(gc->getSpeakerSource()->getDeviceTuple(), tuple)) {
                auto poller = gc->getOwnerPoller(DeviceSource::NullDeviceSource());
                poller->async([gc, option]() {
                    gc->setSpeakerOption(option);
                });
                return true;
            }
            // configuration changed, remove old one
            _gcImp.erase(tuple.shortUrl());
        }
    }

    // create new one
    auto stats_imp = StatisticRecorder::Instance().getSpeakerRecorder(tuple.device_id);
    auto imp = std::make_shared<GenericIPSpeakerImp>(tuple, stats_imp);
    auto poller = imp->getOwnerPoller(DeviceSource::NullDeviceSource());
    poller->async([imp, option]() {
        imp->setSpeakerOption(option);
    });
    _gcImp.emplace(tuple.shortUrl(), imp);
    return true;
}

bool SpeakerManager::addSpeaker(SpeakerStatisticImp::Ptr &stats) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    auto params = stats->getParams();
    auto tuple = params.tuple;
    auto option = params.option;
    auto it = _gcImp.find(tuple.shortUrl());
    if  (it != _gcImp.end()) {
        WarnL << "Speaker " << tuple.shortUrl() << " already exist. Ignore add speaker from statistics";
        return false;
    }

    // create new one
    auto imp = std::make_shared<GenericIPSpeakerImp>(tuple, stats);
    auto poller = imp->getOwnerPoller(DeviceSource::NullDeviceSource());
    poller->async([imp, option]() { 
        imp->setSpeakerOption(option); 
    });
    _gcImp.emplace(tuple.shortUrl(), imp);
    return true;
}

bool SpeakerManager::delSpeaker(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!isReady()) {
        TraceL << "Camera manager has not been ready";
        return false;
    }

    auto it = _gcImp.find(key);
    if (it != _gcImp.end()) {
        auto imp = it->second;
        auto option = imp->getSpeakerOption();
        if (imp->isEnabled()) {
            DebugL << "Device " << key << " is active: " << imp->isEnabled() << ". Enable failover mode";
            option.enableActive = false;
            imp->setSpeakerOption(option);
        } else {
            auto stats_imp = imp->getSpeakerStatisticImp();
            DebugL << "Device " << key << " is not active and has no archived data. Remove device out of list";
            stats_imp->remove();
            _gcImp.erase(key);
        }
        return true;
    }
    return false;
}

void SpeakerManager::clear() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    _gcImp.clear();
}

std::vector<std::string> SpeakerManager::getSpeakerKeys() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    vector<string> ret;
    for (const auto &it : _gcImp) {
        ret.push_back(it.first);
    }
    return ret;
}

void SpeakerManager::loadSavedSpeakerInfo() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    std::weak_ptr<SpeakerManager> weak_self = shared_from_this();

    WorkThreadPool::Instance().getPoller()->async([weak_self]() { 
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        // reset ticker to elapse time
        Ticker ticker;
    
        auto invoker = [weak_self](SpeakerStatisticImp::Ptr &stats) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->addSpeaker(stats);
            DebugL << "Added saved info: " << stats->getParams().tuple.shortUrl();
            return;
        };

        StatisticRecorder::Instance().loadSavedSpeakerStatistics(invoker);

        InfoL << "Loaded all saved speaker. Finished. " << format_duration_verbose(ticker.elapsedTime()) << " elapsed";
        strong_self->setReady(true);
    });
}

void SpeakerManager::setReady(bool ready) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    _ready = ready;
}

bool SpeakerManager::isReady() {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    return _ready;
}

} // namespace managerkit