#include "SpeakerController.h"
#include "ext-plugin/DeviceFactory.h"
#include "ext-plugin/Bosch/BoschIpSpeaker.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

SpeakerController::SpeakerController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {}

void SpeakerController::createTimer() {
    weak_ptr<SpeakerController> weak_self = shared_from_this();
    _timer_ctr = std::make_shared<Timer>(
        10.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            strong_self->onManager();
            return true;
        },
        _poller
    );
}

void SpeakerController::setListener(const std::shared_ptr<DeviceSourceEvent> &listener) {
    setDelegate(listener);
}

bool SpeakerController::isReady() const {
    return _ready.load(); 
}

void SpeakerController::setupController(const SpeakerOption &option) {
    CHECK(_poller->isCurrentThread(), "setupController must be called on the owner poller");

    if (_onvif_ctr) {
        _onvif_ctr.reset();
        _ready = false;
    }

    if (_device_ctr) {
        _device_ctr.reset();
        _audio_playback_ctr = nullptr;
    }

    if (option.ip.empty() || option.port == 0) {
        WarnL << "Invalid speaker address, ip or port is empty";
        return;
    }

    string address = option.ip;
    if (option.autoWebPort) {
        address += ":" + (option.port > 0 ? to_string(option.port) : "80");
    } else {
        address += ":" + (option.webPort > 0 ? to_string(option.webPort) : "80");
    }

    // create new controller, currently only support onvif controller
    _onvif_ctr = std::make_shared<OnvifControl>(address, option.username, option.password);
    DebugL << "Created Onvif controller for device: " << _tuple.shortUrl() << " (" << address << ")"
        << ", username: " << (option.username.empty() ? "empty" : "******")
        << ", password: " << (option.password.empty() ? "empty" : "******");

    _address         = address;
    _ctrl_option     = ControllerOption::from(option);
    _device_name     = option.name;

    setupDeviceController(option);
}

void SpeakerController::stopController() {
    CHECK(_poller->isCurrentThread(), "stopController must be called on the owner poller");

    onControllerReady(false, "Stop", nullptr);

    _timer_ctr.reset();
    _onvif_ctr.reset();
    _device_ctr.reset();
    _audio_playback_ctr = nullptr;
    _ready = false;
    DebugL << "Closed speaker controller for device: " << _tuple.shortUrl() << " (" << _address << ")";
}

void SpeakerController::onManager() {
    // Always called from the Timer which runs on _poller - no dispatch needed.
    if (!_onvif_ctr) {
        onControllerReady(false, "Not supported", std::make_shared<DeviceCapabilities>());
        return;
    }

    if (time(nullptr) - _last_reconnect_time < 60) {
        // avoid reconnecting too frequently
        return;
    }
    _last_reconnect_time = time(nullptr);
    auto onvif_ctr = _onvif_ctr;
    auto weak_self = weak_from_this();

    WorkThreadPool::Instance().getPoller()->async([weak_self, onvif_ctr]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto tuple = strong_self->_tuple;
        auto address = strong_self->_address;
        // reconnect to device
        if (onvif_ctr->connect()) {
            InfoL << "Onvif controller of device " << tuple.shortUrl() << " (" << address << ") connected";
            auto caps = std::make_shared<DeviceCapabilities>();
            caps->isOnvifDevice = true;
            auto features = strong_self->getVendorFeatures(caps->onvifProfile.deviceInfo.manufacturer);
            caps->vendorFeatureSupport.requiresSeparateCredential = strong_self->_requires_device_credential;
            caps->vendorFeatureSupport.supportedVendorFeatures = features;
            caps->vendorFeatureSupport.supportsVendorFeatures = !features.empty();
            caps->onvifProfile.deviceInfo = onvif_ctr->getDeviceInfo();
            strong_self->onControllerReady(true, "connected", std::move(caps));
        } else {
            auto err_msg = onvif_ctr->getSoapErrMsg();
            WarnL << "Onvif controller of device " << tuple.shortUrl() << " (" << address << ") connect failed: " << err_msg;
            strong_self->onControllerReady(false, err_msg, nullptr);
        }
    });
}

