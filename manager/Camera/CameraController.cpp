#include "CameraController.h"
#include "Common/BlockingExecutors.h"
#include "server/WebApiErrCode.h"
#include "Local/MediaGapFillManager.h"
#include "Util/onceToken.h"
#include "Util/util.h"
#include "Common/config.h"
#include <algorithm>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// Posts a device task after a delay; `on_cancel` runs instead if the task no longer applies.
using DelayedDeviceTask = function<void(int delay_ms, const function<void()> &task, const function<void()> &on_cancel)>;

// A device command that failed because of the network or the credentials reports that, so the UI
// can tell the user what to fix; any other failure keeps the command's own code.
static ApiErrCode failureCode(DeviceFailure failure, ApiErrCode fallback) {
    switch (failure) {
        case DeviceFailure::Unauthorized: return ApiErrCode::CODE_DEVICE_UNAUTHORIZED;
        case DeviceFailure::AuthMethodUnsupported: return ApiErrCode::CODE_DEVICE_AUTH_METHOD_NOT_ALLOWED;
        case DeviceFailure::Unreachable: return ApiErrCode::CODE_DEVICE_UNREACHABLE;
        case DeviceFailure::Timeout: return ApiErrCode::CODE_DEVICE_TIMEOUT;
        case DeviceFailure::ClockSkew: return ApiErrCode::CODE_DEVICE_CLOCK_SKEW;
        default: return fallback;
    }
}

static ApiErrCode failureCode(const CameraDeviceControl::Ptr &ptr, ApiErrCode fallback) {
    return failureCode(ptr->lastFailure(), fallback);
}

std::function<DeviceClockReading()> CameraController::clockReader() const {
    auto device_ctr = _device_ctr;
    if (!device_ctr) {
        return nullptr;
    }
    // Only reads atomics, so the caller may run it on any thread.
    return [device_ctr]() { return device_ctr->clockReading(); };
}

SockException CameraController::controllerNotReady() const {
    const auto failure = _device_ctr ? _device_ctr->lastFailure() : _probe_failure;
    const char *reason = nullptr;
    switch (failure) {
        case DeviceFailure::Unauthorized: reason = "device rejected the username or password"; break;
        case DeviceFailure::AuthMethodUnsupported: reason = "device only offers HTTP Basic authentication"; break;
        case DeviceFailure::Unreachable: reason = "device unreachable"; break;
        case DeviceFailure::Timeout: reason = "device did not respond"; break;
        case DeviceFailure::ClockSkew: reason = "device clock is out of sync with the server"; break;
        default: break;
    }
    string msg = "Device controller is not ready";
    if (reason) {
        msg += string(": ") + reason;
    }
    return SockException(Err_other, msg, failureCode(failure, ApiErrCode::CODE_DEVICE_OFFLINE));
}

CameraController::CameraController(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller)
    : _tuple(tuple), _poller(poller),
      _device_queue(std::make_shared<SerialTaskQueue>(BlockingExecutors::device(), "camera " + tuple.shortUrl())) {}

CameraDeviceControl::Ptr CameraController::createDeviceControl(ControlProtocol protocol, const std::string &address,
                                                               const std::string &username, const std::string &password) {
    return makeDeviceControl(protocol, address, username, password);
}

// Only ONVIF provides the SD card replay that MediaGapFillManager drives.
static OnvifControl::Ptr asOnvif(const CameraDeviceControl::Ptr &ctr) {
    return std::dynamic_pointer_cast<OnvifControl>(ctr);
}

void CameraController::postDeviceTask(SerialTaskQueue::Task task) {
    _device_queue->post(std::move(task));
}

void CameraController::postPriorityDeviceTask(SerialTaskQueue::Task task) {
    _device_queue->postFront(std::move(task));
}

bool CameraController::hasPendingProfileConfig() const {
    CHECK(_poller->isCurrentThread(), "hasPendingProfileConfig must be called on the owner poller");
    auto pending = [](const VideoEncoderConfig::VideoEncoderConfigMap &configs) {
        for (auto &pr : configs) {
            const auto &status = pr.second.state.status;
            if (status == configStateToString[VideoConfigSetState::NEW] ||
                status == configStateToString[VideoConfigSetState::PROCESSING]) {
                return true;
            }
        }
        return false;
    };
    // A rule job is time-boxed: it must be confirmed or retried within its apply duration, so it
    // cannot wait for the slow `profileResyncIntervalSec` re-read either.
    return pending(_profileConfigMap) || pending(_ruleConfigMap);
}

void CameraController::postDeviceTaskAfter(int delay_ms, uint64_t ptz_generation,
                                           const SerialTaskQueue::Task &task, const SerialTaskQueue::Task &on_cancel) {
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    // Only the timer runs on _poller; the task itself goes through the serial queue.
    _poller->doDelayTask(delay_ms, [weak_self, ptz_generation, task, on_cancel]() {
        auto self = weak_self.lock();
        if (!self || (ptz_generation != 0 && self->_ptz_generation != ptz_generation)) {
            on_cancel();
            return 0;
        }
        // The follow-up of an interactive command: same priority as the command itself.
        self->postPriorityDeviceTask(task);
        return 0;
    });
}

