#include "SpeakerController.h"
#include "Common/BlockingExecutors.h"
#include "ext-plugin/DeviceFactory.h"
#include "ext-plugin/Bosch/BoschIpSpeaker.h"
#include "Backchannel/BackchannelManager.h"
#include "Common/StrUtil.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

static const std::vector<DeviceBrand> kSupportedBrands = {
    DeviceBrand::BOSCH
};

SpeakerController::SpeakerController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {
    _device_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::device(), "speaker " + tuple.shortUrl());
}

void SpeakerController::probe(const std::string& ip, int port,
                                         const std::string& username, const std::string& password,
                                         std::function<void(const SockException&, IDevice::Ptr)> cb,
                                         size_t brandIndex) {
    if (brandIndex >= kSupportedBrands.size()) {
        WarnL << "Speaker scan failed for " << ip << ":" << port;
        cb(SockException(Err_other), nullptr);
        return;
    }

    DeviceBrand brand = kSupportedBrands[brandIndex];
    SpeakerConfig cfg;
    cfg.ip       = ip;
    cfg.port     = port;
    cfg.username = username;
    cfg.password = password;
    cfg.type     = DeviceType::Speaker;
    cfg.brand    = brand;
    IDevice::Ptr device;
    try {
        device = DeviceFactory::create(cfg);
    } catch (const std::exception &e) {
        TraceL << "Create device failed brand=" << static_cast<int>(brand) << " : " << e.what();
        probe(ip, port, username, password, cb, brandIndex + 1);
        return;
    }

    // `device` keeps its pending requests alive and they hold this callback: the callback drops
    // its copy on every path so the device is not kept alive by itself.
    device->connect([device, ip, port, username, password, cb, brand, brandIndex](bool success, const std::string& data) mutable {
        auto found = std::move(device);
        if (!success) {
            TraceL << "Connect failed brand=" << static_cast<int>(brand) << " : " << data;
            probe(ip, port, username, password, cb, brandIndex + 1);
            return;
        }

        InfoL << "Speaker found: " << ip << ":" << port << " brand=" << static_cast<int>(brand);
        cb(SockException(Err_success), found);
        // Probing only needs the device information: close the vendor session again.
        found->disconnect([ip, port](bool ok, const std::string& data) {
            if (!ok) {
                WarnL << "Disconnect speaker " << ip << ":" << port << " after probe failed: " << data;
            }
        });
    });
}

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

    _address         = address;
    _device_name     = option.name;
    _ctrl_option     = ControllerOption::from(option);

    setupDeviceController();
}

