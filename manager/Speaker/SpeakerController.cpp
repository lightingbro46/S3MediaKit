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

DeviceControl::Ptr SpeakerController::createDeviceControl(ControlProtocol protocol, const std::string &address,
                                                                const std::string &username, const std::string &password) {
    return makeDeviceControl(protocol, address, username, password);
}

void SpeakerController::setupController(const SpeakerOption &option) {
    CHECK(_poller->isCurrentThread(), "setupController must be called on the owner poller");

    if (_configured) {
        _ready = false;
        if (controlProtocolSetting(option.controlProtocol) != controlProtocolSetting(_ctrl_option.controlProtocol)) {
            // Nothing may keep using the old protocol's controller once the switch is published.
            onControllerReady(false, "Control protocol changed", nullptr);
            BackchannelManager::Instance().unregisterDevice(_tuple.device_id);
        }
        // A changed address or credentials may mean another protocol now: AUTO probes again.
        _lock.unlock();
    }
    _device_ctr.reset();
    _configured = false;
    _probing = false;
    _pacer.reset();
    ++_ctrl_generation;
    ReconnectResult pending;
    pending.msg = "Device settings changed, connecting with the new settings";
    finishReconnect(pending);

    if (_vendor_ctr) {
        _vendor_ctr.reset();
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
    _lock.setSetting(option.controlProtocol);

    if (!setupDeviceController()) {
        return;
    }
    _configured = true;
    ControlProtocol protocol = _lock.fixedOrLocked();
    if (protocol != ControlProtocol::NONE) {
        _device_ctr = createDeviceControl(protocol, address, _ctrl_username, option.password);
    }
    DebugL << "Controller of speaker: " << _tuple.shortUrl() << " (" << address << ")"
        << ", protocol setting: " << controlProtocolSetting(option.controlProtocol)
        << ", using: " << (protocol == ControlProtocol::NONE ? "probe" : controlProtocolToString(protocol))
        << ", username: " << (_ctrl_username.empty() ? "empty" : "******")
        << ", password: " << (option.password.empty() ? "empty" : "******");
}

void SpeakerController::setLastControlProtocol(const std::string &protocol) {
    CHECK(_poller->isCurrentThread(), "setLastControlProtocol must be called on the owner poller");
    _lock.restore(controlProtocolFromString(protocol));
}

void SpeakerController::stopController() {
    CHECK(_poller->isCurrentThread(), "stopController must be called on the owner poller");

    onControllerReady(false, "Stop", nullptr);

    _timer_ctr.reset();
    _device_ctr.reset();
    _vendor_ctr.reset();
    _audio_playback_ctr = nullptr;
    _configured = false;
    _probing = false;
    ++_ctrl_generation;
    _ready = false;
    BackchannelManager::Instance().unregisterDevice(_tuple.device_id);
    DebugL << "Closed speaker controller for device: " << _tuple.shortUrl() << " (" << _address << ")";
}

SpeakerController::ConnectContext SpeakerController::connectContext() const {
    ConnectContext ctx;
    ctx.vendor = _vendor_ctr;
    ctx.username = _ctrl_username;
    ctx.password = _ctrl_option.password;
    ctx.sharedDsp = getDeviceBrand(_ctrl_option.manufacturer) == DeviceBrand::BOSCH;
    ctx.generation = _ctrl_generation.load();
    return ctx;
}

void SpeakerController::onManager() {
    // Always called from the Timer which runs on _poller - no dispatch needed.
    if (!_configured || (!_device_ctr && !_lock.isAuto())) {
        onControllerReady(false, "Not supported", std::make_shared<DeviceCapabilities>());
        return;
    }

    GET_CONFIG(int, auth_retry_sec, mediakit::Speaker::kControlAuthRetrySec);
    const uint64_t now = time(nullptr);
    if (_probing || !_pacer.due(now, 60, auth_retry_sec)) {
        // avoid reconnecting too frequently, and logging in with a rejected password even less often
        return;
    }
    _pacer.onAttempt(now);
    if (!_device_ctr) {
        startProtocolProbe();
        return;
    }
    auto device_ctr = _device_ctr;
    // Everything the device queue needs is copied here, on the owner poller.
    auto ctx = connectContext();
    // A speaker that is up only needs one request to confirm it, not the whole connect() handshake.
    const bool was_ready = _ready.load() && device_ctr->isConnected();
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();

    // connect() is a blocking SOAP/HTTP call: run it on the speaker's serial device queue.
    _device_queue->post([weak_self, device_ctr, ctx, was_ready]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        ReconnectResult result;
        result.finished = true;
        result.protocol = controlProtocolToString(device_ctr->protocol());
        if (was_ready && device_ctr->probeConnection()) {
            result.connected = true;
            result.msg = "connected";
            strong_self->postReconnectResult(result);
            return;
        }
        auto tuple = strong_self->_tuple;
        auto address = strong_self->_address;
        // reconnect to device
        if (device_ctr->connect()) {
            strong_self->publishConnected(device_ctr, ctx);
            result.connected = true;
            result.msg = "connected";
        } else {
            auto err_msg = device_ctr->lastError();
            auto err = device_ctr->lastConnectError();
            WarnL << controlProtocolToString(device_ctr->protocol()) << " controller of speaker " << tuple.shortUrl()
                  << " (" << address << ") connect failed (" << connectErrorToString(err) << "): " << err_msg;
            std::weak_ptr<SpeakerController> weak = strong_self;
            strong_self->_poller->async([weak, device_ctr, err]() {
                if (auto self = weak.lock()) {
                    self->onConnectResult(device_ctr, err);
                }
            }, false);
            strong_self->onControllerReady(false, err_msg, nullptr);
            result.error = err;
            result.msg = err_msg;
        }
        // Queued after onConnectResult: a waiter sees the pacer as this attempt left it.
        strong_self->postReconnectResult(result);
    });
}

