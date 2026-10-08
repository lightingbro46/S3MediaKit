#include "SpeakerController.h"
#include "Common/BlockingExecutors.h"
#include "ext-plugin/DeviceFactory.h"
#include "ext-plugin/Bosch/BoschIpSpeaker.h"
#include "Backchannel/BackchannelManager.h"
#include "Common/StrUtil.h"
#include "Common/DeviceFailureCode.h"
#include "Control/OnvifDeviceControl.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

SpeakerController::SpeakerController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _poller(poller) {
    _device_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::device(), "speaker " + tuple.shortUrl());
}

void SpeakerController::probe(const std::string& ip, int port,
                                         const std::string& username, const std::string& password,
                                         std::function<void(const SockException&, IDevice::Ptr)> cb,
                                         size_t brandIndex) {
    const auto &plugins = DeviceFactory::plugins(DeviceType::Speaker);
    if (brandIndex >= plugins.size()) {
        WarnL << "Speaker scan failed for " << ip << ":" << port;
        cb(SockException(Err_other), nullptr);
        return;
    }

    DeviceBrand brand = plugins[brandIndex].brand;
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

void SpeakerController::probeVendorApis(const std::string &ip, int port, const std::string &username,
                                        const std::string &password, const VendorStage::OnProbe &cb) {
    SpeakerConfig cfg;
    cfg.id       = _tuple.device_id;
    cfg.ip       = ip;
    cfg.port     = port;
    cfg.username = username;
    cfg.password = password;
    VendorStage::probe(DeviceType::Speaker, cfg, cb);
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

SockException SpeakerController::notReadyError() const {
    CHECK(_poller->isCurrentThread(), "notReadyError must be called on the owner poller");
    return deviceNotReadyError(_device_ctr ? _device_ctr->lastFailure() : _probe_failure);
}

SockException SpeakerController::vendorNotReadyError() const {
    CHECK(_poller->isCurrentThread(), "vendorNotReadyError must be called on the owner poller");
    return deviceNotReadyError(_vendor_down);
}

void SpeakerController::restoreCapabilities(const DeviceCapabilities &caps) {
    CHECK(_poller->isCurrentThread(), "restoreCapabilities must be called on the owner poller");
    _protocol_caps = caps;
    _vendor_seen = caps.supportsAudioFilePlayback;
}

DeviceControl::Ptr SpeakerController::createDeviceControl(ControlProtocol protocol, const std::string &address,
                                                                const std::string &username, const std::string &password) {
    return makeDeviceControl(protocol, address, username, password);
}

IDevice::Ptr SpeakerController::createVendorDevice(const SpeakerConfig &cfg) {
    return DeviceFactory::create(cfg);
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
    _vendor_stage.configure(DeviceType::Speaker, "");
    resetVendorChannel();
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
    _enableTimeSync  = option.enableTimeSync;
    _last_clock_sync = 0;
    _lock.setSetting(option.controlProtocol);
    // Without a manufacturer the speaker starts as a generic one and every cycle looks for its brand
    // first; a manufacturer without a plugin (e.g. "Hikvision") only probes the control protocols.
    _vendor_stage.configure(DeviceType::Speaker, option.manufacturer);

    const bool vendor_ok = setupDeviceController();
    if (!_audio_playback_ctr) {
        _vendor_seen = false;
    }
    if (!vendor_ok) {
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
        << (_vendor_stage.detecting() ? ", manufacturer: detect" : "")
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
    resetVendorChannel();
    _configured = false;
    _probing = false;
    ++_ctrl_generation;
    _ready = false;
    BackchannelManager::Instance().unregisterDevice(_tuple.device_id);
    DebugL << "Closed speaker controller for device: " << _tuple.shortUrl() << " (" << _address << ")";
}

std::function<DeviceClockReading()> SpeakerController::clockReader() const {
    auto device_ctr = _device_ctr;
    if (!device_ctr) {
        return nullptr;
    }
    // Only reads atomics, so the caller may run it on any thread.
    return [device_ctr]() { return device_ctr->clockReading(); };
}

bool SpeakerController::timeSyncEnabled() const {
    // The clock is only ever set by the maintenance cycle, never by an API command, as for cameras.
    GET_CONFIG(bool, clock_sync_enable, OnvifCtrl::kClockSyncEnable);
    return clock_sync_enable && _enableTimeSync;
}

SpeakerController::ConnectContext SpeakerController::connectContext() const {
    ConnectContext ctx;
    ctx.vendor = _vendor_ctr;
    ctx.username = _ctrl_username;
    ctx.password = _ctrl_option.password;
    ctx.sharedDsp = _vendor_stage.hasPlugin() && _vendor_stage.plugin().sharedDsp;
    ctx.generation = _ctrl_generation.load();
    return ctx;
}

void SpeakerController::onManager() {
    // Always called from the Timer which runs on _poller - no dispatch needed.
    if (_configured) {
        // Its own pacing, whatever the control protocol does.
        checkVendorChannel(time(nullptr));
    }
    if (!_configured || (!_device_ctr && !_lock.isAuto())) {
        onControllerReady(false, "Not supported", std::make_shared<DeviceCapabilities>());
        return;
    }

    GET_CONFIG(int, auth_retry_sec, mediakit::Speaker::kControlAuthRetrySec);
    const uint64_t now = time(nullptr);
    if (_probing || !_pacer.due(now, cycleIntervalSec(), auth_retry_sec)) {
        // avoid reconnecting too frequently, and logging in with a rejected password even less often
        return;
    }
    _pacer.onAttempt(now);
    if (_vendor_stage.detecting()) {
        startDetect();
        return;
    }
    runControlCycle();
}

int SpeakerController::cycleIntervalSec() const {
    GET_CONFIG(int, max_interval_sec, mediakit::Speaker::kDetectMaxIntervalSec);
    return _vendor_stage.cycleIntervalSec(max_interval_sec);
}

void SpeakerController::runControlCycle() {
    const uint64_t now = time(nullptr);
    if (!_device_ctr) {
        startProtocolProbe();
        return;
    }
    auto device_ctr = _device_ctr;
    // Everything the device queue needs is copied here, on the owner poller.
    auto ctx = connectContext();
    // A speaker that is up only needs one request to confirm it, not the whole connect() handshake.
    const bool was_ready = _ready.load() && device_ctr->isConnected();
    GET_CONFIG(int, clock_sync_sec, OnvifCtrl::kClockSyncIntervalSec);
    const bool time_sync = timeSyncEnabled();
    // A command that found the clock off asks for it to be set now rather than at the next interval.
    // Taken even when syncing is off, so a stale request does not fire once it is turned back on.
    const bool sync_wanted = device_ctr->takeClockSyncWanted();
    const bool sync_clock = time_sync && (!was_ready || sync_wanted || clock_sync_sec <= 0 ||
                            now - _last_clock_sync >= static_cast<uint64_t>(clock_sync_sec));
    if (sync_clock) {
        _last_clock_sync = now;
    }
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();

    // connect() is a blocking SOAP/HTTP call: run it on the speaker's serial device queue.
    _device_queue->post([weak_self, device_ctr, ctx, was_ready, sync_clock, time_sync]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        ReconnectResult result;
        result.finished = true;
        result.protocol = controlProtocolToString(device_ctr->protocol());
        if (was_ready && device_ctr->probeConnection()) {
            if (sync_clock) {
                device_ctr->setCameraTimeManual();
            }
            result.connected = true;
            result.msg = "connected";
            strong_self->postReconnectResult(result);
            return;
        }
        auto tuple = strong_self->_tuple;
        auto address = strong_self->_address;
        // reconnect to device
        if (device_ctr->connect()) {
            strong_self->publishConnected(device_ctr, ctx, sync_clock);
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
            // Rejected with the device clock far off: it will never accept us until its clock is
            // set, so set it now; the next cycle connects again.
            if (time_sync && device_ctr->lastFailure() == DeviceFailure::ClockSkew) {
                device_ctr->setCameraTimeManual();
            }
            strong_self->onControllerReady(false, err_msg, nullptr);
            result.error = err;
            result.msg = err_msg;
            result.failure = device_ctr->lastFailure();
            result.clock = device_ctr->clockReading();
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
    _vendor_pacer.reset();
    _vendor_stage.resetBackoff();
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

void SpeakerController::publishConnected(const DeviceControl::Ptr &device_ctr, const ConnectContext &ctx, bool sync_clock) {
    InfoL << controlProtocolToString(device_ctr->protocol()) << " controller of speaker " << _tuple.shortUrl()
          << " (" << _address << ") connected";
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    // Read by connect(): a speaker without a manufacturer is identified by it.
    const std::string manufacturer = device_ctr->getDeviceInfo().manufacturer;
    _poller->async([weak_self, device_ctr, manufacturer]() {
        if (auto self = weak_self.lock()) {
            self->onConnectResult(device_ctr, ConnectError::NONE, manufacturer);
        }
    }, false);
    if (sync_clock) {
        device_ctr->setCameraTimeManual();
    }

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
    // A speaker that just connected always gets its clock checked, as in onManager().
    const bool time_sync = timeSyncEnabled();
    if (time_sync) {
        _last_clock_sync = time(nullptr);
    }
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_queue->post([weak_self, order, address, ctx, time_sync]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        auto result = probeControlProtocols(order, [&](ControlProtocol protocol) {
            return self->createDeviceControl(protocol, address, ctx.username, ctx.password);
        }, self->_tuple.shortUrl() + " (" + address + ")", time_sync);
        DeviceControl::Ptr winner = result.winner;
        const std::string err_msg = result.err_msg;
        const ConnectError err = result.err;
        const DeviceFailure failure = result.failure;
        const DeviceClockReading clock = result.clock;
        if (winner && self->_ctrl_generation.load() != ctx.generation) {
            winner.reset(); // the option changed meanwhile
        }
        const uint64_t generation = ctx.generation;
        std::weak_ptr<SpeakerController> weak = self;
        self->_poller->async([weak, winner, generation, err_msg, err, failure, clock]() {
            if (auto strong = weak.lock()) {
                strong->onProbeDone(winner, generation, err_msg, err, failure, clock);
            }
        }, false);
        if (winner) {
            self->publishConnected(winner, ctx, time_sync);
        } else {
            self->onControllerReady(false, err_msg, nullptr);
        }
    });
}

void SpeakerController::onProbeDone(const DeviceControl::Ptr &device_ctr, uint64_t generation, const std::string &err_msg,
                                    ConnectError err, DeviceFailure failure,
                                    const DeviceClockReading &clock) {
    if (generation != _ctrl_generation.load()) {
        return;
    }
    _probe_failure = failure;
    _probing = false;
    ReconnectResult result;
    result.finished = true;
    if (!device_ctr) {
        WarnL << "No control protocol for speaker " << _tuple.shortUrl() << " (" << _address << "): " << err_msg;
        onAuthResult(err);
        if (_vendor_stage.detecting()) {
            onDetectCycleFailed(err == ConnectError::UNREACHABLE);
        }
        result.error = err;
        result.msg = err_msg;
        result.failure = failure;
        result.clock = clock;
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

void SpeakerController::onConnectResult(const DeviceControl::Ptr &device_ctr, ConnectError err, const std::string &manufacturer) {
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
        if (_vendor_stage.detecting()) {
            onProtocolIdentified(device_ctr, manufacturer);
        }
        return;
    }
    if (_vendor_stage.detecting()) {
        onDetectCycleFailed(err == ConnectError::UNREACHABLE);
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

void SpeakerController::startDetect() {
    _probing = true;
    _vendor_stage.startCycle();
    const uint64_t generation = _ctrl_generation.load();
    auto poller = _poller;
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    // The account of the config: a vendor plugin logs in with it, the ONVIF one does not exist yet.
    probeVendorApis(_ctrl_option.ip, _ctrl_option.port, _ctrl_option.username, _ctrl_option.password,
        [weak_self, poller, generation](const VendorStage::ProbeResult &result) {
            poller->async([weak_self, generation, result]() {
                if (auto self = weak_self.lock()) {
                    self->onVendorProbeDone(generation, result);
                }
            }, false);
        });
}

void SpeakerController::onVendorProbeDone(uint64_t generation, const VendorStage::ProbeResult &result) {
    if (generation != _ctrl_generation.load() || !_vendor_stage.detecting()) {
        return;
    }
    _probing = false;
    if (_vendor_stage.onVendorProbe(result)) {
        InfoL << "Speaker " << _tuple.shortUrl() << " (" << _address << ") detected through its vendor API: "
              << _vendor_stage.name();
        // Connects on the next timer tick (the pacer was reset), as a speaker configured with this
        // brand does: by then the plugin has provisioned the ONVIF account it logs in with. The vendor
        // channel checks on that tick too, and reports the brand even if the protocol does not connect.
        onBrandDetected();
        return;
    }
    if (result.rejected) {
        // The control protocols may still name the manufacturer; otherwise the next cycle tries again.
        WarnL << "Speaker " << _tuple.shortUrl() << " (" << _address
              << "): a vendor API rejected the account, brand not detected yet";
    }
    runControlCycle();
}

void SpeakerController::onProtocolIdentified(const DeviceControl::Ptr &device_ctr, const std::string &manufacturer) {
    if (!_vendor_stage.identify(manufacturer)) {
        // Controlled fine without a name: nothing more to learn by asking again.
        InfoL << "Speaker " << _tuple.shortUrl() << " (" << _address << ") connected over "
              << controlProtocolToString(device_ctr->protocol()) << " without a manufacturer, kept generic";
        return;
    }
    InfoL << "Speaker " << _tuple.shortUrl() << " (" << _address << ") reports manufacturer \"" << manufacturer
          << "\" over " << controlProtocolToString(device_ctr->protocol());
    if (!_vendor_stage.hasPlugin()) {
        onBrandDetected();
        return;
    }
    // A brand with a plugin whose API did not answer this cycle: switch to the plugin. Through the
    // device queue: the connect that got here publishes its "connected" state from there, which must
    // reach _poller before the switch, not after it.
    _probing = true;
    const uint64_t generation = _ctrl_generation.load();
    auto poller = _poller;
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    _device_queue->post([weak_self, poller, generation]() {
        poller->async([weak_self, generation]() {
            auto self = weak_self.lock();
            if (!self || generation != self->_ctrl_generation.load()) {
                return;
            }
            self->_probing = false;
            self->onBrandDetected();
        }, false);
    });
}

void SpeakerController::onBrandDetected() {
    _pacer.reset();
    _ctrl_option.manufacturer = _vendor_stage.name();
    if (_vendor_stage.hasPlugin()) {
        // The controller (if any) logs in with the configured account and any protocol: set the
        // plugin up and recreate it the way setupController() does for this brand.
        if (_device_ctr) {
            _ready = false;
            onControllerReady(false, "Manufacturer detected", nullptr);
            BackchannelManager::Instance().unregisterDevice(_tuple.device_id);
            _device_ctr.reset();
        }
        _lock.unlock();
        ++_ctrl_generation;
        resetVendorChannel();
        if (!setupDeviceController()) {
            _configured = false;
        } else {
            ControlProtocol protocol = _lock.fixedOrLocked();
            if (protocol != ControlProtocol::NONE) {
                _device_ctr = createDeviceControl(protocol, _address, _ctrl_username, _ctrl_option.password);
            }
        }
    }
}

void SpeakerController::onDetectCycleFailed(bool unreachable) {
    _vendor_stage.onCycleFailed(unreachable);
    if (cycleIntervalSec() > 60) {
        DebugL << "Speaker " << _tuple.shortUrl() << " (" << _address << ") without manufacturer did not answer, next detection in "
               << cycleIntervalSec() << " s";
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
    if (caps) {
        _protocol_caps = *caps;
    }
    auto data = toolkit::Any(std::make_shared<DeviceCapabilities>(currentCaps()));
    DeviceSourceEventInterceptor::onControllerReady(DeviceSource::NullDeviceSource(), connect, status, data);
}

DeviceCapabilities SpeakerController::currentCaps() const {
    DeviceCapabilities caps = _protocol_caps;
    caps.vendorApiConnected = false;
    caps.vendorApiStatus.clear();
    caps.supportsAudioFilePlayback = false;
    if (!_audio_playback_ctr) {
        return caps;
    }
    caps.vendorApiConnected = _vendor_ready.load();
    caps.vendorApiStatus = _vendor_status;
    caps.supportsAudioFilePlayback = _vendor_seen;
    if (_vendor_known) {
        // The volume only goes through the vendor API.
        caps.audioVolumeInfo = _vendor_audio;
        caps.supportsAudioVolumeControl = caps.audioVolumeInfo.supportsControl();
    }
    auto &info = caps.onvifProfile.deviceInfo;
    if (info.manufacturer.empty()) {
        // No control protocol told who made it: what the vendor API or the detection did.
        if (_vendor_known && !_vendor_info.manufacturer.empty()) {
            info.manufacturer    = _vendor_info.manufacturer;
            info.model           = _vendor_info.model;
            info.firmwareVersion = _vendor_info.firmwareVersion;
            info.serialNumber    = _vendor_info.serialNumber;
            info.hardwareId      = _vendor_info.hardwareId;
            info.macAddress      = _vendor_info.macAddress;
        } else {
            info.manufacturer = _vendor_stage.name();
        }
    }
    return caps;
}

void SpeakerController::resetVendorChannel() {
    ++_vendor_generation;
    _vendor_checking = false;
    _vendor_ready = false;
    _vendor_status.clear();
    _vendor_down = DeviceFailure::None;
    _vendor_known = false;
    _vendor_pacer.reset();
}

SpeakerConfig SpeakerController::vendorConfig() const {
    SpeakerConfig cfg;
    cfg.id       = _tuple.device_id;
    cfg.name     = _device_name;
    cfg.ip       = _ctrl_option.ip;
    cfg.type     = DeviceType::Speaker;
    cfg.brand    = _vendor_stage.plugin().brand;
    cfg.port     = _ctrl_option.port;
    cfg.username = _ctrl_option.username;
    cfg.password = _ctrl_option.password;
    return cfg;
}

void SpeakerController::checkVendorChannel(uint64_t now) {
    if (!_audio_playback_ctr || _vendor_checking || _vendor_stage.detecting()) {
        return;
    }
    GET_CONFIG(int, auth_retry_sec, mediakit::Speaker::kControlAuthRetrySec);
    if (!_vendor_pacer.due(now, 60, auth_retry_sec)) {
        return;
    }
    _vendor_pacer.onAttempt(now);
    IDevice::Ptr device;
    try {
        device = createVendorDevice(vendorConfig());
    } catch (const std::exception &e) {
        WarnL << "Create vendor API check of speaker " << _tuple.shortUrl() << " failed: " << e.what();
    }
    if (!device) {
        return;
    }
    _vendor_checking = true;
    const uint64_t generation = _vendor_generation;
    auto poller = _poller;
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    // The callbacks run on the plugin's HTTP poller: only the device and copies, then back to _poller.
    device->connect([device, poller, weak_self, generation](bool ok, const std::string &data) {
        const DeviceFailure failure = ok ? DeviceFailure::None : device->lastFailure();
        auto done = [device, poller, weak_self, generation, ok, data, failure]() {
            poller->async([device, weak_self, generation, ok, data, failure]() {
                if (auto self = weak_self.lock()) {
                    if (generation == self->_vendor_generation) {
                        self->_vendor_checking = false;
                        self->onVendorResult(generation, ok, data, failure, device);
                    }
                }
            }, false);
        };
        if (!ok) {
            done();
            return;
        }
        device->disconnect([done](bool, const std::string &) { done(); });
    });
}

void SpeakerController::onVendorResult(uint64_t generation, bool ok, const std::string &msg, DeviceFailure failure,
                                       const IDevice::Ptr &device) {
    if (generation != _vendor_generation || !_audio_playback_ctr) {
        return;
    }
    const DeviceCapabilities before = currentCaps();
    _vendor_pacer.onResult(failure == DeviceFailure::Unauthorized ? ConnectError::AUTH_FAILED : ConnectError::NONE);
    _vendor_down = failure;
    if (ok) {
        if (!_vendor_ready.load()) {
            InfoL << "Vendor API of speaker " << _tuple.shortUrl() << " (" << _address << ") answers";
        }
        _vendor_ready = true;
        _vendor_status.clear();
        _vendor_seen = true;
        if (device) {
            _vendor_info = device->getDeviceInfo();
            if (auto playback = dynamic_cast<IAudioPlayback *>(device.get())) {
                _vendor_audio = playback->getAudioSettings();
            }
            _vendor_known = true;
        }
    } else {
        if (_vendor_ready.load()) {
            WarnL << "Vendor API of speaker " << _tuple.shortUrl() << " (" << _address << ") no longer answers: " << msg;
        }
        _vendor_ready = false;
        _vendor_status = msg.empty() ? "Vendor API did not answer" : msg;
    }
    if (currentCaps() != before) {
        // Republished with the protocol state as it is: the vendor channel only changes the capabilities.
        onControllerReady(_ready.load(), _err_msg, nullptr);
    }
}

bool SpeakerController::setupDeviceController() {
    auto password = _ctrl_option.password;
    auto shortUrl = _tuple.shortUrl();

    _ctrl_username = _ctrl_option.username;
    if (!_vendor_stage.hasPlugin()) {
        return true;
    }
    const VendorPluginInfo plugin = _vendor_stage.plugin();

    // A plugin that needs one control protocol (Bosch: ONVIF for its account and backchannel) gets it,
    // whatever the option says.
    if (!plugin.requiredProtocol.empty()) {
        const ControlProtocol required = controlProtocolFromString(plugin.requiredProtocol);
        const ControlProtocol setting = controlProtocolFromString(_ctrl_option.controlProtocol);
        if (setting != required && setting != ControlProtocol::NONE) {
            WarnL << "Speaker " << shortUrl << " needs " << plugin.requiredProtocol
                  << " for its vendor plugin, ignoring control protocol " << _ctrl_option.controlProtocol;
        }
        _lock.setSetting(plugin.requiredProtocol);
    }

    try {
        _vendor_ctr = createVendorDevice(vendorConfig());
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
    if (!plugin.ownControlAccount) {
        return true;
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
    if (!_vendor_ctr || !_audio_playback_ctr || !_vendor_ready.load()) {
        cb(false, "Vendor API of the speaker is not ready");
        return nullptr;
    }
    return _vendor_ctr;
}

void SpeakerController::runVendorOp(const VendorOp &op, const OnDeviceResult &cb, bool autoDisconnect) {
    _vendor_failure = static_cast<int>(DeviceFailure::None);
    auto device = vendorDevice(cb);
    if (!device) {
        return;
    }

    // Kept when the operation ends, before the logout of autoDisconnect overwrites lastFailure().
    std::weak_ptr<SpeakerController> weak_self = shared_from_this();
    auto record = [weak_self, device](bool ok) {
        if (auto self = weak_self.lock()) {
            self->_vendor_failure = static_cast<int>(ok ? DeviceFailure::None : device->lastFailure());
        }
    };
    // The vendor channel learns from operations too: a failed login takes it down without waiting for
    // the next check, a success refreshes the volume it reports (setAudioVolume() changed it).
    const uint64_t generation = _vendor_generation;
    auto poller = _poller;
    auto report = [weak_self, poller, generation, device](bool ok, const std::string &msg) {
        const DeviceFailure failure = ok ? DeviceFailure::None : device->lastFailure();
        poller->async([weak_self, generation, device, ok, msg, failure]() {
            if (auto self = weak_self.lock()) {
                self->onVendorResult(generation, ok, msg, failure, device);
            }
        }, false);
    };
    device->connect([device, op, cb, autoDisconnect, record, report](bool ok, const std::string& data) {
        if (!ok) {
            record(false);
            report(false, data);
            cb(false, "Connection failed: " + data);
            return;
        }
        OnDeviceResult done = [record, report, cb](bool ok, const std::string& data) {
            record(ok);
            if (ok) {
                report(true, "");
            }
            cb(ok, data);
        };
        if (autoDisconnect) {
            done = [device, cb, record, report](bool ok, const std::string& data) {
                record(ok);
                if (ok) {
                    report(true, "");
                }
                doDisconnect(device, ok, data, cb);
            };
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