void CameraController::createTimer() {
    weak_ptr<CameraController> weak_self = shared_from_this();
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

void CameraController::setListener(const std::shared_ptr<DeviceSourceEvent> &listener) {
    setDelegate(listener);
}

bool CameraController::isReady() const {
    return _ready.load(); 
}

void CameraController::setupController(const CameraOption &option) {
    // Caller: GenericRtspCameraImp::setupController() via setCameraOption() which asserts isCurrentThread.
    CHECK(_poller->isCurrentThread(), "setupController must be called on the owner poller");

    if (_configured) {
        if (ControllerOption::from(option) == _ctrl_option) {
            const auto sync_config = SdCardSyncConfig::from(option);
            const bool sync_config_changed =
                sync_config.sdCardSyncEnabled != _sd_sync_config.sdCardSyncEnabled ||
                sync_config.sdCardSyncAutoSyncEnabled != _sd_sync_config.sdCardSyncAutoSyncEnabled ||
                sync_config.sdCardSyncMinSegmentGapSec != _sd_sync_config.sdCardSyncMinSegmentGapSec ||
                sync_config.sdCardSyncRetryCount != _sd_sync_config.sdCardSyncRetryCount;
            // Connection unchanged — update PTZ settings in-place, no reconnect needed
            _enablePTZControl = option.enablePTZControl;
            _enableTimeSync   = option.enableTimeSync;
            _reversePanAxis   = option.reversePanAxis;
            _reverseTiltAxis  = option.reverseTiltAxis;
            _ptzMode          = option.ptzMode;
            _ptzSpeed         = option.ptzSpeed;
            _keepConfigProfileAndStream = option.keepConfigProfileAndStream;
            _sd_sync_config = sync_config;
            if (sync_config_changed) {
                MediaGapFillManager::Instance().onManager(
                    _tuple.device_id, _ctrl_option.username, _ctrl_option.password,
                    asOnvif(_device_ctr), _sd_sync_config, _ready.load());
            }
            DebugL << "Controller of device: " << _tuple.shortUrl() << " connection unchanged, updated PTZ settings in-place";
            return;
        }
        DebugL << "Controller of device: " << _tuple.shortUrl() << " connection changed, recreating controller";
        MediaGapFillManager::Instance().onManager(
            _tuple.device_id, std::string(), std::string(), nullptr,
            _sd_sync_config, false);
        _ready = false;
        if (controlProtocolSetting(option.controlProtocol) != controlProtocolSetting(_ctrl_option.controlProtocol)) {
            // Nothing may keep using the old protocol's controller once the switch is published.
            onControllerReady(false, "Control protocol changed", nullptr);
        }
        _device_ctr.reset();
        _configured = false;
        // A changed address or credentials may mean another protocol now: AUTO probes again.
        _lock.unlock();
        _probing = false;
        _pacer.reset();
        ++_ctrl_generation;
        ReconnectResult pending;
        pending.msg = "Device settings changed, connecting with the new settings";
        finishReconnect(pending);
    }

    if (option.manufacturer == GENERIC_RTSP_CAMERA && option.ip.empty()) {
        WarnL << "Generic RTSP camera controller is not implemented yet";
        return;
    }

    if (option.ip.empty() || option.port == 0) {
        WarnL << "Invalid camera address, ip or port is empty";
        return;
    }

    string address = option.ip;
    if (option.autoWebPort) {
        address += ":" + (option.port > 0 ? to_string(option.port) : "80");
    } else {
        address += ":" + (option.webPort > 0 ? to_string(option.webPort) : "80");
    }

    // save controller option and PTZ settings for later use
    _address      = address;
    _ctrl_option  = ControllerOption::from(option);
    _lock.setSetting(option.controlProtocol);
    _enablePTZControl = option.enablePTZControl;
    _enableTimeSync   = option.enableTimeSync;
    _reversePanAxis   = option.reversePanAxis;
    _reverseTiltAxis  = option.reverseTiltAxis;
    _ptzMode          = option.ptzMode;
    _ptzSpeed         = option.ptzSpeed;
    _sd_sync_config   = SdCardSyncConfig::from(option);
    _keepConfigProfileAndStream = option.keepConfigProfileAndStream;
    _configured = true;

    // One controller, one protocol: the fixed one, or the one AUTO locked to. An AUTO camera without
    // a lock gets its controller from the probe in onManager().
    // todo: create plugin from manufactor and model
    ControlProtocol protocol = _lock.fixedOrLocked();
    if (protocol != ControlProtocol::NONE) {
        _device_ctr = createDeviceControl(protocol, address, option.username, option.password);
    }
    DebugL << "Controller of device: " << _tuple.shortUrl() << " (" << address << ")"
        << ", protocol setting: " << controlProtocolSetting(option.controlProtocol)
        << ", using: " << (protocol == ControlProtocol::NONE ? "probe" : controlProtocolToString(protocol))
        << ", username: " << (option.username.empty() ? "empty" : "******")
        << ", password: " << (option.password.empty() ? "empty" : "******");
}

void CameraController::setLastControlProtocol(const std::string &protocol) {
    CHECK(_poller->isCurrentThread(), "setLastControlProtocol must be called on the owner poller");
    _lock.restore(controlProtocolFromString(protocol));
}

void CameraController::stopController() {  
    CHECK(_poller->isCurrentThread(), "stopController must be called on the owner poller");

    onControllerReady(false, "Stop", nullptr);
    MediaGapFillManager::Instance().onManager(
        _tuple.device_id, std::string(), std::string(), nullptr,
        _sd_sync_config, false);
    
    _timer_ctr.reset();
    _device_ctr.reset();
    _configured = false;
    _probing = false;
    ++_ctrl_generation;
    _ready = false;
    DebugL << "Closed camera controller for device: " << _tuple.shortUrl() << " (" << _address << ")";
}

void CameraController::onManager() {
    // Always called from the Timer which runs on _poller - no dispatch needed.
    if (!_configured || (!_device_ctr && !_lock.isAuto())) {
        onControllerReady(false, "Not supported", std::make_shared<DeviceCapabilities>());
        return;
    }

    if (_device_ctr && _device_ctr->isControlled()) {
        // A PTZ or other blocking operation is in progress — skip
        WarnL << "Device controller of " << _tuple.shortUrl() << " (" << _address << ") is busy, skip reconnecting";
        return;
    }

    GET_CONFIG(int, auth_retry_sec, CameraControl::kAuthRetrySec);
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
    std::weak_ptr<CameraController> weak_self = shared_from_this();

    // A camera that is already up does not need the whole connect() handshake again.
    // Re-running it every 60s per camera cost ~16 ONVIF operations per camera per
    // minute; at 300 cameras that alone kept the `device` pool busy and left
    // interactive commands (PTZ) queued behind it. The cycle now costs one
    // GetDeviceInformation, plus a profile re-read only when there is a reason for it.
    const bool was_ready = _ready.load() && device_ctr->isConnected();
    GET_CONFIG(int, profile_resync_sec, OnvifCtrl::kProfileResyncIntervalSec);
    GET_CONFIG(int, clock_sync_sec, OnvifCtrl::kClockSyncIntervalSec);
    // A config this server is still trying to push is retried on every cycle, exactly
    // as before. Only the re-read that merely detects a change made on the camera's
    // own web page is slowed down to `profileResyncIntervalSec`.
    const bool resync_profiles = !was_ready || hasPendingProfileConfig() || profile_resync_sec <= 0 ||
                                 now - _last_profile_resync >= static_cast<uint64_t>(profile_resync_sec);
    // The camera clock is only ever set from here, never from an API command. Off for the whole
    // server (onvif.clockSyncEnable) or for this camera (CameraOption::enableTimeSync).
    GET_CONFIG(bool, clock_sync_enable, OnvifCtrl::kClockSyncEnable);
    const bool time_sync = clock_sync_enable && _enableTimeSync;
    // A command that found the clock off asks for it to be set now rather than at the next interval.
    // Taken even when syncing is off, so a stale request does not fire once it is turned back on.
    const bool sync_wanted = device_ctr->takeClockSyncWanted();
    // The clock check is the other thing the old cycle did every 60 s; a camera clock drifts far
    // too slowly to need that, but it still has to happen while the camera stays connected.
    const bool sync_clock = time_sync && (!was_ready || sync_wanted || clock_sync_sec <= 0 ||
                            now - _last_clock_sync >= static_cast<uint64_t>(clock_sync_sec));
    if (resync_profiles) {
        _last_profile_resync = now;
    }
    if (sync_clock) {
        _last_clock_sync = now;
    }

    // connect() is a blocking SOAP/HTTP call: run it on the camera's serial device queue.
    postDeviceTask([weak_self, device_ctr, was_ready, resync_profiles, sync_clock, time_sync]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        auto tuple = strong_self->_tuple;
        auto address = strong_self->_address;

        if (was_ready && device_ctr->probeConnection()) {
            // Still the same camera, still answering, credentials still accepted.
            // Nothing to publish: onControllerReady() is level-based and already
            // holds `connected`, and the gap-fill provider stays registered (new gaps
            // are discovered by MediaGapFillManager::onDeviceStateChanged, not here).
            if (sync_clock) {
                device_ctr->setCameraTimeManual();
            }
            if (resync_profiles && device_ctr->refreshMediaProfiles()) {
                strong_self->syncMediaProfile(device_ctr);
            }
            ReconnectResult result;
            result.finished = result.connected = true;
            result.protocol = controlProtocolToString(device_ctr->protocol());
            result.msg = "connected";
            strong_self->postReconnectResult(result);
            return;
        }

        // reconnect to device
        ReconnectResult result;
        result.finished = true;
        result.protocol = controlProtocolToString(device_ctr->protocol());
        if (device_ctr->connect()) {
            strong_self->publishConnected(device_ctr, sync_clock);
            result.connected = true;
            result.msg = "connected";
        } else {
            auto err_msg = device_ctr->lastError();
            auto err = device_ctr->lastConnectError();
            WarnL << controlProtocolToString(device_ctr->protocol()) << " controller of device " << tuple.shortUrl()
                  << " (" << address << ") connect failed (" << connectErrorToString(err) << "): " << err_msg;
            std::weak_ptr<CameraController> weak = strong_self;
            strong_self->_poller->async([weak, device_ctr, err]() {
                if (auto self = weak.lock()) {
                    self->onConnectResult(device_ctr, err);
                }
            }, false);
            // Rejected with the camera clock far off: it will never accept us until its clock is
            // set, so set it now; the next cycle connects again.
            if (time_sync && device_ctr->lastFailure() == DeviceFailure::ClockSkew) {
                device_ctr->setCameraTimeManual();
            }
            strong_self->onControllerReady(false, err_msg, nullptr);
            result.error = err;
            result.msg = err_msg;
        }
        // Queued after onConnectResult: a waiter sees the pacer as this attempt left it.
        strong_self->postReconnectResult(result);
    });
}

void CameraController::reconnectNow(const ReconnectCallback &cb) {
    CHECK(_poller->isCurrentThread(), "reconnectNow must be called on the owner poller");
    if (!_configured || (!_device_ctr && !_lock.isAuto())) {
        ReconnectResult result;
        result.finished = true;
        result.msg = "Device control is not configured";
        cb(result);
        return;
    }
    if (_device_ctr && _device_ctr->isControlled()) {
        ReconnectResult result;
        result.finished = true;
        result.connected = _ready.load();
        result.protocol = controlProtocolToString(_device_ctr->protocol());
        result.msg = "Device controller is busy with another command, try again later";
        cb(result);
        return;
    }
    InfoL << "Reconnect of device " << _tuple.shortUrl() << " (" << _address << ") requested";
    _reconnect_waiters.push_back(cb);
    _pacer.reset();
    // A probe already running answers the waiter when it ends.
    onManager();
}