void SpeakerController::reconnectNow(const ReconnectCallback &cb) {
    CHECK(_poller->isCurrentThread(), "reconnectNow must be called on the owner poller");
    if (!_configured || (!_device_ctr && !_lock.isAuto())) {
        ReconnectResult result;
        result.finished = true;
        result.msg = "Device control is not configured";
        cb(result);
        return;
    }
    InfoL << "Reconnect of speaker " << _tuple.shortUrl() << " (" << _address << ") requested";
    _reconnect_waiters.push_back(cb);
    _pacer.reset();
    // A probe already running answers the waiter when it ends.
    onManager();
}

void SpeakerController::finishReconnect(const ReconnectResult &result) {
    std::vector<ReconnectCallback> waiters;
    waiters.swap(_reconnect_waiters);
    for (auto &cb : waiters) {
        cb(result);
    }
}

void SpeakerController::postReconnectResult(const ReconnectResult &result) {
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _poller->async([weak_self, result]() {
        if (auto self = weak_self.lock()) {
            self->finishReconnect(result);
        }
    }, false);
}

void SpeakerController::publishConnected(const DeviceControl::Ptr &device_ctr, const ConnectContext &ctx) {
    InfoL << controlProtocolToString(device_ctr->protocol()) << " controller of speaker " << _tuple.shortUrl()
          << " (" << _address << ") connected";
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _poller->async([weak_self, device_ctr]() {
        if (auto self = weak_self.lock()) {
            self->onConnectResult(device_ctr, ConnectError::NONE);
        }
    }, false);

    auto caps = std::make_shared<DeviceCapabilities>();
    caps->isOnvifDevice = device_ctr->protocol() == ControlProtocol::ONVIF;
    caps->controlProtocol = controlProtocolToString(device_ctr->protocol());
    caps->onvifProfile.deviceInfo           = device_ctr->getDeviceInfo();
    caps->onvifProfile.audioOutputProfile   = device_ctr->getAudioOutputProfile();
    caps->onvifProfile.audioInputProfile    = device_ctr->getAudioInputProfile();
    auto audio_playback = dynamic_cast<IAudioPlayback*>(ctx.vendor.get());
    if (audio_playback) caps->audioVolumeInfo = audio_playback->getAudioSettings();
    caps->supportsAudioVolumeControl        = caps->audioVolumeInfo.supportsControl();
    // ONVIF: RTSP backchannel, ISAPI: two-way audio, PSIA: none.
    caps->supportsAudioBackchannel          = device_ctr->supportsAudioBackchannel();
    if (caps->supportsAudioBackchannel) {
        // Still on the device queue, which is where this blocking setup belongs.
        registerAudioBackchannel(device_ctr, ctx);
    }
    onControllerReady(true, "connected", std::move(caps));
}