void SpeakerController::onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps) {
    if (!_poller->isCurrentThread()) {
        auto self = shared_from_this();
        _poller->async([self, connect, status, caps]() {
            self->onControllerReady(connect, status, caps);
        });
        return;
    }

    _ready = connect;
    _err_msg = status;
    auto data = toolkit::Any(caps);
    DeviceSourceEventInterceptor::onControllerReady(DeviceSource::NullDeviceSource(), connect, status, data);
}

void SpeakerController::validateCredential(const std::string& username, const std::string& password, const int port, OnDeviceResult cb) {
    if (!_ready.load()) {
        cb(false, "Device controller is not ready");
        return;
    }

    SpeakerConfig cfg;
    cfg.id = _tuple.device_id;
    cfg.name = _device_name;
    cfg.ip = _ctrl_option.ip;
    cfg.port = port;
    cfg.username = username;
    cfg.password = password;
    cfg.type = DeviceType::Speaker;
    cfg.brand = getDeviceBrand(_ctrl_option.manufacturer);
    IDevice::Ptr speaker;
    try {
        speaker = DeviceFactory::create(cfg);
    } catch (const std::exception& e) {
        WarnL << "Create API speaker controller " << _tuple.shortUrl() << " failed: " << e.what();
        cb(false, std::string("Create API speaker controller failed: ") + e.what());
        return;
    }

    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    speaker->connect([weak_self, speaker, cb](bool ok, const std::string& data) mutable {
        auto self = weak_self.lock();
        if (!self) {
            speaker.reset();
            return;
        }

        if (ok) {
            speaker->disconnect([weak_self, speaker, cb](bool disconnectOk, const std::string& disconnectData) mutable {
                if (!disconnectOk) {
                    WarnL << "Disconnect failed: " << disconnectData;
                }
                auto self = weak_self.lock();
                if (!self) {
                    speaker.reset();
                    return;
                }
                cb(true, "Connection success");
            });
        } else {
            speaker.reset();
            cb(false, "Connection failed: " + data);
        }
    });
}

void SpeakerController::playAudio(const std::string& fileId, const std::string& fileRemoteId, OnDeviceResult cb, bool autoDisconnect) {
    if (!_device_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return;
    }

    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_ctr->connect([weak_self, fileId, fileRemoteId, cb, autoDisconnect](bool ok, const std::string& data) mutable {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }

        if (!ok) {
            std::string err = "Connection failed: " + data;
            cb(false, err);
            return;
        }

        if (!fileRemoteId.empty()) {
            InfoL << "Known remoteId=" << fileRemoteId << ", playing immediately";
            auto playCb = autoDisconnect
                ? [weak_self, cb](bool ok, const std::string& data) {
                    auto self = weak_self.lock();
                    if (!self) {
                        return;
                    }
                    self->doDisconnect(ok, data, cb);
                }
                : cb;
            self->_audio_playback_ctr->playAudio(fileRemoteId, playCb);
            return;
        }

        auto file = AudioFileManager::Instance().getAudioFile(fileId);
        if (file.id.empty() || !file.downloaded) {
            cb(false, "Audio file is not ready for playback or does not exist: " + fileId);
            return;
        }
        TraceL << "Speaker " << self->_device_ctr->getConfig().id << " does not contain file " << file.name << " - uploading before play";
        self->doUploadAudio(file,
            [weak_self, file, cb, autoDisconnect](bool ok, const std::string& remoteId) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                if (!ok) {
                    cb(false, "Upload failed: " + remoteId);
                    return;
                }
                auto playCb = autoDisconnect
                    ? [weak_self, cb](bool ok, const std::string& data) {
                        auto self = weak_self.lock();
                        if (!self) {
                            return;
                        }
                        self->doDisconnect(ok, data, cb);
                    }
                    : cb;
                self->_audio_playback_ctr->playAudio(remoteId, playCb);
            }
        );
    });
}

void SpeakerController::uploadAudio(const AudioFile& file, OnDeviceResult cb, bool autoDisconnect) {
    if (!_device_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return;
    }

    if (file.id.empty()) {
        cb(false, "File id cannot be empty");
        return;
    }

    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_ctr->connect([weak_self, file, cb, autoDisconnect](bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }

        auto uploadCb = autoDisconnect
            ? [weak_self, cb](bool ok, const std::string& data) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                self->doDisconnect(ok, data, cb);
            }
            : cb;

        self->doUploadAudio(file, uploadCb);
    });
}

