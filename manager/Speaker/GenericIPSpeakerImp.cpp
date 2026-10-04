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
    _poller = DeviceSource::nextDevicePoller();
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
        // An AUTO speaker keeps the protocol it was controlled with instead of probing again.
        if (auto strong_statistic = _statistic.lock()) {
            _controller->setLastControlProtocol(strong_statistic->getParams().device_caps.controlProtocol);
        }
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

    auto controller = _controller;
    bool supportsBackchannel = strong_statistic->getParams().device_caps.supportsAudioBackchannel;

    auto resultCb = [controller, speakerId, cb](bool ok, const std::string& data) {
        if (ok) {
            cb(SockException(Err_success, "Trigger play [" + speakerId + "]: OK", ApiErrCode::CODE_SUCCESS));
        } else {
            cb(SockException(Err_other, "Trigger play [" + speakerId + "]: FAIL (error: " + data + ")", ApiErrCode::CODE_PLAY_AUDIO_FAILED));
        }
    };

    auto playByBackchannel = [controller, fileId, resultCb]() {
        controller->playAudioFileByBackchannel(fileId, resultCb);
    };

    auto playByVendorApi = [controller, strong_statistic, fileId, resultCb]() {
        std::string fileRemoteId = strong_statistic->getAudioRemoteId(fileId);
        controller->playAudioFileByVendorApi(fileId, fileRemoteId,
            [strong_statistic, fileId, fileRemoteId, resultCb](bool ok, const std::string& data) {
                if (ok && fileRemoteId.empty()) {
                    // File does not exist on the speaker -> save remoteId to file
                    strong_statistic->setAudioRemoteId(fileId, data);
                }
                resultCb(ok, data);
            });
    };

    typedef std::function<void()> ModeHandler;
    typedef std::pair<int, ModeHandler> ModeHandlerPair;

    const std::vector<ModeHandlerPair> kModeHandlers = {
        { SpeakerOption::kAudioPlaybackModeAuto, supportsBackchannel ? ModeHandler(playByBackchannel) : ModeHandler(playByVendorApi) },
        { SpeakerOption::kAudioPlaybackModeOnvifBackchannel, ModeHandler(playByBackchannel) },
        { SpeakerOption::kAudioPlaybackModeVendorAudioFile, ModeHandler(playByVendorApi) },
    };

    if (_option.audioPlaybackMode == SpeakerOption::kAudioPlaybackModeOnvifBackchannel && !supportsBackchannel) {
        cb(SockException(Err_other, "Device does not support backchannel playback mode", ApiErrCode::CODE_PLAY_AUDIO_FAILED));
        return;
    }

    int mode = _option.audioPlaybackMode;
    std::vector<ModeHandlerPair>::const_iterator it = std::find_if(
        kModeHandlers.begin(), kModeHandlers.end(),
        [mode](const ModeHandlerPair& entry) { return entry.first == mode; });

    if (it == kModeHandlers.end()) {
        cb(SockException(Err_other, "Playback mode is not supported", ApiErrCode::CODE_PLAY_AUDIO_FAILED));
        return;
    }

    it->second();
}

void GenericIPSpeakerImp::reconnectController(const ReconnectCallback &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call reconnectController in it's owner poller");
    if (!_controller) {
        ReconnectResult result;
        result.finished = true;
        result.msg = "Speaker controller is not ready";
        cb(result);
        return;
    }
    _controller->reconnectNow(cb);
}

void GenericIPSpeakerImp::setAudioVolume(int volume, const std::function<void(const toolkit::SockException &ex)> &cb) {
    CHECK(getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "Can only call setAudioVolume in it's owner poller");
    if (!_controller) {
        WarnL << "Speaker " << _src->getUrl() << " controller is not ready. Ignoring set audio volume request";
        cb(SockException(Err_other, "Speaker controller is not ready", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }

    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Speaker " << _src->getUrl() << " statistic has been released. Ignore request";
        cb(SockException(Err_other, "Speaker statistic has been released", ApiErrCode::CODE_DEVICE_OFFLINE));
        return;
    }

    auto audioVolumeInfo = strong_statistic->getAudioVolumeInfo();
    if (!audioVolumeInfo.supportsControl()) {
        cb(SockException(Err_other, "The device is not support audio volume configuration", ApiErrCode::CODE_SET_AUDIO_VOLUME_FAILED));
        return;
    }

    int cVolume = volume;
    if (cVolume == audioVolumeInfo.configuredAudioVolume) {
        cb(SockException(Err_other, "The requested volume is the same as the current volume", ApiErrCode::CODE_SET_AUDIO_VOLUME_FAILED));
        return;
    }

    volume = audioVolumeInfo.toDeviceVolume(volume);
    auto controller = _controller;
    controller->setAudioVolume(volume, [controller, strong_statistic, volume, cVolume, cb](bool ok, const std::string& data) {
        if (ok) {
            strong_statistic->setAudioVolume(volume, cVolume);
            cb(SockException(Err_success, "Set audio volume OK", ApiErrCode::CODE_SUCCESS));
        } else {
            cb(SockException(Err_other, "Set audio volume FAIL (error: " + data + ")", ApiErrCode::CODE_SET_AUDIO_VOLUME_FAILED));
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
    // Compare what was stored: the statistic merges in the configured volume the controller lacks.
    bool caps_changed = caps && (strong_statistic->getParams().device_caps != prev_caps);
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