void SpeakerController::startProtocolProbe() {
    _probing = true;
    GET_CONFIG(string, order_str, mediakit::Speaker::kControlAutoOrder);
    const auto order = parseProtocolOrder(order_str);
    const string address = _address;
    auto ctx = connectContext();
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_queue->post([weak_self, order, address, ctx]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        auto result = probeControlProtocols(order, [&](ControlProtocol protocol) {
            return self->createDeviceControl(protocol, address, ctx.username, ctx.password);
        }, self->_tuple.shortUrl() + " (" + address + ")");
        DeviceControl::Ptr winner = result.winner;
        const std::string err_msg = result.err_msg;
        const ConnectError err = result.err;
        if (winner && self->_ctrl_generation.load() != ctx.generation) {
            winner.reset(); // the option changed meanwhile
        }
        const uint64_t generation = ctx.generation;
        std::weak_ptr<SpeakerController> weak = self;
        self->_poller->async([weak, winner, generation, err_msg, err]() {
            if (auto strong = weak.lock()) {
                strong->onProbeDone(winner, generation, err_msg, err);
            }
        }, false);
        if (winner) {
            self->publishConnected(winner, ctx);
        } else {
            self->onControllerReady(false, err_msg, nullptr);
        }
    });
}

void SpeakerController::onProbeDone(const DeviceControl::Ptr &device_ctr, uint64_t generation, const std::string &err_msg,
                                    ConnectError err) {
    if (generation != _ctrl_generation.load()) {
        return;
    }
    _probing = false;
    ReconnectResult result;
    result.finished = true;
    if (!device_ctr) {
        WarnL << "No control protocol for speaker " << _tuple.shortUrl() << " (" << _address << "): " << err_msg;
        onAuthResult(err);
        result.error = err;
        result.msg = err_msg;
        finishReconnect(result);
        return;
    }
    _device_ctr = device_ctr;
    result.connected = true;
    result.protocol = controlProtocolToString(device_ctr->protocol());
    result.msg = "connected";
    // onConnectResult (queued by publishConnected) clears the credentials verdict.
    finishReconnect(result);
}

void SpeakerController::onConnectResult(const DeviceControl::Ptr &device_ctr, ConnectError err) {
    if (!device_ctr || device_ctr != _device_ctr) {
        return;
    }
    onAuthResult(err);
    if (err == ConnectError::NONE) {
        const ControlProtocol previous = _lock.last();
        if (_lock.onConnected(device_ctr->protocol())) {
            WarnL << "Speaker " << _tuple.shortUrl() << " (" << _address << "): control protocol changed from "
                  << controlProtocolToString(previous) << " to " << controlProtocolToString(device_ctr->protocol());
        }
        return;
    }
    GET_CONFIG(int, reprobe_failures, mediakit::Speaker::kControlReprobeFailures);
    if (_lock.onConnectError(err, reprobe_failures)) {
        unlockProtocol(string("Speaker no longer answers ") + controlProtocolToString(device_ctr->protocol()));
    }
}

void SpeakerController::unlockProtocol(const std::string &reason) {
    WarnL << "Speaker " << _tuple.shortUrl() << " (" << _address << "): " << reason << ", probing the control protocols again";
    _ready = false;
    onControllerReady(false, "Control protocol changed", nullptr);
    BackchannelManager::Instance().unregisterDevice(_tuple.device_id);
    _device_ctr.reset();
    _lock.unlock();
    _pacer.reset();
    ++_ctrl_generation;
}