void CameraController::finishReconnect(const ReconnectResult &result) {
    std::vector<ReconnectCallback> waiters;
    waiters.swap(_reconnect_waiters);
    for (auto &cb : waiters) {
        cb(result);
    }
}

void CameraController::postReconnectResult(const ReconnectResult &result) {
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    _poller->async([weak_self, result]() {
        if (auto self = weak_self.lock()) {
            self->finishReconnect(result);
        }
    }, false);
}

void CameraController::publishConnected(const CameraDeviceControl::Ptr &device_ctr, bool sync_clock) {
    InfoL << controlProtocolToString(device_ctr->protocol()) << " controller of device " << _tuple.shortUrl()
          << " (" << _address << ") connected";
    // Queued ahead of the profile sync below: a protocol switch drops the old profile state first.
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    _poller->async([weak_self, device_ctr]() {
        if (auto self = weak_self.lock()) {
            self->onConnectResult(device_ctr, ConnectError::NONE);
        }
    }, false);
    if (sync_clock) {
        device_ctr->setCameraTimeManual();
    }
    // Reads the profiles here; decisions on _poller, queued before onControllerReady below.
    syncMediaProfile(device_ctr);

    // get device capabilities after connected
    auto caps = std::make_shared<DeviceCapabilities>();
    caps->isOnvifDevice = device_ctr->protocol() == ControlProtocol::ONVIF;
    caps->controlProtocol = controlProtocolToString(device_ctr->protocol());
    caps->onvifProfile.deviceInfo          = device_ctr->getDeviceInfo();
    caps->onvifProfile.mediaProfiles       = device_ctr->getMediaProfilesInfo();
    caps->onvifProfile.ptzProfile          = device_ctr->getPTZProfile();
    caps->onvifProfile.imageProfile        = device_ctr->getImageProfile();
    caps->onvifProfile.relayOutputProfiles = device_ctr->getRelayOutputProfiles();
    caps->onvifProfile.audioOutputProfile  = device_ctr->getAudioOutputProfile();
    caps->onvifProfile.audioInputProfile   = device_ctr->getAudioInputProfile();
    caps->supportsSdCardPlayback           = device_ctr->getSDCardInfo().isSDSupport();
    // Register/refresh the provider before publishing the connected event.
    // The event closes the device gap and immediately triggers discovery.
    MediaGapFillManager::Instance().onManager(
        _tuple.device_id, _ctrl_option.username,
        _ctrl_option.password, asOnvif(device_ctr),
        _sd_sync_config, true);
    onControllerReady(true, "connected", std::move(caps));
}

void CameraController::startProtocolProbe() {
    _probing = true;
    GET_CONFIG(string, order_str, CameraControl::kAutoOrder);
    const auto order = parseProtocolOrder(order_str);
    const uint64_t generation = _ctrl_generation.load();
    const string address = _address;
    const string username = _ctrl_option.username;
    const string password = _ctrl_option.password;
    // Same switches as the clock sync of onManager(); a camera that just connected always gets it.
    GET_CONFIG(bool, clock_sync_enable, OnvifCtrl::kClockSyncEnable);
    const bool time_sync = clock_sync_enable && _enableTimeSync;
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    postDeviceTask([weak_self, order, generation, address, username, password, time_sync]() {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        auto result = probeControlProtocols(order, [&](ControlProtocol protocol) {
            return self->createDeviceControl(protocol, address, username, password);
        }, self->_tuple.shortUrl() + " (" + address + ")", time_sync);
        CameraDeviceControl::Ptr winner = result.winner;
        const std::string err_msg = result.err_msg;
        const ConnectError err = result.err;
        const DeviceFailure failure = result.failure;
        if (winner && self->_ctrl_generation.load() != generation) {
            winner.reset(); // the option changed meanwhile
        }
        std::weak_ptr<CameraController> weak = self;
        self->_poller->async([weak, winner, generation, err_msg, err, failure]() {
            if (auto strong = weak.lock()) {
                strong->onProbeDone(winner, generation, err_msg, err, failure);
            }
        }, false);
        if (winner) {
            self->publishConnected(winner, time_sync);
        } else {
            self->onControllerReady(false, err_msg, nullptr);
        }
    });
}

