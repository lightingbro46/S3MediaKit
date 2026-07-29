#ifndef SPEAKER_SPEAKERMANAGER_H
#define SPEAKER_SPEAKERMANAGER_H

#include "GenericIPSpeakerImp.h"

#include <map>
#include <mutex>
#include <stdexcept>

namespace managerkit {

class SpeakerManager : public std::enable_shared_from_this<SpeakerManager> {
public:
    using Ptr = std::shared_ptr<SpeakerManager>;

    static SpeakerManager& Instance();
    ~SpeakerManager() = default;

    bool addSpeaker(DeviceTuple &tuple, SpeakerOption &option);

    bool addSpeaker(SpeakerStatisticImp::Ptr &stats);

    std::vector<std::string> getSpeakerKeys();

    bool delSpeaker(const std::string& key);

    void clear();

    void loadSavedSpeakerInfo();

private:
    SpeakerManager();

    void setReady(bool ready);

    bool isReady();
private:
    std::recursive_mutex _mtx;
    bool _ready = false;
    std::unordered_map<std::string, GenericIPSpeakerImp::Ptr> _gcImp;
};

} // namespace managerkit

#endif // SPEAKER_SPEAKERMANAGER_H