void SpeakerController::stopAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect) {
    if (!_device_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return;
    }

    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_ctr->connect([weak_self, remoteId, cb, autoDisconnect](bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto stopCb = autoDisconnect
            ? [weak_self, cb](bool ok, const std::string& data) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                self->doDisconnect(ok, data, cb);
            }
            : cb;

        self->_audio_playback_ctr->stopAudio(remoteId, stopCb);
    });
}

void SpeakerController::deleteAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect) {
    if (!_device_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return;
    }

    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_ctr->connect([weak_self, remoteId, cb, autoDisconnect](bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto deleteCb = autoDisconnect
            ? [weak_self, cb](bool ok, const std::string& data) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                self->doDisconnect(ok, data, cb);
            }
            : cb;

        self->_audio_playback_ctr->deleteAudio(remoteId, deleteCb);
    });
}

void SpeakerController::listAudio(OnDeviceResult cb, bool autoDisconnect) {
    if (!_device_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return;
    }

    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_ctr->connect([weak_self, cb, autoDisconnect](bool ok, const std::string& data) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto listCb = autoDisconnect
            ? [weak_self, cb](bool ok, const std::string& data) {
                auto self = weak_self.lock();
                if (!self) {
                    return;
                }
                self->doDisconnect(ok, data, cb);
            }
            : cb;

        self->_audio_playback_ctr->listAudio(listCb);
    });
}

void SpeakerController::doUploadAudio(const AudioFile& file, OnDeviceResult cb) {
    if (!_device_ctr || !_audio_playback_ctr) {
        cb(false, "API controller has been released. Ignoring audio file upload request");
        return;
    }

    if (!FileTypeUtil::isSupported(file.name, _device_ctr->getBrand())) {
        std::string err = "File format is not supported: " + file.name;
        cb(false, err);
        return;
    }

    _audio_playback_ctr->uploadAudio(file,
        [file, cb](bool ok, const std::string& data) {
            if (!ok) {
                cb(false, data);
                return;
            }
            InfoL << "Upload OK, localId=" << file.id << " remoteId=" << data;
            cb(true, data);
        }
    );
}

void SpeakerController::doDisconnect(bool result, const std::string& resultData, OnDeviceResult cb) {
    if (!_device_ctr || !_audio_playback_ctr) {
        cb(false, "API controller has been released. Ignoring disconnect request");
        return;
    }
    _device_ctr->disconnect([result, resultData, cb](bool ok, const std::string& data) {
        if (!ok) {
            WarnL << "Disconnect falied: " << data;
        }
        cb(result, resultData);
    });
}

void SpeakerController::setupDeviceController(const SpeakerOption &option) {
    auto brand = getDeviceBrand(option.manufacturer);
    if (brand == DeviceBrand::UNKNOWN) {
        WarnL << "Device brand is not supported";
        return;
    }
    _requires_device_credential = requiresSeparateDeviceCredential(brand);

    if (_requires_device_credential && !option.separateCredentialConfigured) {
        TraceL << "Separate credentials have not been configured yet. Ignore create API speaker controller";
        return;
    }

    SpeakerConfig cfg;
    cfg.id       = _tuple.device_id;
    cfg.name     = option.name;
    cfg.ip       = option.ip;
    cfg.type     = DeviceType::Speaker;
    cfg.brand    = brand;

    if (_requires_device_credential) {
        cfg.port     = option.devicePort;
        cfg.username = option.deviceUsername;
        cfg.password = option.devicePassword;
    } else {
        // ONVIF credential
        cfg.port     = option.port;
        cfg.username = option.username;
        cfg.password = option.password;
    }

    try {
        _device_ctr = DeviceFactory::create(cfg);
        _audio_playback_ctr = dynamic_cast<IAudioPlayback*>(_device_ctr.get());
        if (!_audio_playback_ctr) {
            _device_ctr.reset();
            WarnL << "Device does not support IAudioPlayback";
        }
    } catch (const std::exception& e) {
        WarnL << "Create API speaker controller " << _tuple.shortUrl() << " failed: " << e.what();
    }
}

std::vector<std::string> SpeakerController::getVendorFeatures(const std::string& manufacturer) {
    if (manufacturer == "Bosch") {
        return {
            VendorFeatureSupport::toString(SupportedFeatures::PlayAudioFile)
        };
    }

    return {};
}

} // namespace managerkit