void SpeakerController::stopController() {
    CHECK(_poller->isCurrentThread(), "stopController must be called on the owner poller");

    onControllerReady(false, "Stop", nullptr);

    _timer_ctr.reset();
    _onvif_ctr.reset();
    _device_ctr.reset();
    _audio_playback_ctr = nullptr;
    _ready = false;
    BackchannelManager::Instance().unregisterDevice(_tuple.device_id);
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
    // Everything the device queue needs is copied here, on the owner poller.
    auto device_ctr = _device_ctr;
    auto onvif_username = _onvif_username;
    auto onvif_password = _ctrl_option.password;
    bool shared_dsp = getDeviceBrand(_ctrl_option.manufacturer) == DeviceBrand::BOSCH;
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();

    // ONVIF connect() is a blocking SOAP/HTTP call: run it on the speaker's serial device queue.
    _device_queue->post([weak_self, onvif_ctr, device_ctr, onvif_username, onvif_password, shared_dsp]() {
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
            caps->onvifProfile.deviceInfo                       = onvif_ctr->getDeviceInfo();
            caps->onvifProfile.audioOutputProfile               = onvif_ctr->getAudioOutputProfile();
            caps->onvifProfile.audioInputProfile                = onvif_ctr->getAudioInputProfile();
            auto audio_playback = dynamic_cast<IAudioPlayback*>(device_ctr.get());
            if (audio_playback) caps->audioVolumeInfo = audio_playback->getAudioSettings();
            caps->supportsAudioVolumeControl                    = caps->audioVolumeInfo.supportsControl();
            caps->supportsAudioBackchannel                      = onvif_ctr->supportAudioBackchannel();
            if (caps->supportsAudioBackchannel) {
                // Still on the device queue, which is where this blocking SOAP setup belongs.
                strong_self->registerAudioBackchannel(onvif_ctr, onvif_username, onvif_password, shared_dsp);
            }
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

void SpeakerController::setupDeviceController() {
    auto address  = _address;
    auto password = _ctrl_option.password;
    auto shortUrl = _tuple.shortUrl();

    auto brand = getDeviceBrand(_ctrl_option.manufacturer);
    auto requiresSeparateCredentail = requiresSeparateDeviceCredential(brand);
    if (!requiresSeparateCredentail) {
        _onvif_username = _ctrl_option.username;
        _onvif_ctr = std::make_shared<OnvifControl>(address, _onvif_username, password);
        DebugL << "Device does not require separate credentials. Creating Onvif controller for device: "
            << shortUrl << " (" << address << ")"
            << ", username: " << (_onvif_username.empty() ? "empty" : "******")
            << ", password: " << (password.empty() ? "empty" : "******");
        return;
    }

    SpeakerConfig cfg;
    cfg.id       = _tuple.device_id;
    cfg.name     = _device_name;
    cfg.ip       = _ctrl_option.ip;
    cfg.type     = DeviceType::Speaker;
    cfg.brand    = brand;
    cfg.port     = _ctrl_option.port;
    cfg.username = _ctrl_option.username;
    cfg.password = password;

    try {
        _device_ctr = DeviceFactory::create(cfg);
        _audio_playback_ctr = dynamic_cast<IAudioPlayback*>(_device_ctr.get());
        if (!_audio_playback_ctr) {
            _device_ctr.reset();
            WarnL << "Device does not support IAudioPlayback";
            return;
        }
    } catch (const std::exception& e) {
        WarnL << "Create API speaker controller " << _tuple.shortUrl() << " failed: " << e.what();
        return;
    }

    // ONVIF goes through the speaker.onvifUsername account provisioned below. The controller is
    // created now, on the owner poller: if the account does not exist yet, the periodic reconnect
    // in onManager() picks it up once provisioning has finished.
    GET_CONFIG(std::string, onvif_username, mediakit::Speaker::kOnvifUsername);
    _onvif_username = onvif_username;
    _onvif_ctr = std::make_shared<OnvifControl>(address, _onvif_username, password);
    DebugL << "Created Onvif controller: " << shortUrl << " (" << address << ")";

    // Provision the ONVIF account with the vendor API. The callbacks only use the device, never the
    // controller's members, which belong to _poller and may be replaced meanwhile. The device is
    // held weakly (its pending requests hold these callbacks); _device_ctr keeps it alive.
    std::weak_ptr<IDevice> weak_device = _device_ctr;
    auto username = _onvif_username;
    _device_ctr->connect([weak_device, username, password, shortUrl](bool ok, const std::string& data) {
        auto device = weak_device.lock();
        if (!device) {
            return;
        }
        if (!ok) {
            WarnL << "Connect to speaker " << shortUrl << " failed: " << data;
            return;
        }
        device->createOnvifUser(username, password, [weak_device, shortUrl](bool ok, const std::string& data) {
            auto device = weak_device.lock();
            if (!device) {
                return;
            }
            if (!ok) {
                WarnL << "Create onvif user on speaker " << shortUrl << " failed: " << data;
            }
            device->disconnect([shortUrl](bool ok, const std::string& data) {
                if (!ok) {
                    WarnL << "Disconnect speaker " << shortUrl << " failed after setup: " << data;
                }
            });
        });
    });
}

// ================== Vendor-specific operations ==================
// Every operation captures the device on the owner poller and its callbacks use only that
// capture: _device_ctr may be replaced or released by setupController()/stopController() while a
// request is in flight, and the callbacks run on the device's HTTP poller.

IDevice::Ptr SpeakerController::vendorDevice(const OnDeviceResult &cb) {
    CHECK(_poller->isCurrentThread(), "Speaker vendor operations must be called on the owner poller");
    if (!_device_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return nullptr;
    }
    return _device_ctr;
}

void SpeakerController::playAudioFileByVendorApi(const std::string& fileId, const std::string& fileRemoteId, OnDeviceResult cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    device->connect([device, fileId, fileRemoteId, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            std::string err = "Connection failed: " + data;
            cb(false, err);
            return;
        }

        auto playCb = autoDisconnect
            ? [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); }
            : cb;

        if (!fileRemoteId.empty()) {
            InfoL << "Known remoteId=" << fileRemoteId << ", playing immediately";
            dynamic_cast<IAudioPlayback*>(device.get())->playAudio(fileRemoteId, playCb);
            return;
        }

        auto file = AudioFileManager::Instance().getAudioFile(fileId);
        if (file.id.empty() || !file.downloaded) {
            cb(false, "Audio file is not ready for playback or does not exist: " + fileId);
            return;
        }
        TraceL << "Speaker " << device->getConfig().id << " does not contain file " << file.name << " - uploading before play";
        doUploadAudio(device, file,
            [device, cb, playCb](bool ok, const std::string& remoteId) {
                if (!ok) {
                    cb(false, "Upload failed: " + remoteId);
                    return;
                }
                dynamic_cast<IAudioPlayback*>(device.get())->playAudio(remoteId, playCb);
            }
        );
    });
}

void SpeakerController::uploadAudio(const AudioFile& file, OnDeviceResult cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    if (file.id.empty()) {
        cb(false, "File id cannot be empty");
        return;
    }

    device->connect([device, file, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }

        auto uploadCb = autoDisconnect
            ? [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); }
            : cb;

        doUploadAudio(device, file, uploadCb);
    });
}

