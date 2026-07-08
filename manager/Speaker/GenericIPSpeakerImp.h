#ifndef SPEAKER_GENERICIPSPEAKERIMP_H
#define SPEAKER_GENERICIPSPEAKERIMP_H

#include "GenericIPSpeaker.h"
#include "SpeakerController.h"
#include "Local/StatisticRecorder.h"

namespace managerkit {

class GenericIPSpeakerImp : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<GenericIPSpeakerImp> {
public:
    using Ptr = std::shared_ptr<GenericIPSpeakerImp>;

    GenericIPSpeakerImp(const DeviceTuple &tuple, const SpeakerStatisticImp::Ptr &statistic);

    ~GenericIPSpeakerImp();

    bool isEnabled() const { return _enabled.load(); }

    GenericIPSpeaker::Ptr getSpeakerSource() const { return _src; }

    void setSpeakerOption(const SpeakerOption &option);

    const SpeakerOption& getSpeakerOption() const { return _option; } 

    SpeakerStatisticImp::Ptr getSpeakerStatisticImp();

    void playAudioFile(const std::string& speakerId, const std::string& fileId, const std::function<void(const toolkit::SockException &ex)> &cb);

    void validateCredential(const std::string& username, const std::string& password, const int port, const std::function<void(const toolkit::SockException &ex)> &cb);

public:
    //////////////DeviceSourceEvent related interface implementation////////////////
    toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &sender) override { return _poller; }

    void onControllerReady(DeviceSource &sender, bool connect, const std::string &status, const toolkit::Any &data) override;

private:
    void onAllResourcesReady();

    void setupController();

    void saveSpeakerOption(const SpeakerOption &option);

    void stop();

private:
    toolkit::EventPoller::Ptr _poller;
    bool _all_stream_ready = false;
    std::atomic<bool> _enabled { false };
    std::atomic<bool> _exit { false };
    GenericIPSpeaker::Ptr _src;
    SpeakerOption _option;
    SpeakerController::Ptr _controller;
    std::weak_ptr<SpeakerStatisticImp> _statistic;
};

} // namespace managerkit

#endif // SPEAKER_GENERICIPSPEAKERIMP_H