void CameraController::onProbeDone(const CameraDeviceControl::Ptr &device_ctr, uint64_t generation, const std::string &err_msg,
                                   ConnectError err, DeviceFailure failure) {
    if (generation != _ctrl_generation.load()) {
        return;
    }
    _probe_failure = failure;
    _probing = false;
    ReconnectResult result;
    result.finished = true;
    if (!device_ctr) {
        WarnL << "No control protocol for device " << _tuple.shortUrl() << " (" << _address << "): " << err_msg;
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

void CameraController::onConnectResult(const CameraDeviceControl::Ptr &device_ctr, ConnectError err) {
    if (!device_ctr || device_ctr != _device_ctr) {
        return;
    }
    onAuthResult(err);
    if (err == ConnectError::NONE) {
        onProtocolConnected(device_ctr->protocol());
        return;
    }
    GET_CONFIG(int, reprobe_failures, CameraControl::kReprobeFailures);
    if (!_lock.onConnectError(err, reprobe_failures)) {
        return;
    }
    unlockProtocol(string("Device no longer answers ") + controlProtocolToString(device_ctr->protocol()));
}

void CameraController::onProtocolConnected(ControlProtocol protocol) {
    const ControlProtocol previous = _lock.last();
    if (_lock.onConnected(protocol)) {
        // Profile tokens and PTZ coordinates of one protocol mean nothing in the other.
        const string err_msg = string("Camera control protocol changed from ") + controlProtocolToString(previous) +
                               " to " + controlProtocolToString(protocol);
        WarnL << "Device " << _tuple.shortUrl() << " (" << _address << "): " << err_msg
              << ", dropping its profile configs, video config rules and user presets";
        std::vector<string> rule_tokens;
        for (const auto &it : _ruleReportMap) {
            rule_tokens.push_back(it.first);
        }
        for (const auto &token : rule_tokens) {
            reportRuleFailureIfPending(token, "DEVICE_PROTOCOL_CHANGED", err_msg);
        }
        _ruleConfigMap.clear();
        _ruleReportMap.clear();
        _profileConfigMap.clear();
        _userPresets.clear();
    }
}

void CameraController::unlockProtocol(const std::string &reason) {
    WarnL << "Device " << _tuple.shortUrl() << " (" << _address << "): " << reason << ", probing the control protocols again";
    MediaGapFillManager::Instance().onManager(
        _tuple.device_id, std::string(), std::string(), nullptr,
        _sd_sync_config, false);
    _ready = false;
    onControllerReady(false, "Control protocol changed", nullptr);
    _device_ctr.reset();
    _lock.unlock();
    _pacer.reset();
    ++_ctrl_generation;
}

void CameraController::onAuthResult(ConnectError err) {
    const bool was_rejected = _pacer.authFailed();
    _pacer.onResult(err);
    if (_pacer.authFailed() && !was_rejected) {
        GET_CONFIG(int, auth_retry_sec, CameraControl::kAuthRetrySec);
        WarnL << "Device " << _tuple.shortUrl() << " (" << _address << ") rejected the credentials: next attempt in "
              << auth_retry_sec << " s, or at once through /media/mserver/device/reconnect";
    }
}

// ── Imaging control (focus / iris) ───────────────────────────────────────────
static void onvifImageControl(const CameraDeviceControl::Ptr &ptr, IMAGE_CONTROL_DIRECT direct, int speed,
                              const function<void(const SockException &ex)> &cb, const DelayedDeviceTask &delayed) {
    switch (direct) {
        case IMAGE_CONTROL_DIRECT::FocusAuto:
            // TODO: trigger auto-focus via Imaging service
            return cb(SockException(Err_success, "Focus auto success", ApiErrCode::CODE_SUCCESS));

        case IMAGE_CONTROL_DIRECT::FocusIn:
        case IMAGE_CONTROL_DIRECT::FocusOut: {
            if (!ptr->enableFocus()) {
                return cb(SockException(Err_other, "Device does not support imaging focus", ApiErrCode::CODE_DEVICE_NO_SUPPORT_IMAGING_FOCUS));
            }
            // speed is raw 0-100; default to 50% if not specified
            float focusSpeed = (direct == IMAGE_CONTROL_DIRECT::FocusIn ? 1.0f : -1.0f)
                             * (speed > 0 ? speed / 100.0f : 0.5f);
            if (!ptr->Imaging_FocusMove(focusSpeed)) {
                return cb(SockException(Err_other, "Imaging focus move failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_IMAGING_FOCUS_CONTROL_FAILED)));
            }
            // Auto-stop after 1 second
            delayed(1000, [ptr, cb]() {
                ptr->Imaging_FocusStop();
                cb(SockException(Err_success, "Imaging focus move success", ApiErrCode::CODE_SUCCESS));
            }, [cb]() {
                cb(SockException(Err_success, "Imaging focus move success", ApiErrCode::CODE_SUCCESS));
            });
            return;
        }

        case IMAGE_CONTROL_DIRECT::IrisAuto:
            // TODO: trigger auto-iris via Imaging service
            return cb(SockException(Err_success, "Iris auto success", ApiErrCode::CODE_SUCCESS));

        case IMAGE_CONTROL_DIRECT::IrisIn:
        case IMAGE_CONTROL_DIRECT::IrisOut: {
            if (!ptr->enableIris()) {
                return cb(SockException(Err_other, "Device does not support iris control", ApiErrCode::CODE_DEVICE_NO_SUPPORT_IMAGING_IRIS));
            }
            std::string auxCmd = (direct == IMAGE_CONTROL_DIRECT::IrisIn) ? "tt:Iris|open" : "tt:Iris|close";
            if (!ptr->PTZ_AuxiliaryCommand(auxCmd)) {
                return cb(SockException(Err_other, "Iris auxiliary command failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_IMAGING_IRIS_CONTROL_FAILED)));
            }
            return cb(SockException(Err_success, "Iris command success", ApiErrCode::CODE_SUCCESS));
        }
    }
}

// ── Relay output control ──────────────────────────────────────────────────────
static void onvifRelayControl(const CameraDeviceControl::Ptr &ptr, RELAY_OUTPUT_CONTROL direct, const std::string &token,
                              const function<void(const SockException &ex)> &cb) {
    if (!ptr->enableRelayOutput()) {
        return cb(SockException(Err_other, "Device does not support relay output", ApiErrCode::CODE_DEVICE_NO_SUPPORT_RELAY_OUTPUT));
    }
    auto relayProfiles = ptr->getRelayOutputProfiles();
    if (relayProfiles.empty()) {
        return cb(SockException(Err_other, "No relay output tokens available", ApiErrCode::CODE_RELAY_OUTPUT_FAILED));
    }
    int idx = 0;
    for (size_t i = 0; i < relayProfiles.size(); ++i) {
        if (relayProfiles[i].token == token) {
            idx = i;
            break;
        }
    }
    bool active = (direct == RELAY_OUTPUT_CONTROL::RelayOn);
    if (!ptr->Relay_SetOutputState(relayProfiles[idx].token, active)) {
        return cb(SockException(Err_other, "Relay output control failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_RELAY_OUTPUT_FAILED)));
    }
    return cb(SockException(Err_success, "Relay output control success", ApiErrCode::CODE_SUCCESS));
}

// ── Standard PTZ (pan / tilt / zoom) ─────────────────────────────────────────
static void onvifPTZMove(const CameraDeviceControl::Ptr &ptr, int ptz_mode, PTZ_DIRECT direct, int speed,
                         const function<void(const SockException &ex)> &cb, const DelayedDeviceTask &delayed) {
    if (!ptr->enablePTZ()) {
        cb(SockException(Err_other, "Device does not support PTZ", ApiErrCode::CODE_DEVICE_NO_SUPPORT_PTZ));
        return;
    }

    auto profile = ptr->getPTZProfile();
    auto clamp = [](float value, float minVal, float maxVal) { return std::max(minVal, std::min(maxVal, value)); };
    float step = speed / 100.0f;

    if (direct == PTZ_DIRECT::Home) {
        // If Home command and preset is enabled, go to home preset if exist, otherwise go to home position
        if (!ptr->PTZ_GotoHomePosition(step, step, step)) {
            cb(SockException(Err_other, "Device execute ptz goto home position failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_PTZ_GOTO_HOME_FAILED)));
            return;
        }
        return cb(SockException(Err_success, "Device execute ptz go to home success", ApiErrCode::CODE_SUCCESS));
    }

    if (profile.isAbsMoveEnable && (ptz_mode == CameraOption::kPTZAbsolutedMode || ptz_mode == CameraOption::kPTZModeAuto)) {
        float pan, tilt, zoom;
        auto status = ptr->PTZ_GetStatus(pan, tilt, zoom);
        if (status != PtzMoveStatus::IDLE) {
            auto err_msg = status == PtzMoveStatus::MOVING ? "Device is running ptz control" : ("Device ptz status unknown: " + ptr->lastError());
            // UNKNOWN is a failed GetStatus: report its network/credential cause when there is one.
            auto code = status == PtzMoveStatus::MOVING ? ApiErrCode::CODE_DEVICE_IS_RUNNING_PTZ
                                                         : failureCode(ptr, ApiErrCode::CODE_DEVICE_IS_RUNNING_PTZ);
            cb(SockException(Err_other, err_msg, code));
            return;
        }
        switch (direct) {
            case PTZ_DIRECT::Up: tilt = clamp(tilt + step, profile.absMinTilt, profile.absMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(tilt - step, profile.absMinTilt, profile.absMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(pan - step, profile.absMinPan, profile.absMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(pan + step, profile.absMinPan, profile.absMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(zoom + step, profile.absMinZoom, profile.absMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(zoom - step, profile.absMinZoom, profile.absMaxZoom); break;
            case PTZ_DIRECT::Home:
                pan = (profile.absMinPan + profile.absMaxPan) / 2;
                tilt = (profile.absMinTilt + profile.absMaxTilt) / 2;
                zoom = (profile.absMinZoom + profile.absMaxZoom) / 2;
                break;
            default: break;
        }
        if (!ptr->PTZ_AbsoluteMove(pan, tilt, zoom, step, step, step)) {
            cb(SockException(Err_other, "Device execute ptz absolute move failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_PTZ_ABSOLUTED_CONTROL_FAILED)));
            return;
        }
        // Execute success
        return cb(SockException(Err_success, "Device execute ptz absolute move success", ApiErrCode::CODE_SUCCESS));
    };

    if (profile.isRelMoveEnable && (ptz_mode == CameraOption::kPTZRelativeMode || ptz_mode == CameraOption::kPTZModeAuto)) {
        float pan = 0.0, tilt = 0.0, zoom = 0.0;
        switch (direct) {
            case PTZ_DIRECT::Up: tilt = clamp(step, profile.relMinTilt, profile.relMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(-step, profile.relMinTilt, profile.relMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(-step, profile.relMinPan, profile.relMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(step, profile.relMinPan, profile.relMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(step, profile.relMinZoom, profile.relMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(-step, profile.relMinZoom, profile.relMaxZoom); break;
            case PTZ_DIRECT::Home:
                pan = (profile.relMinPan + profile.relMaxPan) / 2;
                tilt = (profile.relMinTilt + profile.relMaxTilt) / 2;
                zoom = (profile.relMinZoom + profile.relMaxZoom) / 2;
                break;
            default: break;
        }
        if (!ptr->PTZ_RelativeMove(pan, tilt, zoom , step, step, step)) {
            cb(SockException(Err_other, "Device execute ptz relative move failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_PTZ_RELATIVE_CONTROL_FAILED)));
            return;
        }
        // Execute success
        return cb(SockException(Err_success, "Device execute ptz relative move success", ApiErrCode::CODE_SUCCESS));
    } 
    
    if (profile.isConsMoveEnable && (ptz_mode == CameraOption::kPTZContinousMode || ptz_mode == CameraOption::kPTZModeAuto)) {
        float pan = 0.0, tilt = 0.0, zoom = 0.0;
        switch (direct) {
            case PTZ_DIRECT::Up: tilt = clamp(step, profile.consMinTilt, profile.consMaxTilt); break;
            case PTZ_DIRECT::Down: tilt = clamp(-step, profile.consMinTilt, profile.consMaxTilt); break;
            case PTZ_DIRECT::Left: pan = clamp(-step, profile.consMinPan, profile.consMaxPan); break;
            case PTZ_DIRECT::Right: pan = clamp(step, profile.consMinPan, profile.consMaxPan); break;
            case PTZ_DIRECT::ZoomIn: zoom = clamp(step, profile.consMinZoom, profile.consMaxZoom); break;
            case PTZ_DIRECT::ZoomOut: zoom = clamp(-step, profile.consMinZoom, profile.consMaxZoom); break;
            case PTZ_DIRECT::Home:
                pan = (profile.consMinPan + profile.consMaxPan) / 2;
                tilt = (profile.consMinTilt + profile.consMaxTilt) / 2;
                zoom = (profile.consMinZoom + profile.consMaxZoom) / 2;
                break;
            default: break;
        }

        int timeout_sec = 1;
        if (!ptr->PTZ_ContinuousMove(pan, tilt, zoom, timeout_sec)) {
            cb(SockException(Err_other, "Device execute ptz continuous move failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_PTZ_CONTINUOUS_CONTROL_FAILED)));
            return;
        }

        // Stop after the move timeout, unless a newer PTZ command was issued meanwhile: stopping
        // then would cut that command short (the camera also stops by itself at the timeout).
        delayed(timeout_sec * 1000, [ptr, cb]() {
            if (!ptr->PTZ_Stop(true, true)) {
                cb(SockException(Err_other, "Device execute stop ptz continuous move failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_PTZ_CONTINUOUS_CONTROL_FAILED)));
                return;
            }
            // Execute success
            cb(SockException(Err_success, "Device execute ptz continuous move success", ApiErrCode::CODE_SUCCESS));
        }, [cb]() {
            cb(SockException(Err_success, "Device execute ptz continuous move success", ApiErrCode::CODE_SUCCESS));
        });
        return;
    }
    /* Unreachable */
    cb(SockException(Err_other, "Device does not support selected PTZ control mode", ApiErrCode::CODE_DEVICE_NO_SUPPORT_SELECTED_PTZ_MODE));
}

void CameraController::PTZMove(const std::string &strDirect, int speed, const function<void(const SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::PTZMove) asserts isCurrentThread - no dispatch needed.
    // check permission from camera option
    if (!_enablePTZControl) {
        cb(SockException(Err_other, "Camera is configured to disable PTZ control", ApiErrCode::CODE_DEVICE_CONFIG_DISABLE_PTZ));
        return;
    }

    // convert direction string to PTZ_DIRECT
    PTZ_DIRECT direct;
    if (strDirect == "up")
        direct = !_reverseTiltAxis ? PTZ_DIRECT::Up : PTZ_DIRECT::Down;
    else if (strDirect == "down")
        direct = !_reverseTiltAxis ? PTZ_DIRECT::Down : PTZ_DIRECT::Up;
    else if (strDirect == "right")
        direct = !_reversePanAxis ? PTZ_DIRECT::Right : PTZ_DIRECT::Left;
    else if (strDirect == "left")
        direct = !_reversePanAxis ? PTZ_DIRECT::Left : PTZ_DIRECT::Right;
    else if (strDirect == "zoomIn")
        direct = PTZ_DIRECT::ZoomIn;
    else if (strDirect == "zoomOut")
        direct = PTZ_DIRECT::ZoomOut;
    else
        direct = PTZ_DIRECT::Home;

    int ptz_speed = speed < 0 ? speed : static_cast<int>(_ptzSpeed);

    if (_device_ctr && _device_ctr->isConnected()) {
        // Read state on _poller before dispatching to avoid data races.
        auto device_ctr = _device_ctr;
        int ptz_mode   = _ptzMode;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        const uint64_t generation = ++_ptz_generation;
        DelayedDeviceTask delayed = [weak_self, generation](int delay_ms, const function<void()> &task, const function<void()> &on_cancel) {
            auto self = weak_self.lock();
            if (!self) {
                on_cancel();
                return;
            }
            self->postDeviceTaskAfter(delay_ms, generation, task, on_cancel);
        };
        postPriorityDeviceTask([device_ctr, ptz_mode, direct, ptz_speed, cb, delayed]() {
            onvifPTZMove(device_ctr, ptz_mode, direct, ptz_speed, cb, delayed);
        });
        return;
    }
    // todo: add more ptz function from manufacturer sdk

    return cb(controllerNotReady());
}

void CameraController::ImageMoveControl(const std::string &strDirect, int speed, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::ImageMove) asserts isCurrentThread - no dispatch needed.
    
    // convert direction string to IMAGE_CONTROL_DIRECT
    IMAGE_CONTROL_DIRECT direct;
    if (strDirect == "focusAuto")
        direct = IMAGE_CONTROL_DIRECT::FocusAuto;
    else if (strDirect == "focusIn")
        direct = IMAGE_CONTROL_DIRECT::FocusIn;
    else if (strDirect == "focusOut")
        direct = IMAGE_CONTROL_DIRECT::FocusOut;
    else if (strDirect == "irisAuto")
        direct = IMAGE_CONTROL_DIRECT::IrisAuto;
    else if (strDirect == "irisIn")
        direct = IMAGE_CONTROL_DIRECT::IrisIn;
    else if (strDirect == "irisOut")
        direct = IMAGE_CONTROL_DIRECT::IrisOut;
    else {
        direct = IMAGE_CONTROL_DIRECT::FocusAuto;
    }
    
    if (_device_ctr && _device_ctr->isConnected()) {
        auto device_ctr = _device_ctr;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        DelayedDeviceTask delayed = [weak_self](int delay_ms, const function<void()> &task, const function<void()> &on_cancel) {
            auto self = weak_self.lock();
            if (!self) {
                on_cancel();
                return;
            }
            self->postDeviceTaskAfter(delay_ms, 0, task, on_cancel);
        };
        postPriorityDeviceTask([device_ctr, direct, speed, cb, delayed]() {
            onvifImageControl(device_ctr, direct, speed, cb, delayed);
        });
        return;
    }

    return cb(controllerNotReady());
}

void CameraController::RelayOutputControl(const std::string &strDirect, const std::string &relay_token, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::RelayOutputControl) asserts isCurrentThread - no dispatch needed.

    // convert direction string to RELAY_OUTPUT_CONTROL
    RELAY_OUTPUT_CONTROL direct;
    if (strDirect == "relayOn") {
        direct = RELAY_OUTPUT_CONTROL::RelayOn;
    } else if (strDirect == "relayOff") {
        direct = RELAY_OUTPUT_CONTROL::RelayOff;
    } else {
        direct = RELAY_OUTPUT_CONTROL::RelayOff;
    }

    if (_device_ctr && _device_ctr->isConnected()) {
        auto device_ctr = _device_ctr;
        postPriorityDeviceTask([device_ctr, relay_token, direct, cb]() {
            onvifRelayControl(device_ctr, direct, relay_token, cb);
        });
        return;
    }

    return cb(controllerNotReady());
}

void CameraController::onControllerReady(bool connect, const std::string &status, const std::shared_ptr<DeviceCapabilities> &caps) {
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

bool CameraController::addUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex, float pan, float tilt, float zoom)> &cb) {
    if (presetToken.empty() || presetName.empty()) {
        return false;
    }
    if (!(_device_ctr && _device_ctr->isConnected())) {
        cb(controllerNotReady(), 0, 0, 0);
        return false;
    }
    auto profile = _device_ctr->getPTZProfile();
    if (!profile.isPresetEnable) {
        cb(SockException(Err_other, "Device does not support PTZ preset", ApiErrCode::CODE_DEVICE_NO_SUPPORT_PTZ_PRESET), 0, 0, 0);
        return false;
    }
    auto device_ctr = _device_ctr;
    auto poller    = _poller;
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    std::string token = presetToken;
    std::string name  = presetName;
    // Blocking SOAP — run it on the camera's serial device queue so _poller stays responsive.
    postPriorityDeviceTask([weak_self, device_ctr, poller, token, name, cb]() {
        float p = 0, t = 0, z = 0;
        // PTZ_GetStatus returns UNKNOWN only on SOAP failure; IDLE/MOVING both indicate
        // the call succeeded and the coordinates are valid.
        auto status = device_ctr->PTZ_GetStatus(p, t, z);
        SockException result = (status == PtzMoveStatus::UNKNOWN)
            ? SockException(Err_other, "Execute ptz get status failed: " + device_ctr->lastError(), failureCode(device_ctr, ApiErrCode::CODE_PTZ_GET_STATUS_FAILED))
            : SockException(Err_success, "User preset added successfully", ApiErrCode::CODE_SUCCESS);
        poller->async([weak_self, token, name, p, t, z, result, cb]() mutable {
            auto self = weak_self.lock();
            if (result) { cb(result, 0, 0, 0); return; }
            if (self) { self->addUserPTZPreset(token, name, p, t, z); }
            cb(result, p, t, z);
        });
    });
    return true;
}

bool CameraController::addUserPTZPreset(const std::string &presetToken, const std::string &presetName, float &pan, float &tilt, float &zoom) {
    OnvifPTZProfile::PTZPreset preset;
    preset.Token = presetToken;
    preset.Name = presetName;
    preset.absPan = pan;
    preset.absTilt = tilt;
    preset.absZoom = zoom;
    _userPresets[presetToken] = std::move(preset);
    return true;
}

bool CameraController::removeUserPTZPreset(const std::string &presetToken, const std::string &presetName, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::removeUserPTZPreset) asserts isCurrentThread - no dispatch needed.
    if (_userPresets.find(presetToken) != _userPresets.end()) {
        _userPresets.erase(presetToken);
        cb(SockException(Err_success, "User preset removed successfully", ApiErrCode::CODE_SUCCESS));
        return true;
    }
    cb(SockException(Err_other, "User preset not found", ApiErrCode::CODE_PTZ_PRESET_NOT_FOUND));
    return false;
}

void CameraController::PTZGotoPreset(const std::string &presetToken, bool isUserPreset, const std::function<void(const toolkit::SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::PTZGotoPreset) asserts isCurrentThread - no dispatch needed.
    // check permission from camera option
    if (!_enablePTZControl) {
        cb(SockException(Err_other, "Camera is configured to disable PTZ control", ApiErrCode::CODE_DEVICE_CONFIG_DISABLE_PTZ));
        return;
    }
    // A preset move supersedes a pending continuous-move stop.
    ++_ptz_generation;

    if (isUserPreset) {
        auto it = _userPresets.find(presetToken);
        if (it == _userPresets.end()) {
            cb(SockException(Err_other, "User preset not found", ApiErrCode::CODE_PTZ_PRESET_NOT_FOUND));
            return;
        }
        auto &preset = it->second;
        if (_device_ctr && _device_ctr->isConnected()) {
            auto device_ctr = _device_ctr;
            float pan = preset.absPan, tilt = preset.absTilt, zoom = preset.absZoom;
            postPriorityDeviceTask([device_ctr, pan, tilt, zoom, cb]() mutable {
                if (!device_ctr->PTZ_AbsoluteMove(pan, tilt, zoom)) {
                    cb(SockException(Err_other, "Device execute ptz goto user preset failed: " + device_ctr->lastError(), failureCode(device_ctr, ApiErrCode::CODE_PTZ_GOTO_USER_PRESET_FAILED)));
                } else {
                    cb(SockException(Err_success, "Device execute ptz goto user preset success", ApiErrCode::CODE_SUCCESS));
                }
            });
            return;
        } else {
            cb(controllerNotReady());
            return;
        }
    } else {
        if (_device_ctr && _device_ctr->isConnected()) {
            auto device_ctr = _device_ctr;
            string token = presetToken;
            postPriorityDeviceTask([device_ctr, token, cb]() {
                if (!device_ctr->PTZ_GotoPreset(token, 0.5, 0.5, 0.5)) {
                    cb(SockException(Err_other, "Device execute ptz goto preset failed: " + device_ctr->lastError(), failureCode(device_ctr, ApiErrCode::CODE_PTZ_GOTO_PRESET_FAILED)));
                } else {
                    cb(SockException(Err_success, "Device execute ptz goto preset success", ApiErrCode::CODE_SUCCESS));
                }
            });
            return;
        } else {
            cb(controllerNotReady());
            return;
        }
    }
}

static void onvifGetMediaProfile(const CameraDeviceControl::Ptr &ptr, const string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb) {
    VideoEncoderConfig config;
    if (!ptr->getVideoEncoderConfigByToken(profileToken, config)) {
        cb(SockException(Err_other, "Device execute get media profile failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_ONVIF_GET_CONFIG_FAILED)), config);
        return;
    }
    cb(SockException(Err_success, "Device execute get media profile success", ApiErrCode::CODE_SUCCESS), config);
}

void CameraController::getMediaProfileAsync(const string &profileToken, const std::function<void(const toolkit::SockException &ex, VideoEncoderConfig &config)> &cb) {
    // Caller (GenericRtspCameraImp::getMediaProfile) asserts isCurrentThread - no dispatch needed.
    if (_device_ctr && _device_ctr->isConnected()) {
        auto device_ctr = _device_ctr;
        auto poller = _poller;
        string token = profileToken;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        auto on_callback = [weak_self, poller, cb](const toolkit::SockException &ex, VideoEncoderConfig &config) mutable {
            VideoEncoderConfig config_copy = config; // Make a copy to avoid dangling reference
            poller->async([weak_self, cb, ex, config_copy]() mutable {
                // note: dispatch to _poller to ensure thread safety when accessing CameraController members in the callback
                cb(ex, config_copy);
            });
        };
        postDeviceTask([device_ctr, token, on_callback]() {
            onvifGetMediaProfile(device_ctr, token, on_callback);
        });
        return;
    }
    // todo: add more ptz function from manufacturer sdk
    VideoEncoderConfig config;
    return cb(controllerNotReady(), config);
}

void CameraController::getSDCardInfoAsync(const std::function<void(const toolkit::SockException &ex, SDCardInformation &info)> &cb) {
    // Caller (GenericRtspCameraImp::getSDCardInfo) asserts isCurrentThread - no dispatch needed.
    if (_device_ctr && _device_ctr->isConnected()) {
        auto sdInfo = _device_ctr->getSDCardInfo();
        cb(SockException(Err_success, "Get SD card info success", ApiErrCode::CODE_SUCCESS), sdInfo);
        return;
    }
    SDCardInformation info;
    cb(controllerNotReady(), info);
    return;
}

static void onvifSetMediaProfile(const CameraDeviceControl::Ptr &ptr, const string &profileToken, const VideoEncoderConfig &config, const function<void(const SockException &ex)> &cb, const function<void()> &on_success) { 
    if (!ptr->setVideoEncoderConfigByToken(profileToken, config)) {
        cb(SockException(Err_other, "Device execute set media profile failed: " + ptr->lastError(), failureCode(ptr, ApiErrCode::CODE_ONVIF_SET_CONFIG_FAILED)));
        return;
    }
        
    on_success();
    return cb(SockException(Err_success, "Device execute set media profile success", ApiErrCode::CODE_SUCCESS));
}

void CameraController::setMediaProfileAsync(const string &profileToken, VideoEncoderConfig &config, const function<void(const SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::setMediaProfile) asserts isCurrentThread - no dispatch needed.
    if (_device_ctr && _device_ctr->isConnected()) {
        VideoEncoderConfig new_f_config;
        auto it = _profileConfigMap.find(profileToken);
        if (it != _profileConfigMap.end()) {
            VideoEncoderConfig f_config = it->second;
            
            new_f_config.vcodec = config.vcodec;
            new_f_config.fps = config.fps;
            new_f_config.bitrate = config.bitrate;
            new_f_config.width = config.width;
            new_f_config.height = config.height;
            new_f_config.state.retry_time = f_config.state.retry_time;
            new_f_config.state.status = f_config.state.status;
            if (config != f_config) {
                TraceL << "Profile config changed by setting";
                new_f_config.state.retry_time = 5;
                new_f_config.state.status = configStateToString[VideoConfigSetState::NEW];
            } else {
                TraceL << "The config does not change";
                return cb(SockException(Err_other, "New config is the same as the current config", ApiErrCode::CODE_ONVIF_SET_CONFIG_NOT_CHANGE));
            }
        } else {
            TraceL << "Profile config not exist in current map, add new config";
            new_f_config = config;
            new_f_config.state.retry_time = 5;
            new_f_config.state.status = configStateToString[VideoConfigSetState::NEW];
            addProfileConfig(profileToken, new_f_config);
        }
        if (_ruleConfigMap.count(profileToken)) {
            // A video config rule owns the token: pushing the user config now would fight the rule
            // (each sync pushes the rule back). Only the baseline changes; the rollback at the end of
            // the rule pushes it, and stream_settings keeps it across a restart meanwhile.
            InfoL << "Profile token: " << profileToken << " is held by a video config rule, config deferred until it ends";
            addProfileConfig(profileToken, new_f_config);
            return cb(SockException(Err_success, "Config saved, applied when the video config rule ends", ApiErrCode::CODE_ONVIF_SET_CONFIG_DEFERRED));
        }
        auto device_ctr = _device_ctr;
        auto poller = _poller;
        std::weak_ptr<CameraController> weak_self = shared_from_this();
        auto on_success = [profileToken, new_f_config, poller, weak_self]() mutable {
            poller->async([weak_self, profileToken, new_f_config]() mutable {
                if (auto self = weak_self.lock()) {
                    self->addProfileConfig(profileToken, new_f_config);
                }
            });
        };
        postDeviceTask([device_ctr, profileToken, config, cb, on_success]() {
            onvifSetMediaProfile(device_ctr, profileToken, config, cb, on_success);
        });
        return;
    }    
    // todo: add more ptz function from manufacturer sdk

    return cb(controllerNotReady());
}

void CameraController::addProfileConfig(const std::string &profileToken, const VideoEncoderConfig &config, bool emitEvent) {
    DebugL << "Add profile config to map, profile token: " << profileToken 
        << ", vcodec: " << config.vcodec 
        << ", fps: " << config.fps 
        << ", bitrate: " << config.bitrate 
        << ", width: " << config.width 
        << ", height: " << config.height
        << ", retry_time: " << config.state.retry_time
        << ", status: " << config.state.status;
    // Update the profile config in the map and emit event if needed. This function is called after successfully set media profile to camera or retry setting
    _profileConfigMap[profileToken] = config;

    if (emitEvent) {
        auto flag = NOTICE_EMIT(BroadcastStreamSettingChangeArgs, Broadcast::kBroadcastStreamSettingChange, _tuple, profileToken, config);
        if (!flag) {
            WarnL << "Nobody listen for stream change event";
        }
    }
}

void CameraController::syncMediaProfile(const CameraDeviceControl::Ptr &device_ctr) {
    // Runs on the device queue: only the SOAP read happens here, the decisions use _profileConfigMap
    // and are made on _poller.
    if (!device_ctr || !device_ctr->isConnected()) {
        WarnL << "Device controller is not ready when retrying set media profile";
        return;
    }
    auto profiles = device_ctr->getMediaProfilesInfo();
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    _poller->async([weak_self, device_ctr, profiles]() {
        if (auto self = weak_self.lock()) {
            self->applyMediaProfileSync(device_ctr, profiles);
        }
    }, false);
}

void CameraController::applyMediaProfileSync(const CameraDeviceControl::Ptr &device_ctr, const OnvifMediaProfileMap &profiles) {
    // Retries setting media profiles in NEW/PROCESSING state; the retry logic is based on
    // retry_time, which is reset to 5 once the camera config matches.
    CHECK(_poller->isCurrentThread(), "applyMediaProfileSync must run on the owner poller");
    std::vector<ProfileSetRequest> requests;
    for (const auto &profile : profiles) {
        auto token = profile.token;

        // A video config rule owns this token: retry the rule config, not the baseline.
        auto rule_it = _ruleConfigMap.find(token);
        if (rule_it != _ruleConfigMap.end()) {
            VideoEncoderConfig &rule_config = rule_it->second;
            if (profile.isVideoConfigEqual(rule_config)) {
                if (rule_config.state.status != configStateToString[VideoConfigSetState::SUCCESS]) {
                    rule_config.state.status = configStateToString[VideoConfigSetState::SUCCESS];
                    InfoL << "Profile token: " << token << " rule config is consistent with camera, mark as SUCCESS";
                }
                rule_config.state.retry_time = 5;
                reportRuleConfigResult(token, true, "", "");
            } else if (rule_config.state.retry_time > 0) {
                ProfileSetRequest request;
                request.token = token;
                request.is_rule = true;
                request.based_on = rule_config;
                request.on_success = rule_config;
                request.on_success.state.retry_time--;
                request.on_success.state.status = configStateToString[VideoConfigSetState::PROCESSING];
                request.on_failure = rule_config;
                request.on_failure.state.retry_time = 0;
                request.on_failure.state.status = configStateToString[VideoConfigSetState::FAILED];
                requests.push_back(request);
            } else {
                rule_config.state.status = configStateToString[VideoConfigSetState::FAILED];
                reportRuleConfigResult(token, false, "CONFIG_NOT_APPLIED",
                                       "Camera accepted the request but the config was not applied after retries");
            }
            continue;
        }

        auto it = _profileConfigMap.find(token);
        if (it == _profileConfigMap.end()) {
            TraceL << "Profile token: " << token << " not found in current profile config map, skip syncing";
            continue;
        }

        VideoEncoderConfig f_config = it->second;
        if (profile.isVideoConfigEqual(f_config)) {
            f_config.state.retry_time = 5;
            if (f_config.state.status != configStateToString[VideoConfigSetState::EXTERNAL]) {
                f_config.state.status = configStateToString[VideoConfigSetState::SUCCESS];
                InfoL << "Profile token: " << token << " config is consistent with camera, mark as SUCCESS";
            }
            addProfileConfig(token, f_config);
            continue;
        }
        // The config in camera is different from the config in map, which means the config is changed by camera or failed to set to camera.
        // We will retry setting config to camera if retry_time > 0, otherwise we will consider it as failed and update the state to FAILED.
        // If the config is changed by camera, we will update the config in map to keep it consistent with camera, and set the state to EXTERNAL
        // to indicate the config is changed by external and we will not try to set it to camera until next time when we detect the config is changed again.
        if (f_config.state.retry_time == 0 && f_config.state.status == configStateToString[VideoConfigSetState::PROCESSING]) {
            f_config.state.status = configStateToString[VideoConfigSetState::FAILED];
            InfoL << "Profile token: " << token << " config failed to set to camera after retrying, mark as FAILED";
            addProfileConfig(token, f_config);
        } else if (f_config.state.retry_time > 0 && (f_config.state.status == configStateToString[VideoConfigSetState::PROCESSING] || f_config.state.status == configStateToString[VideoConfigSetState::NEW])) {
            ProfileSetRequest request;
            request.token = token;
            request.based_on = f_config;
            request.on_success = f_config;
            request.on_success.state.retry_time--;
            request.on_success.state.status = configStateToString[VideoConfigSetState::PROCESSING];
            request.on_failure = f_config;
            request.on_failure.state.retry_time = 0;
            request.on_failure.state.status = configStateToString[VideoConfigSetState::FAILED];
            requests.push_back(request);
        } else if (!_keepConfigProfileAndStream) {
            InfoL << "Profile token: " << token << " config changed by camera";
            // If the previous config update failed after 5 retry attempts, update the user with the
            // current camera config instead of retrying the same failed config again.
            ProfileSetRequest request;
            request.token = token;
            request.based_on = f_config;
            f_config.state.retry_time = 4;
            f_config.state.status = configStateToString[VideoConfigSetState::PROCESSING];
            request.on_success = f_config;
            request.on_failure = f_config;
            requests.push_back(request);
        } else {
            InfoL << "Profile token: " << token << " config changed by camera";
            f_config.vcodec = profile.vcodec;
            f_config.fps = profile.fps;
            f_config.bitrate = profile.bitrate;
            f_config.width = profile.width;
            f_config.height = profile.height;
            f_config.state.retry_time = 5;
            f_config.state.status = configStateToString[VideoConfigSetState::EXTERNAL];
            InfoL << "Keep the config in map consistent with camera for profile token: " << token;
            addProfileConfig(token, f_config);
        }
    }
    if (requests.empty()) {
        return;
    }

    // The set commands are blocking SOAP: send them on the device queue, store results on _poller.
    auto poller = _poller;
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    postDeviceTask([weak_self, device_ctr, poller, requests]() {
        for (const auto &request : requests) {
            const bool ok = device_ctr->setVideoEncoderConfigByToken(request.token, request.on_success);
            std::string err_msg;
            if (ok) {
                InfoL << "Retry set config to camera for profile token: " << request.token
                      << "; retries left: " << request.on_success.state.retry_time;
            } else {
                err_msg = device_ctr->lastError();
                WarnL << "Failed to set config to camera for profile token: " << request.token << ": " << err_msg;
            }
            poller->async([weak_self, request, ok, err_msg]() {
                if (auto self = weak_self.lock()) {
                    self->storeProfileSetResult(request, ok, err_msg);
                }
            }, false);
        }
    });
}

void CameraController::storeProfileSetResult(const ProfileSetRequest &request, bool ok, const std::string &err_msg) {
    if (request.is_rule) {
        auto rule_it = _ruleConfigMap.find(request.token);
        if (rule_it == _ruleConfigMap.end()) {
            // The rule was rolled back or replaced while its config was being set.
            return;
        }
        const auto &current = rule_it->second;
        if (current != request.based_on || current.state.status != request.based_on.state.status ||
            current.state.retry_time != request.based_on.state.retry_time) {
            DebugL << "Rule token: " << request.token << " changed while its config was being set, keep the newer entry";
            return;
        }
        rule_it->second = ok ? request.on_success : request.on_failure;
        if (!ok) {
            reportRuleConfigResult(request.token, false, "SET_CONFIG_FAILED", err_msg);
        }
        return;
    }

    auto it = _profileConfigMap.find(request.token);
    if (it != _profileConfigMap.end()) {
        const auto &current = it->second;
        if (current != request.based_on || current.state.status != request.based_on.state.status ||
            current.state.retry_time != request.based_on.state.retry_time) {
            DebugL << "Profile token: " << request.token << " changed while its config was being set, keep the newer entry";
            return;
        }
    }
    addProfileConfig(request.token, ok ? request.on_success : request.on_failure);
}

VideoEncoderConfig CameraController::seedBaselineIfAbsent(const string &profileToken, const VideoEncoderConfig &seed) {
    auto it = _profileConfigMap.find(profileToken);
    if (it != _profileConfigMap.end()) {
        // Set by the user while the camera was being read: that is the baseline now.
        return it->second;
    }
    addProfileConfig(profileToken, seed, true);
    return seed;
}

bool CameraController::getBaselineVideoConfig(const string &profileToken, VideoEncoderConfig &config) const {
    auto it = _profileConfigMap.find(profileToken);
    if (it == _profileConfigMap.end()) {
        return false;
    }
    config = it->second;
    return true;
}

void CameraController::clearRuleVideoConfig(const string &profileToken) {
    _ruleConfigMap.erase(profileToken);
    _ruleReportMap.erase(profileToken);
}

bool CameraController::isRuleResultReported(const string &profileToken) const {
    auto it = _ruleReportMap.find(profileToken);
    return it != _ruleReportMap.end() && it->second.reported;
}

void CameraController::reportRuleConfigResult(const string &profileToken, bool success,
                                              const string &err_code, const string &err_msg) {
    auto it = _ruleReportMap.find(profileToken);
    if (it == _ruleReportMap.end() || it->second.reported) {
        return;
    }
    it->second.reported = true;
    it->second.success = success;
    for (const auto &log_id : it->second.logIds) {
        emitRuleResult(log_id, profileToken, success, err_code, err_msg);
    }
}

void CameraController::emitRuleResult(const string &log_id, const string &profileToken, bool success,
                                      const string &err_code, const string &err_msg) {
    InfoL << "Report rule config result, logId: " << log_id
          << ", token: " << profileToken
          << ", status: " << (success ? "SUCCESS" : "FAILED")
          << (success ? "" : (", code: " + err_code + ", msg: " + err_msg));
    NOTICE_EMIT(BroadcastVideoConfigRuleResultArgs, Broadcast::kBroadcastVideoConfigRuleResult,
                log_id, _tuple.device_id, success, err_code, err_msg);
}

CameraController::RuleAttachResult CameraController::attachRuleLogId(const string &profileToken, const string &logId) {
    auto it = _ruleReportMap.find(profileToken);
    if (it == _ruleReportMap.end() || _ruleConfigMap.find(profileToken) == _ruleConfigMap.end()) {
        return RuleAttachResult::NEEDS_REAPPLY;
    }
    auto &ctx = it->second;
    if (ctx.reported && !ctx.success) {
        // The camera never took this config: extending must try it again, not repeat the failure.
        return RuleAttachResult::NEEDS_REAPPLY;
    }
    ctx.logIds.push_back(logId);
    if (ctx.reported) {
        // Already confirmed on the camera: the extending request is satisfied right away.
        emitRuleResult(logId, profileToken, true, "", "");
    }
    return RuleAttachResult::ATTACHED;
}

void CameraController::markRuleResultReported(const string &profileToken, bool success) {
    auto it = _ruleReportMap.find(profileToken);
    if (it == _ruleReportMap.end()) {
        return;
    }
    it->second.reported = true;
    it->second.success = success;
}

bool CameraController::reportRuleFailureIfPending(const string &profileToken, const string &err_code, const string &err_msg) {
    auto it = _ruleReportMap.find(profileToken);
    if (it == _ruleReportMap.end() || it->second.reported) {
        return false;
    }
    reportRuleConfigResult(profileToken, false, err_code, err_msg);
    return true;
}

void CameraController::applyRuleVideoConfigAsync(const std::string &logId, const string &profileToken, const VideoEncoderConfig &config, const function<void(const SockException &ex)> &cb) {
    // Caller (GenericRtspCameraImp::applyRuleVideoConfig) asserts isCurrentThread - no dispatch needed.
    VideoEncoderConfig rule_config = config;
    rule_config.state.retry_time = 5;
    rule_config.state.status = configStateToString[VideoConfigSetState::NEW];
    _ruleConfigMap[profileToken] = rule_config;

    RuleReportContext ctx;
    ctx.logIds.push_back(logId);
    _ruleReportMap[profileToken] = ctx;

    if (!_device_ctr || !_device_ctr->isConnected()) {
        cb(controllerNotReady());
        return;
    }

    auto device_ctr = _device_ctr;
    auto poller = _poller;
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    // The set command is blocking SOAP: send it on the device queue, store the result on _poller.
    postDeviceTask([weak_self, device_ctr, profileToken, rule_config, cb, poller]() {
        onvifSetMediaProfile(device_ctr, profileToken, rule_config,
            [weak_self, poller, cb, profileToken](const SockException &ex) mutable {
                poller->async([weak_self, profileToken, ex, cb]() mutable {
                    if (auto self = weak_self.lock()) {
                        auto it = self->_ruleConfigMap.find(profileToken);
                        if (it != self->_ruleConfigMap.end()) {
                            // PROCESSING whatever the outcome: a camera can accept the set command
                            // without applying it, so only the next profile re-read may confirm the
                            // rule (SUCCESS + report) or retry it.
                            it->second.state.status = configStateToString[VideoConfigSetState::PROCESSING];
                        }
                    }
                    cb(ex);
                });
            },
            []() {});
    });
}

void CameraController::rollbackRuleVideoConfigAsync(const string &profileToken, const VideoEncoderConfig &baseline, const function<void(const SockException &ex)> &cb) {
    _ruleConfigMap.erase(profileToken);
    _ruleReportMap.erase(profileToken);

    if (!_device_ctr || !_device_ctr->isConnected()) {
        markBaselineNeedResync(profileToken);
        cb(controllerNotReady());
        return;
    }

    auto device_ctr = _device_ctr;
    auto poller = _poller;
    std::weak_ptr<CameraController> weak_self = shared_from_this();
    postDeviceTask([weak_self, device_ctr, profileToken, baseline, cb, poller]() {
        onvifSetMediaProfile(device_ctr, profileToken, baseline,
            [weak_self, poller, cb, profileToken](const SockException &ex) mutable {
                poller->async([weak_self, profileToken, ex, cb]() mutable {
                    if (ex) {
                        if (auto self = weak_self.lock()) {
                            self->markBaselineNeedResync(profileToken);
                        }
                    }
                    cb(ex);
                });
            },
            []() {});
    });
}

void CameraController::markBaselineNeedResync(const std::string &profileToken) {
    auto it = _profileConfigMap.find(profileToken);
    if (it == _profileConfigMap.end()) {
        return;
    }
    VideoEncoderConfig config = it->second;
    config.state.retry_time = 5;
    config.state.status = configStateToString[VideoConfigSetState::PROCESSING];
    addProfileConfig(profileToken, config, false);
}

} // namespace managerkit