void SpeakerController::stopAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    device->connect([device, remoteId, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto stopCb = autoDisconnect
            ? [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); }
            : cb;

        dynamic_cast<IAudioPlayback*>(device.get())->stopAudio(remoteId, stopCb);
    });
}

void SpeakerController::deleteAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    device->connect([device, remoteId, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto deleteCb = autoDisconnect
            ? [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); }
            : cb;

        dynamic_cast<IAudioPlayback*>(device.get())->deleteAudio(remoteId, deleteCb);
    });
}

void SpeakerController::listAudio(OnDeviceResult cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    device->connect([device, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto listCb = autoDisconnect
            ? [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); }
            : cb;

        dynamic_cast<IAudioPlayback*>(device.get())->listAudio(listCb);
    });
}

void SpeakerController::setAudioVolume(int volume, OnDeviceResult cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    device->connect([device, volume, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        auto setVolCb = autoDisconnect
            ? [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); }
            : cb;

        dynamic_cast<IAudioPlayback*>(device.get())->setAudioVolume(volume, setVolCb);
    });
}

void SpeakerController::doUploadAudio(const IDevice::Ptr &device, const AudioFile& file, OnDeviceResult cb) {
    if (!FileTypeUtil::isSupported(file.name, device->getBrand())) {
        std::string err = "File format is not supported: " + file.name;
        cb(false, err);
        return;
    }

    dynamic_cast<IAudioPlayback*>(device.get())->uploadAudio(file,
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

void SpeakerController::doDisconnect(const IDevice::Ptr &device, bool result, const std::string& resultData, OnDeviceResult cb) {
    device->disconnect([result, resultData, cb](bool ok, const std::string& data) {
        if (!ok) {
            WarnL << "Disconnect falied: " << data;
        }
        cb(result, resultData);
    });
}

// ================== Backchannel operations ==================
void SpeakerController::playAudioFileByBackchannel(const std::string& fileId, OnDeviceResult cb) {
    // supportsAudioBackchannel has already been verified by caller (GenericIPSpeakerImp::playAudioFile)
    if (!_ready.load()) {
        cb(false, "Device is offline");
        return;
    }

    auto file = AudioFileManager::Instance().getAudioFile(fileId);
    if (file.id.empty() || !file.downloaded) {
        cb(false, "Audio file is not ready for backchannel or does not exist: " + fileId);
        return;
    }

    GET_CONFIG(std::string, speaker_path, mediakit::Speaker::kSpeakerSavePath);
    GET_CONFIG(std::string, audio_file_dir, mediakit::Speaker::kAudioFilesDir);
    std::string filePath = file.localPath(toolkit::File::absolutePath(audio_file_dir, speaker_path));

    auto& backchannel = BackchannelManager::Instance();
    bool ok = backchannel.playFile(_tuple.device_id, filePath);
    cb(ok, ok ? "" : backchannel.getErrorMsg());
}

void SpeakerController::registerAudioBackchannel(const OnvifControl::Ptr &onvif_ctr, const std::string &username,
                                                 const std::string &password, bool sharedDspPipeline) {
    if (!onvif_ctr) {
        return;
    }

    std::string backchannelUri;
    if (!onvif_ctr->setupAudioBackchannel(backchannelUri, sharedDspPipeline) || backchannelUri.empty()) {
        WarnL << "Setup audio backchannel failed for device " << _tuple.shortUrl();
        return;
    }

    if (backchannelUri.find("@") == string::npos && !password.empty()) {
        backchannelUri = UriUtils::replaceCredentials(backchannelUri, username, password);
    }

    BackchannelManager::Instance().registerDevice(_tuple.device_id, backchannelUri);
}

} // namespace managerkit