void SpeakerController::onAuthResult(ConnectError err) {
    const bool was_rejected = _pacer.authFailed();
    _pacer.onResult(err);
    if (_pacer.authFailed() && !was_rejected) {
        GET_CONFIG(int, auth_retry_sec, mediakit::Speaker::kControlAuthRetrySec);
        WarnL << "Speaker " << _tuple.shortUrl() << " (" << _address << ") rejected the credentials: next attempt in "
              << auth_retry_sec << " s, or at once through /media/mserver/device/reconnect";
    }
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

bool SpeakerController::setupDeviceController() {
    auto password = _ctrl_option.password;
    auto shortUrl = _tuple.shortUrl();

    auto brand = getDeviceBrand(_ctrl_option.manufacturer);
    auto requiresSeparateCredentail = requiresSeparateDeviceCredential(brand);
    if (!requiresSeparateCredentail) {
        _ctrl_username = _ctrl_option.username;
        return true;
    }

    // The vendor plugin provisions an ONVIF account and its backchannel needs ONVIF: such a speaker is
    // always controlled with ONVIF, whatever the option says.
    if (controlProtocolFromString(_ctrl_option.controlProtocol) != ControlProtocol::ONVIF &&
        controlProtocolFromString(_ctrl_option.controlProtocol) != ControlProtocol::NONE) {
        WarnL << "Speaker " << shortUrl << " needs ONVIF for its vendor plugin, ignoring control protocol "
              << _ctrl_option.controlProtocol;
    }
    _lock.setSetting("ONVIF");

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
        _vendor_ctr = DeviceFactory::create(cfg);
        _audio_playback_ctr = dynamic_cast<IAudioPlayback*>(_vendor_ctr.get());
        if (!_audio_playback_ctr) {
            _vendor_ctr.reset();
            WarnL << "Device does not support IAudioPlayback";
            return false;
        }
    } catch (const std::exception& e) {
        WarnL << "Create API speaker controller " << _tuple.shortUrl() << " failed: " << e.what();
        return false;
    }

    // ONVIF goes through the speaker.onvifUsername account provisioned below. The controller is
    // created by setupController() right after this, on the owner poller: if the account does not
    // exist yet, the periodic reconnect in onManager() picks it up once provisioning has finished.
    GET_CONFIG(std::string, onvif_username, mediakit::Speaker::kOnvifUsername);
    _ctrl_username = onvif_username;

    // Provision the ONVIF account with the vendor API. The callbacks only use the device, never the
    // controller's members, which belong to _poller and may be replaced meanwhile. The device is
    // held weakly (its pending requests hold these callbacks); _vendor_ctr keeps it alive.
    std::weak_ptr<IDevice> weak_device = _vendor_ctr;
    auto username = _ctrl_username;
    _vendor_ctr->connect([weak_device, username, password, shortUrl](bool ok, const std::string& data) {
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
    return true;
}

// ================== Vendor-specific operations ==================
// Every operation captures the device on the owner poller and its callbacks use only that
// capture: _vendor_ctr may be replaced or released by setupController()/stopController() while a
// request is in flight, and the callbacks run on the device's HTTP poller.

IDevice::Ptr SpeakerController::vendorDevice(const OnDeviceResult &cb) {
    CHECK(_poller->isCurrentThread(), "Speaker vendor operations must be called on the owner poller");
    if (!_vendor_ctr || !_audio_playback_ctr || !_ready.load()) {
        cb(false, "API controller is not ready");
        return nullptr;
    }
    return _vendor_ctr;
}

void SpeakerController::runVendorOp(const VendorOp &op, const OnDeviceResult &cb, bool autoDisconnect) {
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    device->connect([device, op, cb, autoDisconnect](bool ok, const std::string& data) {
        if (!ok) {
            cb(false, "Connection failed: " + data);
            return;
        }
        OnDeviceResult done = cb;
        if (autoDisconnect) {
            done = [device, cb](bool ok, const std::string& data) { doDisconnect(device, ok, data, cb); };
        }
        op(device, dynamic_cast<IAudioPlayback*>(device.get()), done);
    });
}

void SpeakerController::playAudioFileByVendorApi(const std::string& fileId, const std::string& fileRemoteId, OnDeviceResult cb, bool autoDisconnect) {
    runVendorOp([fileId, fileRemoteId](const IDevice::Ptr &device, IAudioPlayback *playback, const OnDeviceResult &done) {
        if (!fileRemoteId.empty()) {
            InfoL << "Known remoteId=" << fileRemoteId << ", playing immediately";
            playback->playAudio(fileRemoteId, done);
            return;
        }

        auto file = AudioFileManager::Instance().getAudioFile(fileId);
        if (file.id.empty() || !file.downloaded) {
            done(false, "Audio file is not ready for playback or does not exist: " + fileId);
            return;
        }
        TraceL << "Speaker " << device->getConfig().id << " does not contain file " << file.name << " - uploading before play";
        doUploadAudio(device, file, [device, done](bool ok, const std::string& remoteId) {
            if (!ok) {
                done(false, "Upload failed: " + remoteId);
                return;
            }
            dynamic_cast<IAudioPlayback*>(device.get())->playAudio(remoteId, done);
        });
    }, cb, autoDisconnect);
}

void SpeakerController::uploadAudio(const AudioFile& file, OnDeviceResult cb, bool autoDisconnect) {
    if (file.id.empty()) {
        cb(false, "File id cannot be empty");
        return;
    }
    runVendorOp([file](const IDevice::Ptr &device, IAudioPlayback *, const OnDeviceResult &done) {
        doUploadAudio(device, file, done);
    }, cb, autoDisconnect);
}

void SpeakerController::stopAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect) {
    runVendorOp([remoteId](const IDevice::Ptr &, IAudioPlayback *playback, const OnDeviceResult &done) {
        playback->stopAudio(remoteId, done);
    }, cb, autoDisconnect);
}

void SpeakerController::deleteAudio(const std::string& remoteId, OnDeviceResult cb, bool autoDisconnect) {
    runVendorOp([remoteId](const IDevice::Ptr &, IAudioPlayback *playback, const OnDeviceResult &done) {
        playback->deleteAudio(remoteId, done);
    }, cb, autoDisconnect);
}

void SpeakerController::listAudio(OnDeviceResult cb, bool autoDisconnect) {
    runVendorOp([](const IDevice::Ptr &, IAudioPlayback *playback, const OnDeviceResult &done) {
        playback->listAudio(done);
    }, cb, autoDisconnect);
}

void SpeakerController::setAudioVolume(int volume, OnDeviceResult cb, bool autoDisconnect) {
    runVendorOp([volume](const IDevice::Ptr &, IAudioPlayback *playback, const OnDeviceResult &done) {
        playback->setAudioVolume(volume, done);
    }, cb, autoDisconnect);
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

void SpeakerController::registerAudioBackchannel(const DeviceControl::Ptr &device_ctr, const ConnectContext &ctx) {
    AudioBackchannelInfo info;
    if (!device_ctr->setupAudioBackchannel(info, ctx.sharedDsp) || info.url.empty()) {
        WarnL << "Setup audio backchannel failed for device " << _tuple.shortUrl();
        return;
    }

    if (info.transport == AudioBackchannelInfo::RTSP_ONVIF) {
        if (info.url.find("@") == string::npos && !ctx.password.empty()) {
            info.url = UriUtils::replaceCredentials(info.url, ctx.username, ctx.password);
        }
    } else {
        info.username = ctx.username;
        info.password = ctx.password;
    }
    if (_ctrl_generation.load() != ctx.generation) {
        return; // the controller was replaced meanwhile: its backchannel is not this speaker's any more
    }

    BackchannelManager::Instance().registerDevice(_tuple.device_id, info);
}

} // namespace managerkit
