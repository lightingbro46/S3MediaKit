#include "GenericIPSpeakerImp.h"
#include "server/WebApiErrCode.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericIPSpeakerImp::GenericIPSpeakerImp(const DeviceTuple &tuple, const SpeakerStatisticImp::Ptr &statistic)
    : _statistic(statistic) {
    _poller = EventPollerPool::Instance().getPoller();
    CHECK(_poller != nullptr, "Failed to get poller for GenericIPSpeakerImp");
    _src = std::make_shared<GenericIPSpeaker>(tuple);
    statistic->setDeviceTuple(tuple);
}

GenericIPSpeakerImp::~GenericIPSpeakerImp() {
    _exit = true;
    if (_enabled.load()) {
        if (_poller->isCurrentThread()) {
            stop();
        } else {
            _poller->sync([this]() { stop(); });
        }
    }
}

void GenericIPSpeakerImp::setSpeakerOption(const SpeakerOption& option) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setSpeakerOption in it's owner poller");
    if (_option == option) {
        return; // No change
    }
    _option = option;
    // _enabled = option.enableActive;
    _enabled = true;
    saveSpeakerOption(option);
    
    if (!_enabled.load()) {
        stop();
        return;
    }

    setupController();
    onAllResourcesReady();
}

void GenericIPSpeakerImp::onAllResourcesReady() {
    if (_exit.load()) {
        return;
    }
    if (_src) {
        // set this as listener to handle device events
        _src->setListener(shared_from_this());
        // trigger device registration event
        _src->regist();
    }
    if (_controller) {
        _controller->setListener(shared_from_this());
    }
}

void GenericIPSpeakerImp::setupController() {
    if (!_controller) {
        _controller = std::make_shared<SpeakerController>(_src->getDeviceTuple(), _poller);
        _controller->createTimer();
    }
    _controller->setupController(_option);
}

void GenericIPSpeakerImp::stop() {
    if (_controller) {
        _controller->stopController();
    }
}

SpeakerStatisticImp::Ptr GenericIPSpeakerImp::getSpeakerStatisticImp() {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Speaker " << _src->getUrl() << " statistic has been released. Ignore get statistic request";
        return nullptr;
    }
    return strong_statistic;
}

void GenericIPSpeakerImp::playAudioFile(const std::string& speakerId, const std::string& fileId, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call playAudioFile in it's owner poller");
    if (!_controller) {
        WarnL << "Speaker " << _src->getUrl() << " controller is not ready. Ignoring play audio request";
        cb(SockException(Err_other, "Speaker controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }

    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Speaker " << _src->getUrl() << " statistic has been released. Ignore request";
        cb(SockException(Err_other, "Speaker statistic has been released", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }

    if (!strong_statistic->isFeatureSupported(SupportedFeatures::PlayAudioFile)) {
        WarnL << "Speaker " << _src->getUrl() << " does not support this feature";
        cb(SockException(Err_other, "Speaker does not support this feature", ApiErrCode::CODE_PLAY_AUDIO_FAILED));
        return;
    }

    std::string fileRemoteId = strong_statistic->getAudioRemoteId(fileId);
    auto controller = _controller;

    controller->playAudio(fileId, fileRemoteId,
        [controller, strong_statistic, speakerId, fileId, fileRemoteId, cb](bool ok, const std::string& data) {
            if (ok) {
                if (fileRemoteId.empty()) {
                    // File does not exist on the speaker -> save remoteId to file
                    strong_statistic->setAudioRemoteId(fileId, data);
                }
                cb(SockException(Err_success, "Trigger play [" + speakerId + "]: OK", ApiErrCode::CODE_SUCCESS));
            } else {
                cb(SockException(Err_other, "Trigger play [" + speakerId + "]: FAIL (error: " + data + ")", ApiErrCode::CODE_PLAY_AUDIO_FAILED));
            }
        });
}

void GenericIPSpeakerImp::validateCredential(const std::string& username, const std::string& password, const int port, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call validateCredential in it's owner poller");
    if (!_controller) {
        WarnL << "Speaker " << _src->getUrl() << " controller is not ready. Ignoring request";
        cb(SockException(Err_other, "Speaker controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }

    _controller->validateCredential(username, password, port, [cb](bool ok, const std::string& data) {
        if (ok) {
            cb(SockException(Err_success, data, ApiErrCode::CODE_SUCCESS));
        } else {
            cb(SockException(Err_other, data, ApiErrCode::CODE_DEVICE_NOT_FOUND));
        }
    });
}

void GenericIPSpeakerImp::saveSpeakerOption(const SpeakerOption &option) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Speaker " << _src->getUrl() << " statistic has been released. Ignore Speaker option save";
        return;
    }
    strong_statistic->setSpeakerOption(option);
}

void GenericIPSpeakerImp::onControllerReady(DeviceSource &sender, bool connect, const std::string &status, const toolkit::Any &data) {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Speaker " << _src->getUrl() << " statistic has been released. Ignore controller ready event";
        return;
    }

    auto old_params = strong_statistic->getParams();
    bool prev_connect = old_params.connect;
    DeviceCapabilities prev_caps = old_params.device_caps;

    const DeviceCapabilities *caps = nullptr;
    if (data.is<DeviceCapabilities>()) {
        caps = &data.get<DeviceCapabilities>();
    }
    strong_statistic->addDeviceCapabilities(connect, status, caps);

    bool is_restart = !prev_connect && connect;
    bool caps_changed = caps && (*caps != prev_caps);
    if (!is_restart && !caps_changed) {
        return;
    }

    std::weak_ptr<GenericIPSpeakerImp> weak_self = shared_from_this();
    WorkThreadPool::Instance().getPoller()->async([weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto &src = *strong_self->_src;
        auto strong_statistic = strong_self->_statistic.lock();
        if (strong_statistic) {
            auto params = strong_statistic->getParams();
            NOTICE_EMIT(BroadcastDeviceCapsChangedArgs, Broadcast::kBroadcastDeviceCapsChanged, params.device_caps, src);
        }
    });
}

} // namespace managerkit
