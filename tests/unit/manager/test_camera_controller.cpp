#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Camera/CameraController.h"
#include "Speaker/SpeakerController.h"
#include "ext-plugin/Bosch/BoschIpSpeaker.h"
#include "Camera/VideoConfigRuleDecision.h"
#include "Common/config.h"
#include "Poller/EventPoller.h"
#include "Util/NoticeCenter.h"
#include "server/WebApiErrCode.h"

using namespace managerkit;
using namespace toolkit;

namespace managerkit {

// Test-only access to the media profile sync (declared friend in CameraController.h).
class CameraControllerTestPeer {
public:
    static void syncMediaProfile(CameraController &c, const DeviceControl::Ptr &ctr) { c.syncMediaProfile(ctr); }

    static void onManager(CameraController &c) { c.onManager(); }

    static void setReady(CameraController &c, bool ready) { c._ready = ready; }

    // Lifts the 60 s pacing of the maintenance cycle.
    static void allowReconnect(CameraController &c) { c._pacer.skipWait(); }

    // The last attempt was rejected for its credentials: the cycle waits camera.control_auth_retry_sec.
    static bool authRejected(CameraController &c) { return c._pacer.authFailed(); }

    static ControlProtocol deviceProtocol(CameraController &c) {
        return c._device_ctr ? c._device_ctr->protocol() : ControlProtocol::NONE;
    }

    // Lets the next onManager() run (it is throttled to once per 60 s); `clock_synced_now` makes the
    // regular clock interval not due yet.
    static void allowNextCycle(CameraController &c, bool clock_synced_now) {
        c._pacer.skipWait();
        if (clock_synced_now) {
            c._last_clock_sync = time(nullptr);
        }
    }

    static bool hasPendingProfileConfig(CameraController &c) { return c.hasPendingProfileConfig(); }

    static bool profileConfig(CameraController &c, const std::string &token, VideoEncoderConfig &out) {
        auto it = c._profileConfigMap.find(token);
        if (it == c._profileConfigMap.end()) return false;
        out = it->second;
        return true;
    }

    static bool ruleConfig(CameraController &c, const std::string &token, VideoEncoderConfig &out) {
        auto it = c._ruleConfigMap.find(token);
        if (it == c._ruleConfigMap.end()) return false;
        out = it->second;
        return true;
    }
};

} // namespace managerkit

namespace {

// Records every device call; each call takes a little time so overlapping calls would be seen.
class FakeOnvifControl : public OnvifControl {
public:
    explicit FakeOnvifControl(EventPoller::Ptr camera_poller)
        : OnvifControl("127.0.0.1:1"), _camera_poller(std::move(camera_poller)) {}

    struct Call {
        std::string name;
        std::string arg;
        bool on_camera_poller = false;
    };

    bool isConnected() const override { return true; }
    bool connect() override { record("Connect", ""); return _connect_ok.load(); }
    bool probeConnection() override { record("Probe", ""); return _probe_ok.load(); }
    bool refreshMediaProfiles() override { return record("RefreshProfiles", ""); }
    void setCameraTimeManual() override { record("SetCameraTime", ""); }
    DeviceFailure lastFailure() const override { return _failure.load(); }
    bool takeClockSyncWanted() override { return _sync_wanted.exchange(false); }

    void setLastFailure(DeviceFailure failure) { _failure = failure; }
    void setClockSyncWanted(bool wanted) { _sync_wanted = wanted; }

    // `connect_ok` is false in the maintenance tests on purpose: the success branch registers a
    // gap-fill provider and publishes capabilities, which a unit test has no database for.
    void setMaintenanceResults(bool probe_ok, bool connect_ok) {
        _probe_ok = probe_ok;
        _connect_ok = connect_ok;
    }

    bool enablePTZ() override { return true; }
    OnvifPTZProfile getPTZProfile() override {
        OnvifPTZProfile profile;
        profile.isConsMoveEnable = true;
        return profile;
    }
    std::string getSoapErrMsg() override { return "fake error"; }

    bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout) override {
        return record("ContinuousMove", "");
    }
    bool PTZ_Stop(bool, bool) override { return record("Stop", ""); }
    bool PTZ_GotoPreset(const std::string &token, float, float, float) override { return record("GotoPreset", token); }

    OnvifMediaProfileMap getMediaProfilesInfo() override {
        std::lock_guard<std::mutex> lck(_mtx);
        return _profiles;
    }
    bool setVideoEncoderConfigByToken(const std::string &token, const VideoEncoderConfig &config) override {
        record("SetVideoEncoderConfig", token);
        std::lock_guard<std::mutex> lck(_mtx);
        _set_configs.push_back(config);
        return token != _failing_token;
    }

    // Configs of the SetVideoEncoderConfig calls, in order.
    std::vector<VideoEncoderConfig> setConfigs() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _set_configs;
    }

    void setProfiles(const OnvifMediaProfileMap &profiles, const std::string &failing_token) {
        std::lock_guard<std::mutex> lck(_mtx);
        _profiles = profiles;
        _failing_token = failing_token;
    }

    std::vector<Call> calls() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _calls;
    }
    int maxConcurrent() const { return _max_concurrent.load(); }

private:
    bool record(const std::string &name, const std::string &arg) {
        int now = ++_concurrent;
        int seen = _max_concurrent.load();
        while (now > seen && !_max_concurrent.compare_exchange_weak(seen, now)) {}
        {
            std::lock_guard<std::mutex> lck(_mtx);
            Call call;
            call.name = name;
            call.arg = arg;
            call.on_camera_poller = _camera_poller->isCurrentThread();
            _calls.push_back(call);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        --_concurrent;
        return true;
    }

    EventPoller::Ptr _camera_poller;
    std::mutex _mtx;
    std::vector<Call> _calls;
    std::vector<VideoEncoderConfig> _set_configs;
    OnvifMediaProfileMap _profiles;
    std::string _failing_token;
    std::atomic<int> _concurrent{0};
    std::atomic<int> _max_concurrent{0};
    std::atomic<bool> _probe_ok{true};
    std::atomic<bool> _connect_ok{true};
    std::atomic<DeviceFailure> _failure{DeviceFailure::None};
    std::atomic<bool> _sync_wanted{false};
};

class TestCameraController : public CameraController {
public:
    TestCameraController(const DeviceTuple &tuple, const EventPoller::Ptr &poller, std::shared_ptr<FakeOnvifControl> fake)
        : CameraController(tuple, poller), _fake(std::move(fake)) {}

protected:
    DeviceControl::Ptr createDeviceControl(ControlProtocol, const std::string &, const std::string &,
                                                 const std::string &) override {
        return _fake;
    }

private:
    std::shared_ptr<FakeOnvifControl> _fake;
};

struct CbProbe {
    std::mutex mtx;
    std::vector<int> codes;
    std::function<void(const SockException &)> cb() {
        auto self = this;
        return [self](const SockException &ex) {
            std::lock_guard<std::mutex> lck(self->mtx);
            self->codes.push_back(ex.getCustomCode());
        };
    }
    std::vector<int> get() {
        std::lock_guard<std::mutex> lck(mtx);
        return codes;
    }
};

bool waitFor(const std::function<bool()> &pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

class CameraControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        _poller = EventPollerPool::Instance().getPoller();
        _fake = std::make_shared<FakeOnvifControl>(_poller);
        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = "cam-controller-test";
        _controller = std::make_shared<TestCameraController>(tuple, _poller, _fake);
        CameraOption option;
        option.manufacturer = "ONVIF";
        option.ip = "127.0.0.1";
        option.port = 1;
        option.ptzMode = CameraOption::kPTZContinousMode;
        option.controlProtocol = "ONVIF";
        auto controller = _controller;
        _poller->sync([controller, option]() { controller->setupController(option); });
    }

    void onPoller(const std::function<void()> &task) { _poller->sync(task); }

    EventPoller::Ptr _poller;
    std::shared_ptr<FakeOnvifControl> _fake;
    std::shared_ptr<TestCameraController> _controller;
};

} // namespace

// Regression: the 1s continuous-move stop of a PTZ command was scheduled on a random poller and
// could not be cancelled, so it stopped the next PTZ command sent within that second.
TEST_F(CameraControllerTest, NewPtzCommandSupersedesPendingStop) {
    CbProbe first, second;
    auto controller = _controller;
    onPoller([&]() { controller->PTZMove("left", 50, first.cb()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    onPoller([&]() { controller->PTZMove("right", 50, second.cb()); });

    ASSERT_TRUE(waitFor([&]() { return !first.get().empty() && !second.get().empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // a stale stop would show up here

    auto calls = _fake->calls();
    ASSERT_EQ(3u, calls.size());
    EXPECT_EQ("ContinuousMove", calls[0].name);
    EXPECT_EQ("ContinuousMove", calls[1].name);
    EXPECT_EQ("Stop", calls[2].name);
    EXPECT_EQ(std::vector<int>{ApiErrCode::CODE_SUCCESS}, first.get());
    EXPECT_EQ(std::vector<int>{ApiErrCode::CODE_SUCCESS}, second.get());
    for (const auto &call : calls) {
        EXPECT_FALSE(call.on_camera_poller) << call.name;
    }
}

TEST_F(CameraControllerTest, CommandsOfOneCameraRunInOrderWithoutOverlap) {
    CbProbe move, preset;
    auto controller = _controller;
    onPoller([&]() {
        controller->PTZMove("left", 50, move.cb());
        controller->PTZGotoPreset("preset-1", false, preset.cb());
    });
    ASSERT_TRUE(waitFor([&]() { return !move.get().empty() && !preset.get().empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // past the 1s stop delay

    auto calls = _fake->calls();
    ASSERT_EQ(2u, calls.size()) << "the preset move supersedes the pending stop";
    EXPECT_EQ("ContinuousMove", calls[0].name);
    EXPECT_EQ("GotoPreset", calls[1].name);
    EXPECT_EQ("preset-1", calls[1].arg);
    EXPECT_EQ(1, _fake->maxConcurrent());
    EXPECT_EQ(std::vector<int>{ApiErrCode::CODE_SUCCESS}, preset.get());
}

// Regression: syncMediaProfile ran on a worker thread and modified _profileConfigMap there. The map
// is now only changed on the camera poller; the set commands still run off it.
TEST_F(CameraControllerTest, MediaProfileSyncUpdatesMapOnPollerOnly) {
    auto makeConfig = [](int fps, const std::string &status, int retry) {
        VideoEncoderConfig config;
        config.vcodec = "H264";
        config.width = 1920;
        config.height = 1080;
        config.bitrate = 4096;
        config.fps = fps;
        config.quality = 0;
        config.state.status = status;
        config.state.retry_time = retry;
        return config;
    };
    auto makeProfile = [](const std::string &token, int fps) {
        OnvifMediaProfile profile;
        profile.token = token;
        profile.vcodec = "H264";
        profile.width = 1920;
        profile.height = 1080;
        profile.bitrate = 4096;
        profile.fps = fps;
        return profile;
    };
    const std::string kNew = configStateToString[VideoConfigSetState::NEW];
    auto controller = _controller;
    onPoller([&]() {
        controller->addProfileConfig("p-retry", makeConfig(25, kNew, 5), false);   // camera differs, set succeeds
        controller->addProfileConfig("p-fail", makeConfig(25, kNew, 5), false);    // camera differs, set fails
        controller->addProfileConfig("p-match", makeConfig(15, kNew, 5), false);   // camera matches
    });
    _fake->setProfiles({makeProfile("p-retry", 15), makeProfile("p-fail", 15), makeProfile("p-match", 15)}, "p-fail");

    // Every map update goes through addProfileConfig, which emits kBroadcastStreamSettingChange:
    // record the thread of each emission for this camera.
    static int s_tag;
    std::mutex updates_mtx;
    std::vector<bool> updates_on_poller;
    auto poller = _poller;
    NoticeCenter::Instance().addListener(&s_tag, mediakit::Broadcast::kBroadcastStreamSettingChange,
        [&updates_mtx, &updates_on_poller, poller](BroadcastStreamSettingChangeArgs) {
            if (args.device_id != "cam-controller-test") return;
            std::lock_guard<std::mutex> lck(updates_mtx);
            updates_on_poller.push_back(poller->isCurrentThread());
        });

    // Called like the connect task does, from a thread that is not the camera poller.
    std::thread device([&]() { CameraControllerTestPeer::syncMediaProfile(*controller, _fake); });
    device.join();

    auto read = [&](const std::string &token) {
        VideoEncoderConfig config;
        onPoller([&]() { CameraControllerTestPeer::profileConfig(*controller, token, config); });
        return config;
    };
    const std::string kProcessing = configStateToString[VideoConfigSetState::PROCESSING];
    const std::string kFailed = configStateToString[VideoConfigSetState::FAILED];
    const std::string kSuccess = configStateToString[VideoConfigSetState::SUCCESS];
    ASSERT_TRUE(waitFor([&]() {
        return read("p-retry").state.status == kProcessing && read("p-fail").state.status == kFailed;
    }));
    EXPECT_EQ(4, read("p-retry").state.retry_time);
    EXPECT_EQ(0, read("p-fail").state.retry_time);
    EXPECT_EQ(kSuccess, read("p-match").state.status);

    auto calls = _fake->calls();
    ASSERT_EQ(2u, calls.size());
    for (const auto &call : calls) {
        EXPECT_EQ("SetVideoEncoderConfig", call.name);
        EXPECT_FALSE(call.on_camera_poller);
    }

    NoticeCenter::Instance().delListener(&s_tag, mediakit::Broadcast::kBroadcastStreamSettingChange);
    std::lock_guard<std::mutex> lck(updates_mtx);
    EXPECT_EQ(3u, updates_on_poller.size());
    for (size_t i = 0; i < updates_on_poller.size(); ++i) {
        EXPECT_TRUE(updates_on_poller[i]) << "profile map updated off the camera poller (update " << i << ")";
    }
}

// ── OnvifControl PTZ capability parsing ──────────────────────────────────────
//
// Regression cover for the crash found by the 300-camera stress run: a camera that reports
// pan/tilt but no zoom space (legal ONVIF) used to make getDeviceCapabilities() index an empty
// vector and take the whole MediaServer process down.
namespace {

// Owns the gSOAP response objects for one case. They are plain classes, so the test can build
// them on the heap and wire the raw pointers by hand — no soap context needed for pure parsing.
class PTZOptionsBuilder {
public:
    tt__Space2DDescription *panTilt(float min_x, float max_x, float min_y, float max_y) {
        auto *space = add(new tt__Space2DDescription());
        space->XRange = range(min_x, max_x);
        space->YRange = range(min_y, max_y);
        return space;
    }

    tt__Space1DDescription *zoom(float min_v, float max_v) {
        auto *space = add1d(new tt__Space1DDescription());
        space->XRange = range(min_v, max_v);
        return space;
    }

    tt__PTZSpaces *spaces() {
        if (!_spaces) {
            _spaces.reset(new tt__PTZSpaces());
        }
        return _spaces.get();
    }

    // `with_spaces == false` reproduces a device whose PTZConfigurationOptions has no Spaces.
    _tptz__GetConfigurationOptionsResponse response(bool with_options = true, bool with_spaces = true) {
        _tptz__GetConfigurationOptionsResponse resp;
        resp.PTZConfigurationOptions = nullptr;
        if (!with_options) {
            return resp;
        }
        if (!_options) {
            _options.reset(new tt__PTZConfigurationOptions());
        }
        _options->Spaces = with_spaces ? spaces() : nullptr;
        resp.PTZConfigurationOptions = _options.get();
        return resp;
    }

private:
    tt__FloatRange *range(float min_v, float max_v) {
        std::unique_ptr<tt__FloatRange> r(new tt__FloatRange());
        r->Min = min_v;
        r->Max = max_v;
        _ranges.push_back(std::move(r));
        return _ranges.back().get();
    }
    tt__Space2DDescription *add(tt__Space2DDescription *s) {
        _spaces2d.push_back(std::unique_ptr<tt__Space2DDescription>(s));
        return s;
    }
    tt__Space1DDescription *add1d(tt__Space1DDescription *s) {
        _spaces1d.push_back(std::unique_ptr<tt__Space1DDescription>(s));
        return s;
    }

    std::vector<std::unique_ptr<tt__FloatRange>> _ranges;
    std::vector<std::unique_ptr<tt__Space2DDescription>> _spaces2d;
    std::vector<std::unique_ptr<tt__Space1DDescription>> _spaces1d;
    std::unique_ptr<tt__PTZSpaces> _spaces;
    std::unique_ptr<tt__PTZConfigurationOptions> _options;
};

} // namespace

TEST(OnvifPTZSpacesTest, ReadsEveryRangeWhenTheDeviceReportsAllSpaces) {
    PTZOptionsBuilder builder;
    auto *spaces = builder.spaces();
    spaces->AbsolutePanTiltPositionSpace.push_back(builder.panTilt(-1.0f, 1.0f, -0.5f, 0.5f));
    spaces->AbsoluteZoomPositionSpace.push_back(builder.zoom(0.0f, 1.0f));
    spaces->ContinuousPanTiltVelocitySpace.push_back(builder.panTilt(-0.9f, 0.9f, -0.4f, 0.4f));
    spaces->ContinuousZoomVelocitySpace.push_back(builder.zoom(-0.8f, 0.8f));
    spaces->RelativePanTiltTranslationSpace.push_back(builder.panTilt(-0.7f, 0.7f, -0.3f, 0.3f));
    spaces->RelativeZoomTranslationSpace.push_back(builder.zoom(-0.6f, 0.6f));

    OnvifPTZProfile profile;
    OnvifControl::fillPTZSpaces(builder.response(), profile);

    EXPECT_TRUE(profile.isAbsMoveEnable);
    EXPECT_TRUE(profile.isConsMoveEnable);
    EXPECT_TRUE(profile.isRelMoveEnable);
    EXPECT_FLOAT_EQ(-1.0f, profile.absMinPan);
    EXPECT_FLOAT_EQ(0.5f, profile.absMaxTilt);
    EXPECT_FLOAT_EQ(1.0f, profile.absMaxZoom);
    EXPECT_FLOAT_EQ(-0.8f, profile.consMinZoom);
    EXPECT_FLOAT_EQ(0.6f, profile.relMaxZoom);
}

// The case that crashed: pan/tilt present, zoom absent.
TEST(OnvifPTZSpacesTest, SurvivesPanTiltOnlyDeviceAndKeepsMoveEnabled) {
    PTZOptionsBuilder builder;
    auto *spaces = builder.spaces();
    spaces->AbsolutePanTiltPositionSpace.push_back(builder.panTilt(-1.0f, 1.0f, -1.0f, 1.0f));
    spaces->ContinuousPanTiltVelocitySpace.push_back(builder.panTilt(-1.0f, 1.0f, -1.0f, 1.0f));
    spaces->RelativePanTiltTranslationSpace.push_back(builder.panTilt(-1.0f, 1.0f, -1.0f, 1.0f));
    // no zoom space at all

    OnvifPTZProfile profile;
    const OnvifPTZProfile defaults;
    OnvifControl::fillPTZSpaces(builder.response(), profile);

    // Pan/tilt still drives the capability flags; presets depend on isAbsMoveEnable.
    EXPECT_TRUE(profile.isAbsMoveEnable);
    EXPECT_TRUE(profile.isConsMoveEnable);
    EXPECT_TRUE(profile.isRelMoveEnable);
    EXPECT_FLOAT_EQ(-1.0f, profile.absMinPan);
    // Zoom keeps its defaults rather than reading past the end of an empty vector.
    EXPECT_FLOAT_EQ(defaults.absMinZoom, profile.absMinZoom);
    EXPECT_FLOAT_EQ(defaults.absMaxZoom, profile.absMaxZoom);
    EXPECT_FLOAT_EQ(defaults.consMinZoom, profile.consMinZoom);
    EXPECT_FLOAT_EQ(defaults.relMaxZoom, profile.relMaxZoom);
}

TEST(OnvifPTZSpacesTest, ToleratesMissingSpacesAndMissingOptions) {
    PTZOptionsBuilder builder;
    const OnvifPTZProfile defaults;

    OnvifPTZProfile no_spaces;
    OnvifControl::fillPTZSpaces(builder.response(true, false), no_spaces);
    EXPECT_FALSE(no_spaces.isAbsMoveEnable);
    EXPECT_FLOAT_EQ(defaults.absMinPan, no_spaces.absMinPan);

    OnvifPTZProfile no_options;
    OnvifControl::fillPTZSpaces(builder.response(false), no_options);
    EXPECT_FALSE(no_options.isAbsMoveEnable);
}

TEST(OnvifPTZSpacesTest, ToleratesNullEntriesAndNullRanges) {
    PTZOptionsBuilder builder;
    auto *spaces = builder.spaces();
    spaces->AbsolutePanTiltPositionSpace.push_back(nullptr);
    // A space whose required XRange/YRange the device omitted.
    auto *bare = builder.panTilt(0.0f, 0.0f, 0.0f, 0.0f);
    bare->XRange = nullptr;
    bare->YRange = nullptr;
    spaces->ContinuousPanTiltVelocitySpace.push_back(bare);
    spaces->ContinuousZoomVelocitySpace.push_back(nullptr);

    OnvifPTZProfile profile;
    const OnvifPTZProfile defaults;
    OnvifControl::fillPTZSpaces(builder.response(), profile);

    // A null first entry means "no usable space": the capability must stay off.
    EXPECT_FALSE(profile.isAbsMoveEnable);
    // A present space with no ranges still counts as supported, values stay at defaults.
    EXPECT_TRUE(profile.isConsMoveEnable);
    EXPECT_FLOAT_EQ(defaults.consMinPan, profile.consMinPan);
    EXPECT_FLOAT_EQ(defaults.consMinZoom, profile.consMinZoom);
}

TEST(OnvifPTZProfilePickTest, SkipsProfilesWithoutPTZAndHandlesEmptyList) {
    _trt__GetProfilesResponse empty;
    EXPECT_EQ(nullptr, OnvifControl::pickPTZProfile(empty));

    std::unique_ptr<tt__Profile> without(new tt__Profile());
    without->PTZConfiguration = nullptr;
    std::unique_ptr<tt__Profile> with(new tt__Profile());
    std::unique_ptr<tt__PTZConfiguration> config(new tt__PTZConfiguration());
    config->token = "ptz-token";
    with->PTZConfiguration = config.get();
    with->token = "profile-2";

    _trt__GetProfilesResponse resp;
    resp.Profiles.push_back(nullptr);           // gSOAP can leave a null entry
    resp.Profiles.push_back(without.get());     // first real profile has no PTZ
    resp.Profiles.push_back(with.get());
    tt__Profile *picked = OnvifControl::pickPTZProfile(resp);
    ASSERT_NE(nullptr, picked);
    EXPECT_EQ("profile-2", picked->token);
}

// ---------------------------------------------------------------------------
// V3: the camera-clock offset cache that removes a GetSystemDateAndTime round
// trip from every ONVIF operation. Both halves of the decision are pure, so
// they are tested here without a camera; the traffic drop itself is measured by
// the stress harness.
// ---------------------------------------------------------------------------

TEST(OnvifClockOffsetCacheTest, ReusesTheEntryUntilTheTtlRunsOut) {
    const time_t stamped_at = 1000000;
    EXPECT_TRUE(OnvifControl::clockOffsetFresh(stamped_at, stamped_at, 300));
    EXPECT_TRUE(OnvifControl::clockOffsetFresh(stamped_at, stamped_at + 299, 300));
    // Exactly at the TTL the entry is already stale.
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(stamped_at, stamped_at + 300, 300));
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(stamped_at, stamped_at + 3600, 300));
}

TEST(OnvifClockOffsetCacheTest, NeverUsesAnEmptyOrDisabledCache) {
    const time_t now = 1000000;
    // 0 is the "no entry yet" sentinel, and an invalidated entry is set back to it.
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(0, now, 300));
    // ttl <= 0 restores the old behaviour: measure the camera clock every time.
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(now, now, 0));
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(now, now, -1));
}

TEST(OnvifClockOffsetCacheTest, DiscardsTheEntryWhenTheLocalClockJumpsBack) {
    const time_t stamped_at = 1000000;
    // NTP stepped the host clock backwards: the entry's age is unknowable.
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(stamped_at, stamped_at - 1, 300));
    EXPECT_FALSE(OnvifControl::clockOffsetFresh(stamped_at, stamped_at - 86400, 300));
}

TEST(OnvifAuthFaultTest, DropsTheOffsetOnTimestampAndAuthFaults) {
    EXPECT_TRUE(OnvifControl::isAuthOrTimestampFault(
        "SOAP-ENV:Sender[wsse:FailedAuthentication] The security token could not be authenticated"));
    EXPECT_TRUE(OnvifControl::isAuthOrTimestampFault("wsse:InvalidSecurityToken"));
    EXPECT_TRUE(OnvifControl::isAuthOrTimestampFault("wsu:MessageExpired"));
    EXPECT_TRUE(OnvifControl::isAuthOrTimestampFault("ter:NotAuthorized"));
    EXPECT_TRUE(OnvifControl::isAuthOrTimestampFault("Sender not Authorized"));
    // Cameras are inconsistent about case in fault strings.
    EXPECT_TRUE(OnvifControl::isAuthOrTimestampFault("FAILEDAUTHENTICATION"));
}

TEST(OnvifAuthFaultTest, KeepsTheOffsetOnFaultsThatSayNothingAboutTheClock) {
    EXPECT_FALSE(OnvifControl::isAuthOrTimestampFault(""));
    EXPECT_FALSE(OnvifControl::isAuthOrTimestampFault("ter:ActionNotSupported"));
    EXPECT_FALSE(OnvifControl::isAuthOrTimestampFault("End of file or no input: Operation timed out"));
    EXPECT_FALSE(OnvifControl::isAuthOrTimestampFault("Connection refused"));
    EXPECT_FALSE(OnvifControl::isAuthOrTimestampFault("ter:InvalidArgVal ter:NoProfile"));
}

// AUTO tries PSIA only after NOT_THIS_PROTOCOL: a slow or locked-out ONVIF camera must never land there.
TEST(OnvifConnectErrorTest, OnlyAClearNonOnvifAnswerIsNotThisProtocol) {
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, OnvifControl::classifyConnectError(404, ""));
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, OnvifControl::classifyConnectError(405, ""));
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, OnvifControl::classifyConnectError(SOAP_NO_TAG, ""));
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, OnvifControl::classifyConnectError(SOAP_TAG_MISMATCH, ""));

    EXPECT_EQ(ConnectError::UNREACHABLE, OnvifControl::classifyConnectError(SOAP_EOF, "Operation timed out"));
    EXPECT_EQ(ConnectError::UNREACHABLE, OnvifControl::classifyConnectError(SOAP_TCP_ERROR, "Connection refused"));
    EXPECT_EQ(ConnectError::AUTH_FAILED, OnvifControl::classifyConnectError(401, ""));
    EXPECT_EQ(ConnectError::AUTH_FAILED, OnvifControl::classifyConnectError(SOAP_FAULT, "ter:NotAuthorized"));
    EXPECT_EQ(ConnectError::OTHER, OnvifControl::classifyConnectError(SOAP_FAULT, "ter:ActionNotSupported"));
    EXPECT_EQ(ConnectError::OTHER, OnvifControl::classifyConnectError(500, ""));
}

// ── V4: the maintenance cycle of a camera that is already up ─────────────────────────────────────
// It used to redo the whole OnvifControl::connect() handshake every 60s for every camera (~16 ONVIF
// operations each). At 300 cameras that background traffic alone filled the shared `device` pool, so
// PTZ commands queued behind it and timed out.

namespace {

// Names of the device calls recorded so far, in order.
std::vector<std::string> callNames(const std::shared_ptr<FakeOnvifControl> &fake) {
    std::vector<std::string> names;
    for (const auto &call : fake->calls()) {
        names.push_back(call.name);
    }
    return names;
}

bool sawCall(const std::shared_ptr<FakeOnvifControl> &fake, const std::string &name) {
    auto names = callNames(fake);
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_F(CameraControllerTest, ReadyControllerIsProbedInsteadOfReconnected) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(true, false);
    onPoller([&]() { CameraControllerTestPeer::setReady(*controller, true); });

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });

    // On the very first cycle both the clock check and the profile re-read are due.
    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "RefreshProfiles"); }));
    EXPECT_EQ(std::vector<std::string>({"Probe", "SetCameraTime", "RefreshProfiles"}), callNames(fake));
    // The whole point: no handshake.
    EXPECT_FALSE(sawCall(fake, "Connect"));
    EXPECT_TRUE(controller->isReady());
}

TEST_F(CameraControllerTest, ProbeFailureFallsBackToFullConnect) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(false, false);
    onPoller([&]() { CameraControllerTestPeer::setReady(*controller, true); });

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });

    // A camera that stops answering the probe still gets the full reconnect, and the failure is
    // published as before.
    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "Connect"); }));
    EXPECT_EQ(std::vector<std::string>({"Probe", "Connect"}), callNames(fake));
    EXPECT_TRUE(waitFor([&]() { return !controller->isReady(); }));
}

TEST_F(CameraControllerTest, ControllerThatWasNeverReadyIsNotProbed) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(true, false);
    // _ready is false: there is no connection to probe yet.

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });

    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "Connect"); }));
    EXPECT_FALSE(sawCall(fake, "Probe"));
}

// A config this server is still trying to push must keep being retried on every cycle, whatever
// `onvif.profileResyncIntervalSec` says — that is what hasPendingProfileConfig() decides.
TEST_F(CameraControllerTest, PendingProfileConfigIsOnlyNewOrProcessing) {
    auto controller = _controller;
    auto config = [](const std::string &status) {
        VideoEncoderConfig out;
        out.vcodec = "H264";
        out.width = 1920;
        out.height = 1080;
        out.bitrate = 4096;
        out.fps = 25;
        out.state.status = status;
        out.state.retry_time = 5;
        return out;
    };
    auto pending = [&]() {
        bool result = false;
        onPoller([&]() { result = CameraControllerTestPeer::hasPendingProfileConfig(*controller); });
        return result;
    };

    EXPECT_FALSE(pending()); // nothing configured yet

    // A settled profile is not a reason to re-read anything.
    onPoller([&]() {
        controller->addProfileConfig("p-success", config(configStateToString[VideoConfigSetState::SUCCESS]), false);
        controller->addProfileConfig("p-external", config(configStateToString[VideoConfigSetState::EXTERNAL]), false);
        controller->addProfileConfig("p-failed", config(configStateToString[VideoConfigSetState::FAILED]), false);
    });
    EXPECT_FALSE(pending());

    onPoller([&]() {
        controller->addProfileConfig("p-new", config(configStateToString[VideoConfigSetState::NEW]), false);
    });
    EXPECT_TRUE(pending());

    onPoller([&]() {
        controller->addProfileConfig("p-new", config(configStateToString[VideoConfigSetState::SUCCESS]), false);
        controller->addProfileConfig("p-processing", config(configStateToString[VideoConfigSetState::PROCESSING]), false);
    });
    EXPECT_TRUE(pending());
}

// ── Video config rule jobs ───────────────────────────────────────────────────────────────────────
// A rule is time-boxed (applyDuration). Its result used to be reported only by a profile re-read,
// and hasPendingProfileConfig() ignored rules, so with the default 900 s re-read interval a rule
// that the camera had applied at once was still reported CONTROLLER_OFFLINE when it expired.

namespace {

const char *kRuleToken = "p-rule";

VideoEncoderConfig ruleConfig() {
    VideoEncoderConfig config;
    config.vcodec = "H264";
    config.width = 1280;
    config.height = 720;
    config.bitrate = 1024;
    config.fps = 10;
    config.quality = 0;
    return config;
}

OnvifMediaProfile ruleProfile(bool matches_rule) {
    OnvifMediaProfile profile;
    profile.token = kRuleToken;
    profile.vcodec = "H264";
    profile.width = matches_rule ? 1280 : 1920;
    profile.height = matches_rule ? 720 : 1080;
    profile.bitrate = matches_rule ? 1024 : 4096;
    profile.fps = matches_rule ? 10 : 25;
    return profile;
}

// Collects the kBroadcastVideoConfigRuleResult emissions of the test camera.
class RuleResultRecorder {
public:
    struct Result {
        std::string log_id;
        bool success = false;
        std::string err_code;
    };

    RuleResultRecorder() {
        auto self = this;
        NoticeCenter::Instance().addListener(this, mediakit::Broadcast::kBroadcastVideoConfigRuleResult,
            [self](BroadcastVideoConfigRuleResultArgs) {
                if (device_id != "cam-controller-test") return;
                Result result;
                result.log_id = log_id;
                result.success = success;
                result.err_code = err_code;
                std::lock_guard<std::mutex> lck(self->_mtx);
                self->_results.push_back(result);
            });
    }

    ~RuleResultRecorder() {
        NoticeCenter::Instance().delListener(this, mediakit::Broadcast::kBroadcastVideoConfigRuleResult);
    }

    std::vector<Result> results() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _results;
    }

private:
    std::mutex _mtx;
    std::vector<Result> _results;
};

int setCallCount(const std::shared_ptr<FakeOnvifControl> &fake) {
    int count = 0;
    for (const auto &call : fake->calls()) {
        if (call.name == "SetVideoEncoderConfig") ++count;
    }
    return count;
}

} // namespace

class CameraControllerRuleTest : public CameraControllerTest {
protected:
    VideoEncoderConfig readRule() {
        VideoEncoderConfig config;
        auto controller = _controller;
        onPoller([&]() { CameraControllerTestPeer::ruleConfig(*controller, kRuleToken, config); });
        return config;
    }

    bool pending() {
        bool result = false;
        auto controller = _controller;
        onPoller([&]() { result = CameraControllerTestPeer::hasPendingProfileConfig(*controller); });
        return result;
    }

    // Applies the rule and waits for the direct set command to finish; returns its error code.
    int applyRule() {
        CbProbe probe;
        auto controller = _controller;
        auto cb = probe.cb();
        onPoller([&]() { controller->applyRuleVideoConfigAsync("log-1", kRuleToken, ruleConfig(), cb); });
        EXPECT_TRUE(waitFor([&]() { return !probe.get().empty(); }));
        auto codes = probe.get();
        return codes.empty() ? -1 : codes.front();
    }

    // One profile re-read, driven like the maintenance cycle does from the device queue.
    void syncOnce() {
        auto controller = _controller;
        auto fake = _fake;
        std::thread device([controller, fake]() { CameraControllerTestPeer::syncMediaProfile(*controller, fake); });
        device.join();
        // The decisions and the stored set results are queued on the poller: let them drain,
        // then let the set commands the decisions posted run and store their results as well.
        for (int i = 0; i < 3; ++i) {
            onPoller([]() {});
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    }
};

TEST_F(CameraControllerRuleTest, PendingRuleKeepsTheProfileReReadDue) {
    _fake->setProfiles({ruleProfile(true)}, "");
    EXPECT_FALSE(pending());

    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    // The set command succeeded but nothing has confirmed the camera applied it yet.
    EXPECT_EQ(configStateToString[VideoConfigSetState::PROCESSING], readRule().state.status);
    EXPECT_TRUE(pending());

    syncOnce();
    EXPECT_EQ(configStateToString[VideoConfigSetState::SUCCESS], readRule().state.status);
    EXPECT_FALSE(pending());
}

TEST_F(CameraControllerRuleTest, AppliedRuleIsReportedOnceWhenTheReReadConfirmsIt) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(true)}, "");

    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    EXPECT_TRUE(recorder.results().empty()) << "reported before the camera config was read back";

    syncOnce();
    syncOnce();
    auto results = recorder.results();
    ASSERT_EQ(1u, results.size());
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ("log-1", results[0].log_id);
    EXPECT_EQ(1, setCallCount(_fake)); // confirmed without pushing the rule again
}

TEST_F(CameraControllerRuleTest, FailedDirectSetIsRetriedInsteadOfReported) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(false)}, kRuleToken);

    EXPECT_EQ(ApiErrCode::CODE_OTHER_EXCEPTION, applyRule());
    EXPECT_TRUE(recorder.results().empty());
    EXPECT_TRUE(pending());

    // The camera answers again: the re-read pushes the rule once more, then confirms it.
    _fake->setProfiles({ruleProfile(false)}, "");
    syncOnce();
    EXPECT_EQ(2, setCallCount(_fake));
    EXPECT_TRUE(recorder.results().empty());

    _fake->setProfiles({ruleProfile(true)}, "");
    syncOnce();
    auto results = recorder.results();
    ASSERT_EQ(1u, results.size());
    EXPECT_TRUE(results[0].success);
}

TEST_F(CameraControllerRuleTest, RuleTheCameraNeverAppliesIsReportedOnceAfterTheRetries) {
    RuleResultRecorder recorder;
    // The camera accepts every set command but keeps its old config.
    _fake->setProfiles({ruleProfile(false)}, "");

    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    for (int i = 0; i < 8 && recorder.results().empty(); ++i) {
        syncOnce();
    }
    syncOnce(); // one more re-read must not report again

    auto results = recorder.results();
    ASSERT_EQ(1u, results.size());
    EXPECT_FALSE(results[0].success);
    EXPECT_EQ("CONFIG_NOT_APPLIED", results[0].err_code);
    EXPECT_EQ(1 + 5, setCallCount(_fake)); // the direct set, then retry_time re-pushes
    EXPECT_FALSE(pending());
}

// ── Requests that extend a running rule ──────────────────────────────────────────────────────────
// A caller keeps a config by sending it again before it expires. That request used to be dropped
// because it matched the running job: the job still rolled back at its first deadline, and a job
// queued behind it still ran although the newest request asked for the running config.

namespace {

VideoEncoderConfig ruleVariant(int fps) {
    VideoEncoderConfig config = ruleConfig();
    config.fps = fps;
    return config;
}

} // namespace

TEST(VideoConfigRuleDecisionTest, FirstRequestStartsUnlessItAsksForTheBaseline) {
    const VideoEncoderConfig baseline = ruleVariant(25);
    auto start = decideRuleRequest(false, baseline, 0, false, baseline, ruleVariant(10), 5000);
    EXPECT_EQ(RuleRequestAction::START, start.action);
    EXPECT_EQ(5000, start.deadlineAt);
    EXPECT_FALSE(start.supersedeQueued);

    auto noop = decideRuleRequest(false, baseline, 0, false, baseline, ruleVariant(25), 5000);
    EXPECT_EQ(RuleRequestAction::ALREADY_APPLIED, noop.action);
}

TEST(VideoConfigRuleDecisionTest, SameConfigExtendsToTheLaterDeadline) {
    const VideoEncoderConfig baseline = ruleVariant(25);
    const VideoEncoderConfig active = ruleVariant(10);

    auto later = decideRuleRequest(true, active, 3000, false, baseline, ruleVariant(10), 9000);
    EXPECT_EQ(RuleRequestAction::EXTEND_ACTIVE, later.action);
    EXPECT_EQ(9000, later.deadlineAt);
    EXPECT_FALSE(later.supersedeQueued);

    // A shorter request never cuts the running job short.
    auto shorter = decideRuleRequest(true, active, 9000, false, baseline, ruleVariant(10), 3000);
    EXPECT_EQ(RuleRequestAction::EXTEND_ACTIVE, shorter.action);
    EXPECT_EQ(9000, shorter.deadlineAt);
}

TEST(VideoConfigRuleDecisionTest, ExtendingDropsTheWaitingJob) {
    const VideoEncoderConfig baseline = ruleVariant(25);
    auto decision = decideRuleRequest(true, ruleVariant(10), 3000, true, baseline, ruleVariant(10), 9000);
    EXPECT_EQ(RuleRequestAction::EXTEND_ACTIVE, decision.action);
    EXPECT_TRUE(decision.supersedeQueued);
}

TEST(VideoConfigRuleDecisionTest, DifferentConfigQueuesAndReplacesTheWaitingJob) {
    const VideoEncoderConfig baseline = ruleVariant(25);
    auto first = decideRuleRequest(true, ruleVariant(10), 3000, false, baseline, ruleVariant(15), 9000);
    EXPECT_EQ(RuleRequestAction::QUEUE, first.action);
    EXPECT_EQ(9000, first.deadlineAt);
    EXPECT_FALSE(first.supersedeQueued);

    // Same config as the waiting job still replaces it: the newest request carries the logId.
    auto again = decideRuleRequest(true, ruleVariant(10), 3000, true, baseline, ruleVariant(15), 9500);
    EXPECT_EQ(RuleRequestAction::QUEUE, again.action);
    EXPECT_TRUE(again.supersedeQueued);
}

TEST(VideoConfigRuleDecisionTest, BaselineRequestDuringAJobWaitsForItsRollback) {
    const VideoEncoderConfig baseline = ruleVariant(25);
    auto decision = decideRuleRequest(true, ruleVariant(10), 3000, false, baseline, ruleVariant(25), 9000);
    EXPECT_EQ(RuleRequestAction::QUEUE, decision.action);
}

class CameraControllerRuleExtendTest : public CameraControllerRuleTest {
protected:
    CameraController::RuleAttachResult attach(const std::string &log_id) {
        auto controller = _controller;
        auto result = CameraController::RuleAttachResult::NEEDS_REAPPLY;
        onPoller([&]() { result = controller->attachRuleLogId(kRuleToken, log_id); });
        return result;
    }

    bool failPending(const std::string &code) {
        auto controller = _controller;
        bool result = false;
        onPoller([&]() { result = controller->reportRuleFailureIfPending(kRuleToken, code, "msg"); });
        return result;
    }

    static std::vector<std::string> logIds(const std::vector<RuleResultRecorder::Result> &results) {
        std::vector<std::string> ids;
        for (const auto &result : results) ids.push_back(result.log_id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }
};

TEST_F(CameraControllerRuleExtendTest, WithoutARuleTheConfigMustBeAppliedAgain) {
    EXPECT_EQ(CameraController::RuleAttachResult::NEEDS_REAPPLY, attach("log-2"));
}

TEST_F(CameraControllerRuleExtendTest, ExtensionBeforeTheConfirmationSharesTheResult) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(true)}, "");
    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());

    EXPECT_EQ(CameraController::RuleAttachResult::ATTACHED, attach("log-2"));
    EXPECT_TRUE(recorder.results().empty());

    syncOnce();
    auto results = recorder.results();
    ASSERT_EQ(2u, results.size());
    EXPECT_EQ(std::vector<std::string>({"log-1", "log-2"}), logIds(results));
    for (const auto &result : results) EXPECT_TRUE(result.success);
}

TEST_F(CameraControllerRuleExtendTest, ExtensionAfterTheConfirmationSucceedsAtOnce) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(true)}, "");
    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    syncOnce();
    ASSERT_EQ(1u, recorder.results().size());

    EXPECT_EQ(CameraController::RuleAttachResult::ATTACHED, attach("log-2"));
    auto results = recorder.results();
    ASSERT_EQ(2u, results.size());
    EXPECT_EQ("log-2", results[1].log_id);
    EXPECT_TRUE(results[1].success);
    EXPECT_EQ(1, setCallCount(_fake)); // nothing pushed again
}

TEST_F(CameraControllerRuleExtendTest, ExtensionOfARuleTheCameraRefusedIsAppliedAgain) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(false)}, "");
    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    _fake->setProfiles({ruleProfile(false)}, kRuleToken); // the re-read push fails: SET_CONFIG_FAILED
    syncOnce();
    ASSERT_EQ(1u, recorder.results().size());
    ASSERT_FALSE(recorder.results()[0].success);

    EXPECT_EQ(CameraController::RuleAttachResult::NEEDS_REAPPLY, attach("log-2"));
    EXPECT_EQ(1u, recorder.results().size()); // the failure is not repeated for log-2
}

// A job recovered after a restart whose result was already reported: the camera's confirmation
// does not report it again, and an extension is answered from the stored outcome.
TEST_F(CameraControllerRuleExtendTest, RecoveredRuleReportedBeforeTheRestartIsNotReportedAgain) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(true)}, "");
    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    auto controller = _controller;
    onPoller([&]() { controller->markRuleResultReported(kRuleToken, true); });

    syncOnce();
    EXPECT_TRUE(recorder.results().empty());
    EXPECT_FALSE(failPending("CONFIG_NOT_CONFIRMED"));
    EXPECT_TRUE(recorder.results().empty());

    EXPECT_EQ(CameraController::RuleAttachResult::ATTACHED, attach("log-2"));
    auto results = recorder.results();
    ASSERT_EQ(1u, results.size());
    EXPECT_EQ("log-2", results[0].log_id);
    EXPECT_TRUE(results[0].success);
}

TEST_F(CameraControllerRuleExtendTest, ExpiryFailureReachesEveryRequestOnce) {
    RuleResultRecorder recorder;
    _fake->setProfiles({ruleProfile(false)}, "");
    ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
    ASSERT_EQ(CameraController::RuleAttachResult::ATTACHED, attach("log-2"));

    EXPECT_TRUE(failPending("CONFIG_NOT_CONFIRMED"));
    EXPECT_FALSE(failPending("CONFIG_NOT_CONFIRMED"));
    auto results = recorder.results();
    ASSERT_EQ(2u, results.size());
    EXPECT_EQ(std::vector<std::string>({"log-1", "log-2"}), logIds(results));
    for (const auto &result : results) {
        EXPECT_FALSE(result.success);
        EXPECT_EQ("CONFIG_NOT_CONFIRMED", result.err_code);
    }
}

// onvifSetVideoConfigs while a video config rule holds the profile: only the baseline changes, and
// the rollback at the end of the rule pushes it.
class CameraControllerDeferredSetTest : public CameraControllerRuleTest {
protected:
    struct SetOutcome {
        bool error = true;
        int code = -1;
    };

    static VideoEncoderConfig config(int width, int height, int bitrate, float fps) {
        VideoEncoderConfig c;
        c.vcodec = "H264";
        c.width = width;
        c.height = height;
        c.bitrate = bitrate;
        c.fps = fps;
        c.quality = 0;
        return c;
    }
    // What the camera ran before the rule (ruleProfile(false)), and what the user asks for meanwhile.
    static VideoEncoderConfig userOld() { return config(1920, 1080, 4096, 25); }
    static VideoEncoderConfig userNew() { return config(1600, 900, 2048, 20); }

    static OnvifMediaProfile profileOf(const VideoEncoderConfig &c) {
        OnvifMediaProfile profile;
        profile.token = kRuleToken;
        profile.vcodec = c.vcodec;
        profile.width = c.width;
        profile.height = c.height;
        profile.bitrate = c.bitrate;
        profile.fps = c.fps;
        return profile;
    }

    void seedBaseline(const VideoEncoderConfig &c) {
        auto controller = _controller;
        VideoEncoderConfig baseline = c;
        baseline.state.retry_time = 5;
        baseline.state.status = configStateToString[VideoConfigSetState::SUCCESS];
        onPoller([&]() { controller->addProfileConfig(kRuleToken, baseline, false); });
    }

    VideoEncoderConfig readBaseline() {
        VideoEncoderConfig out;
        auto controller = _controller;
        onPoller([&]() { CameraControllerTestPeer::profileConfig(*controller, kRuleToken, out); });
        return out;
    }

    SetOutcome setUser(const VideoEncoderConfig &c) {
        auto outcome = std::make_shared<SetOutcome>();
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto controller = _controller;
        onPoller([&]() {
            VideoEncoderConfig requested = c;
            controller->setMediaProfileAsync(kRuleToken, requested, [outcome, done](const SockException &ex) {
                outcome->error = (bool)ex;
                outcome->code = ex.getCustomCode();
                *done = true;
            });
        });
        EXPECT_TRUE(waitFor([&]() { return done->load(); }));
        return *outcome;
    }

    int rollback() {
        CbProbe probe;
        auto controller = _controller;
        auto cb = probe.cb();
        VideoEncoderConfig baseline = readBaseline();
        onPoller([&]() { controller->rollbackRuleVideoConfigAsync(kRuleToken, baseline, cb); });
        EXPECT_TRUE(waitFor([&]() { return !probe.get().empty(); }));
        auto codes = probe.get();
        return codes.empty() ? -1 : codes.front();
    }

    // Rule applied and confirmed on the camera, the user's old config as baseline.
    void runConfirmedRule(RuleResultRecorder &recorder) {
        seedBaseline(userOld());
        _fake->setProfiles({ruleProfile(true)}, "");
        ASSERT_EQ(ApiErrCode::CODE_SUCCESS, applyRule());
        syncOnce();
        ASSERT_EQ(1u, recorder.results().size());
    }
};

TEST_F(CameraControllerDeferredSetTest, SetDuringRuleOnlyUpdatesTheBaseline) {
    RuleResultRecorder recorder;
    runConfirmedRule(recorder);
    int sets_before = setCallCount(_fake);

    auto outcome = setUser(userNew());
    EXPECT_FALSE(outcome.error);
    EXPECT_EQ(ApiErrCode::CODE_ONVIF_SET_CONFIG_DEFERRED, outcome.code);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(sets_before, setCallCount(_fake)); // nothing sent to the camera

    auto baseline = readBaseline();
    EXPECT_TRUE(baseline == userNew());
    EXPECT_EQ(configStateToString[VideoConfigSetState::NEW], baseline.state.status);
    auto rule = readRule();
    EXPECT_TRUE(rule == ruleConfig());
    EXPECT_EQ(configStateToString[VideoConfigSetState::SUCCESS], rule.state.status);

    // The next re-read neither pushes the baseline nor reports the rule again.
    syncOnce();
    EXPECT_EQ(sets_before, setCallCount(_fake));
    EXPECT_EQ(1u, recorder.results().size());
}

TEST_F(CameraControllerDeferredSetTest, RollbackAfterDeferredSetPushesTheUserConfig) {
    RuleResultRecorder recorder;
    runConfirmedRule(recorder);
    ASSERT_EQ(ApiErrCode::CODE_ONVIF_SET_CONFIG_DEFERRED, setUser(userNew()).code);
    int sets_before = setCallCount(_fake);

    EXPECT_EQ(ApiErrCode::CODE_SUCCESS, rollback());
    ASSERT_EQ(sets_before + 1, setCallCount(_fake));
    EXPECT_TRUE(_fake->setConfigs().back() == userNew());

    // The camera now runs the user's config: the re-read confirms the baseline.
    _fake->setProfiles({profileOf(userNew())}, "");
    syncOnce();
    EXPECT_EQ(configStateToString[VideoConfigSetState::SUCCESS], readBaseline().state.status);
    EXPECT_EQ(sets_before + 1, setCallCount(_fake));
}

TEST_F(CameraControllerDeferredSetTest, SetEqualToBaselineDuringRuleIsNotChange) {
    RuleResultRecorder recorder;
    runConfirmedRule(recorder);

    auto outcome = setUser(userOld());
    EXPECT_TRUE(outcome.error);
    EXPECT_EQ(ApiErrCode::CODE_ONVIF_SET_CONFIG_NOT_CHANGE, outcome.code);
    EXPECT_EQ(configStateToString[VideoConfigSetState::SUCCESS], readBaseline().state.status);
}

TEST_F(CameraControllerDeferredSetTest, SetWithoutRuleStillPushesToTheCamera) {
    seedBaseline(userOld());
    _fake->setProfiles({profileOf(userOld())}, "");

    auto outcome = setUser(userNew());
    EXPECT_FALSE(outcome.error);
    EXPECT_NE(ApiErrCode::CODE_ONVIF_SET_CONFIG_DEFERRED, outcome.code);
    ASSERT_EQ(1, setCallCount(_fake));
    EXPECT_TRUE(_fake->setConfigs().back() == userNew());
}

TEST_F(CameraControllerDeferredSetTest, SeedDoesNotOverwriteAConcurrentUserSet) {
    auto controller = _controller;
    VideoEncoderConfig seed = userOld();
    VideoEncoderConfig used;

    // No baseline yet: the camera read becomes the baseline.
    onPoller([&]() { used = controller->seedBaselineIfAbsent(kRuleToken, seed); });
    EXPECT_TRUE(used == userOld());
    EXPECT_TRUE(readBaseline() == userOld());

    // A user set landed first: the (older) camera read does not replace it.
    seedBaseline(userNew());
    onPoller([&]() { used = controller->seedBaselineIfAbsent(kRuleToken, seed); });
    EXPECT_TRUE(used == userNew());
    EXPECT_TRUE(readBaseline() == userNew());
}

// ── Control protocol selection: one controller, one protocol per camera ─────────────────────────

namespace {

// What the fake devices of one test did, in order: "create X", "connect X", "destroy X", "ready 0 <status>".
class ProtocolWorld {
public:
    void log(const std::string &event) {
        std::lock_guard<std::mutex> lck(_mtx);
        _events.push_back(event);
    }
    std::vector<std::string> events() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _events;
    }
    void clear() {
        std::lock_guard<std::mutex> lck(_mtx);
        _events.clear();
    }
    int count(const std::string &event) {
        auto all = events();
        return static_cast<int>(std::count(all.begin(), all.end(), event));
    }
    void setResult(ControlProtocol protocol, ConnectError err) {
        _results[static_cast<int>(protocol)] = static_cast<int>(err);
    }
    ConnectError result(ControlProtocol protocol) const {
        return static_cast<ConnectError>(_results[static_cast<int>(protocol)].load());
    }
    // lastFailure() of every fake of `protocol` (DeviceFailure::None unless a test sets it).
    void setFailure(ControlProtocol protocol, DeviceFailure failure) {
        _failures[static_cast<int>(protocol)] = static_cast<int>(failure);
    }
    DeviceFailure failure(ControlProtocol protocol) const {
        return static_cast<DeviceFailure>(_failures[static_cast<int>(protocol)].load());
    }

    std::atomic<int> live{0};
    // takeClockSyncWanted() of the fakes: a command found the device clock off.
    std::atomic<bool> sync_wanted{false};
    // probeConnection() of a connected fake succeeds; off, every cycle runs a full connect().
    std::atomic<bool> probe_ok{false};

private:
    std::mutex _mtx;
    std::vector<std::string> _events;
    // Indexed by ControlProtocol. Not a Hikvision camera unless a test says so: ISAPI is not spoken.
    std::atomic<int> _results[4] = {{static_cast<int>(ConnectError::NONE)},
                                    {static_cast<int>(ConnectError::NONE)},
                                    {static_cast<int>(ConnectError::NONE)},
                                    {static_cast<int>(ConnectError::NOT_THIS_PROTOCOL)}};
    std::atomic<int> _failures[4] = {{0}, {0}, {0}, {0}};
};

class FakeProtocolControl : public DeviceControl {
public:
    FakeProtocolControl(ControlProtocol protocol, std::shared_ptr<ProtocolWorld> world)
        : _protocol(protocol), _world(std::move(world)) {
        ++_world->live;
        _world->log(std::string("create ") + controlProtocolToString(_protocol));
    }
    ~FakeProtocolControl() override {
        --_world->live;
        _world->log(std::string("destroy ") + controlProtocolToString(_protocol));
    }

    bool connect() override {
        _world->log(std::string("connect ") + controlProtocolToString(_protocol) + " live=" + std::to_string(_world->live.load()));
        _err = static_cast<int>(_world->result(_protocol));
        _connected = _err == static_cast<int>(ConnectError::NONE);
        return _connected;
    }
    void disconnect() override {}
    ControlProtocol protocol() const override { return _protocol; }
    ConnectError lastConnectError() const override { return static_cast<ConnectError>(_err.load()); }
    std::string lastError() override { return connectErrorToString(lastConnectError()); }
    bool probeConnection() override {
        _world->log(std::string("probe ") + controlProtocolToString(_protocol));
        return _connected && _world->probe_ok;
    }
    bool isConnected() const override { return _connected; }
    bool isControlled() const override { return false; }
    DeviceFailure lastFailure() const override { return _world->failure(_protocol); }
    bool takeClockSyncWanted() override { return _world->sync_wanted.exchange(false); }
    void setCameraTimeManual() override { _world->log(std::string("settime ") + controlProtocolToString(_protocol)); }
    OnvifDeviceInfo getDeviceInfo() override { return OnvifDeviceInfo(); }
    OnvifMediaProfileMap getMediaProfilesInfo() override { return OnvifMediaProfileMap(); }
    bool refreshMediaProfiles() override { return true; }
    bool getVideoEncoderConfigByToken(const std::string &, VideoEncoderConfig &) override { return false; }
    bool setVideoEncoderConfigByToken(const std::string &, const VideoEncoderConfig &) override { return false; }
    OnvifPTZProfile getPTZProfile() override {
        OnvifPTZProfile profile;
        profile.isConsMoveEnable = true;
        return profile;
    }
    bool enablePTZ() override { return true; }
    bool PTZ_ContinuousMove(float, float, float, int) override {
        _world->log(std::string("ptz ") + controlProtocolToString(_protocol));
        return true;
    }
    bool PTZ_RelativeMove(float, float, float, float, float, float) override { return true; }
    bool PTZ_AbsoluteMove(float, float, float) override { return true; }
    bool PTZ_AbsoluteMove(float, float, float, float, float, float) override { return true; }
    bool PTZ_Stop(bool, bool) override { return true; }
    bool PTZ_GotoPreset(const std::string &, float, float, float) override { return true; }
    bool PTZ_GotoHomePosition(float, float, float) override { return true; }
    bool PTZ_AuxiliaryCommand(const std::string &) override { return true; }
    PtzMoveStatus PTZ_GetStatus(float &, float &, float &) override { return PtzMoveStatus::IDLE; }
    OnvifImageProfile getImageProfile() const override { return OnvifImageProfile(); }
    bool enableFocus() override { return false; }
    bool enableIris() override { return false; }
    bool Imaging_FocusMove(float) override { return false; }
    bool Imaging_FocusStop() override { return false; }
    std::vector<OnvifRelayOutputProfile> getRelayOutputProfiles() const override { return {}; }
    bool enableRelayOutput() override { return false; }
    bool Relay_SetOutputState(const std::string &, bool) override { return false; }

private:
    ControlProtocol _protocol;
    std::shared_ptr<ProtocolWorld> _world;
    std::atomic<int> _err{static_cast<int>(ConnectError::NONE)};
    std::atomic<bool> _connected{false};
};

class ProtocolTestController : public CameraController {
public:
    ProtocolTestController(const DeviceTuple &tuple, const EventPoller::Ptr &poller, std::shared_ptr<ProtocolWorld> world)
        : CameraController(tuple, poller), _world(std::move(world)) {}

protected:
    DeviceControl::Ptr createDeviceControl(ControlProtocol protocol, const std::string &, const std::string &,
                                                 const std::string &) override {
        return std::make_shared<FakeProtocolControl>(protocol, _world);
    }

private:
    std::shared_ptr<ProtocolWorld> _world;
};

class ReadyRecorder : public DeviceSourceEvent {
public:
    ReadyRecorder(std::shared_ptr<ProtocolWorld> world, EventPoller::Ptr poller)
        : _world(std::move(world)), _poller(std::move(poller)) {}
    void onControllerReady(DeviceSource &, bool connect, const std::string &status, const Any &) override {
        _world->log(std::string("ready ") + (connect ? "1 " : "0 ") + status);
    }
    EventPoller::Ptr getOwnerPoller(DeviceSource &) override { return _poller; }

private:
    std::shared_ptr<ProtocolWorld> _world;
    EventPoller::Ptr _poller;
};

bool contains(const std::vector<std::string> &events, const std::string &prefix) {
    for (const auto &e : events) {
        if (e.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

// Index of the first event starting with `prefix`, -1 if none.
int indexOf(const std::vector<std::string> &events, const std::string &prefix) {
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].compare(0, prefix.size(), prefix) == 0) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

class CameraProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        _poller = EventPollerPool::Instance().getPoller();
        _world = std::make_shared<ProtocolWorld>();
        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = "cam-controller-test";
        _controller = std::make_shared<ProtocolTestController>(tuple, _poller, _world);
        _listener = std::make_shared<ReadyRecorder>(_world, _poller);
        _controller->setListener(_listener);
        _controller->createTimer();
    }

    void TearDown() override {
        auto controller = _controller;
        onPoller([controller]() { controller->stopController(); });
    }

    void onPoller(const std::function<void()> &task) { _poller->sync(task); }

    void setup(const std::string &protocol, const std::string &last = "") {
        CameraOption option;
        option.manufacturer = "ONVIF";
        option.ip = "127.0.0.1";
        option.port = 1;
        option.controlProtocol = protocol;
        auto controller = _controller;
        onPoller([controller, option, last]() {
            if (!last.empty()) controller->setLastControlProtocol(last);
            controller->setupController(option);
        });
    }

    // One maintenance cycle, waited for until the fake devices stop reporting.
    void cycle() {
        auto controller = _controller;
        onPoller([controller]() {
            CameraControllerTestPeer::allowReconnect(*controller);
            CameraControllerTestPeer::onManager(*controller);
        });
        settle();
    }

    void settle() {
        size_t seen = _world->events().size();
        for (int quiet = 0, i = 0; quiet < 4 && i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            onPoller([]() {});
            size_t now = _world->events().size();
            quiet = now == seen ? quiet + 1 : 0;
            seen = now;
        }
    }

    ControlProtocol current() {
        ControlProtocol protocol = ControlProtocol::NONE;
        auto controller = _controller;
        onPoller([&]() { protocol = CameraControllerTestPeer::deviceProtocol(*controller); });
        return protocol;
    }

    EventPoller::Ptr _poller;
    std::shared_ptr<ProtocolWorld> _world;
    std::shared_ptr<ProtocolTestController> _controller;
    std::shared_ptr<ReadyRecorder> _listener;
};

TEST_F(CameraProtocolTest, FixedOnvifNeverTriesPsia) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NOT_THIS_PROTOCOL);
    setup("ONVIF");
    for (int i = 0; i < 7; ++i) cycle();
    EXPECT_EQ(7, _world->count("connect ONVIF live=1"));
    EXPECT_FALSE(contains(_world->events(), "create PSIA"));
    EXPECT_EQ(ControlProtocol::ONVIF, current());
}

TEST_F(CameraProtocolTest, FixedPsiaNeverTriesOnvif) {
    _world->setResult(ControlProtocol::PSIA, ConnectError::NOT_THIS_PROTOCOL);
    setup("PSIA");
    for (int i = 0; i < 7; ++i) cycle();
    EXPECT_EQ(7, _world->count("connect PSIA live=1"));
    EXPECT_FALSE(contains(_world->events(), "create ONVIF"));
}

TEST_F(CameraProtocolTest, AutoFallsBackToPsiaOnlyAfterOnvifSaysNotThisProtocol) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NOT_THIS_PROTOCOL);
    setup("AUTO");
    EXPECT_EQ(ControlProtocol::NONE, current()) << "no controller before the probe";
    cycle();

    auto events = _world->events();
    // The ONVIF controller is gone before the PSIA one even exists.
    EXPECT_LT(indexOf(events, "destroy ONVIF"), indexOf(events, "create PSIA"));
    EXPECT_EQ(1, _world->count("connect PSIA live=1"));
    EXPECT_TRUE(contains(events, "ready 1 connected"));
    EXPECT_EQ(ControlProtocol::PSIA, current());
    EXPECT_TRUE(_controller->isReady());

    // Locked: later cycles reconnect PSIA only.
    _world->clear();
    cycle();
    EXPECT_FALSE(contains(_world->events(), "create"));
    EXPECT_EQ(1, _world->count("connect PSIA live=1"));
}

TEST_F(CameraProtocolTest, AutoDoesNotTryOtherProtocolsWhenOnvifIsUnreachable) {
    const ConnectError errors[] = {ConnectError::UNREACHABLE, ConnectError::OTHER};
    setup("AUTO");
    for (auto err : errors) {
        _world->setResult(ControlProtocol::ONVIF, err);
        cycle();
    }
    auto events = _world->events();
    EXPECT_EQ(2, _world->count("create ONVIF"));
    EXPECT_FALSE(contains(events, "create ISAPI"));
    EXPECT_FALSE(contains(events, "create PSIA"));
    EXPECT_EQ(0, _world->live.load());
    EXPECT_EQ(ControlProtocol::NONE, current());
    EXPECT_FALSE(_controller->isReady());
}

// Hikvision with ONVIF disabled, or without an ONVIF user, answers 401 to ONVIF only.
TEST_F(CameraProtocolTest, AutoTriesIsapiAfterOnvifRejectsThePassword) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    setup("AUTO");
    cycle();
    auto events = _world->events();
    EXPECT_LT(indexOf(events, "destroy ONVIF"), indexOf(events, "create ISAPI"));
    EXPECT_EQ(1, _world->count("connect ISAPI live=1"));
    EXPECT_FALSE(contains(events, "create PSIA"));
    EXPECT_EQ(ControlProtocol::ISAPI, current());
    EXPECT_TRUE(_controller->isReady());
}

TEST_F(CameraProtocolTest, AutoStopsWhenIsapiRejectsThePasswordAndReportsTheOnvifError) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setResult(ControlProtocol::ISAPI, ConnectError::AUTH_FAILED);
    setup("AUTO");
    cycle();
    auto events = _world->events();
    EXPECT_EQ(1, _world->count("create ISAPI"));
    EXPECT_FALSE(contains(events, "create PSIA")) << "a rejected password says nothing about the protocol";
    EXPECT_TRUE(contains(events, "ready 0 ONVIF: AUTH_FAILED"));
    EXPECT_EQ(ControlProtocol::NONE, current());
}

TEST_F(CameraProtocolTest, LockedOnvifStaysLockedWhenItRejectsThePassword) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    setup("AUTO", "ONVIF");
    for (int i = 0; i < 6; ++i) cycle();
    EXPECT_FALSE(contains(_world->events(), "create ISAPI"));
    EXPECT_EQ(ControlProtocol::ONVIF, current());
}

// Waits (on the test thread) for the answer of a reconnectNow() request.
struct ReconnectWait {
    std::mutex mtx;
    std::condition_variable cv;
    int answers = 0;
    ReconnectResult result;

    ReconnectCallback callback() {
        return [this](const ReconnectResult &r) {
            std::lock_guard<std::mutex> lck(mtx);
            result = r;
            ++answers;
            cv.notify_all();
        };
    }
    bool wait() {
        std::unique_lock<std::mutex> lck(mtx);
        return cv.wait_for(lck, std::chrono::seconds(5), [this]() { return answers > 0; });
    }
};

TEST_F(CameraProtocolTest, RejectedPasswordSlowsDownTheCycleUntilItConnects) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    setup("ONVIF");
    cycle();
    auto controller = _controller;
    bool rejected = false;
    onPoller([&]() { rejected = CameraControllerTestPeer::authRejected(*controller); });
    EXPECT_TRUE(rejected);

    // The 10 s timer tick right after does not log in again (60 s or 300 s, see ReconnectPacerTest).
    onPoller([controller]() { CameraControllerTestPeer::onManager(*controller); });
    settle();
    EXPECT_EQ(1, _world->count("connect ONVIF live=1"));

    _world->setResult(ControlProtocol::ONVIF, ConnectError::NONE);
    cycle();
    onPoller([&]() { rejected = CameraControllerTestPeer::authRejected(*controller); });
    EXPECT_FALSE(rejected);
    EXPECT_TRUE(_controller->isReady());
}

TEST_F(CameraProtocolTest, ReconnectNowLogsInAtOnceAndReportsTheResult) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    setup("ONVIF");
    cycle();
    EXPECT_EQ(1, _world->count("connect ONVIF live=1"));

    // Still the wrong password: the request answers with the rejection.
    auto controller = _controller;
    ReconnectWait first;
    onPoller([&]() { controller->reconnectNow(first.callback()); });
    ASSERT_TRUE(first.wait());
    EXPECT_TRUE(first.result.finished);
    EXPECT_FALSE(first.result.connected);
    EXPECT_EQ(ConnectError::AUTH_FAILED, first.result.error);
    EXPECT_EQ("ONVIF", first.result.protocol);
    EXPECT_EQ(2, _world->count("connect ONVIF live=1")) << "no wait for the auth retry interval";

    // The password was fixed on the camera: the next request connects without waiting.
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NONE);
    ReconnectWait second;
    onPoller([&]() { controller->reconnectNow(second.callback()); });
    ASSERT_TRUE(second.wait());
    EXPECT_TRUE(second.result.connected);
    EXPECT_EQ(ConnectError::NONE, second.result.error);
    settle();
    EXPECT_TRUE(_controller->isReady());
    bool rejected = true;
    onPoller([&]() { rejected = CameraControllerTestPeer::authRejected(*controller); });
    EXPECT_FALSE(rejected);

    // Already connected: one probe request is enough.
    _world->probe_ok = true;
    _world->clear();
    ReconnectWait third;
    onPoller([&]() { controller->reconnectNow(third.callback()); });
    ASSERT_TRUE(third.wait());
    EXPECT_TRUE(third.result.connected);
    EXPECT_EQ(1, _world->count("probe ONVIF"));
    EXPECT_FALSE(contains(_world->events(), "connect"));
}

TEST_F(CameraProtocolTest, ReconnectNowProbesAnAutoCameraWithoutProtocol) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setResult(ControlProtocol::ISAPI, ConnectError::AUTH_FAILED);
    setup("AUTO");
    cycle();
    auto controller = _controller;
    bool rejected = false;
    onPoller([&]() { rejected = CameraControllerTestPeer::authRejected(*controller); });
    EXPECT_TRUE(rejected) << "a probe that ends on a rejected password slows down too";

    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    ReconnectWait wait;
    onPoller([&]() { controller->reconnectNow(wait.callback()); });
    ASSERT_TRUE(wait.wait());
    EXPECT_TRUE(wait.result.connected);
    EXPECT_EQ("ISAPI", wait.result.protocol);
    settle();
    EXPECT_EQ(ControlProtocol::ISAPI, current());
    onPoller([&]() { rejected = CameraControllerTestPeer::authRejected(*controller); });
    EXPECT_FALSE(rejected);
}

TEST_F(CameraProtocolTest, ReconnectNowWithoutControllerAnswersAtOnce) {
    auto controller = _controller;
    ReconnectWait wait;
    onPoller([&]() { controller->reconnectNow(wait.callback()); });
    ASSERT_TRUE(wait.wait());
    EXPECT_TRUE(wait.result.finished);
    EXPECT_FALSE(wait.result.connected);
    EXPECT_FALSE(contains(_world->events(), "create"));
}

TEST_F(CameraProtocolTest, AutoKeepsTheSavedProtocolWithoutProbing) {
    setup("AUTO", "PSIA");
    EXPECT_EQ(ControlProtocol::PSIA, current());
    cycle();
    auto events = _world->events();
    EXPECT_FALSE(contains(events, "create ONVIF"));
    EXPECT_EQ(1, _world->count("connect PSIA live=1"));
}

TEST_F(CameraProtocolTest, LockedAutoProbesAgainAfterRepeatedNotThisProtocol) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NOT_THIS_PROTOCOL);
    setup("AUTO", "ONVIF");
    cycle();
    cycle();
    // Any other failure resets the count: an unreachable camera says nothing about the protocol.
    _world->setResult(ControlProtocol::ONVIF, ConnectError::UNREACHABLE);
    cycle();
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NOT_THIS_PROTOCOL);
    for (int i = 0; i < 4; ++i) cycle();
    EXPECT_EQ(ControlProtocol::ONVIF, current());
    EXPECT_FALSE(contains(_world->events(), "create PSIA"));

    cycle(); // fifth in a row: unlock
    EXPECT_EQ(ControlProtocol::NONE, current());
    EXPECT_TRUE(contains(_world->events(), "ready 0 Control protocol changed"));
    cycle(); // probe
    EXPECT_EQ(ControlProtocol::PSIA, current());
    EXPECT_EQ(1, _world->count("connect PSIA live=1"));
}

TEST_F(CameraProtocolTest, SwitchingProtocolUnpublishesTheOldControllerAndFailsPendingRules) {
    RuleResultRecorder recorder;
    setup("ONVIF");
    cycle();
    ASSERT_EQ(ControlProtocol::ONVIF, current());
    ASSERT_TRUE(_controller->isReady());

    CbProbe probe;
    auto controller = _controller;
    auto cb = probe.cb();
    onPoller([&]() { controller->applyRuleVideoConfigAsync("log-switch", "onvif-token", ruleConfig(), cb); });
    ASSERT_TRUE(waitFor([&]() { return !probe.get().empty(); }));
    settle();

    _world->clear();
    setup("PSIA");
    auto events = _world->events();
    ASSERT_GE(indexOf(events, "ready 0 Control protocol changed"), 0);
    EXPECT_LT(indexOf(events, "ready 0 Control protocol changed"), indexOf(events, "destroy ONVIF"));
    EXPECT_LT(indexOf(events, "destroy ONVIF"), indexOf(events, "create PSIA"));
    EXPECT_FALSE(_controller->isReady());

    cycle();
    EXPECT_EQ(1, _world->count("connect PSIA live=1"));
    auto results = recorder.results();
    ASSERT_EQ(1u, results.size());
    EXPECT_EQ("log-switch", results[0].log_id);
    EXPECT_FALSE(results[0].success);
    EXPECT_EQ("DEVICE_PROTOCOL_CHANGED", results[0].err_code);
}

TEST_F(CameraProtocolTest, CommandsDuringTheProbeReportTheDeviceOffline) {
    setup("AUTO");
    CbProbe probe;
    auto controller = _controller;
    auto cb = probe.cb();
    onPoller([&]() { controller->PTZMove("left", 50, cb); });
    ASSERT_TRUE(waitFor([&]() { return !probe.get().empty(); }));
    EXPECT_EQ(std::vector<int>{ApiErrCode::CODE_DEVICE_OFFLINE}, probe.get());
    EXPECT_FALSE(contains(_world->events(), "ptz"));
}

TEST(ControlProtocolSettingTest, UnknownValuesMeanAuto) {
    EXPECT_EQ("ONVIF", controlProtocolSetting("onvif"));
    EXPECT_EQ("PSIA", controlProtocolSetting("PSIA"));
    EXPECT_EQ("ISAPI", controlProtocolSetting("isapi"));
    EXPECT_EQ("AUTO", controlProtocolSetting(""));
    EXPECT_EQ("AUTO", controlProtocolSetting("HIKVISION"));
    EXPECT_EQ("AUTO", controlProtocolSetting("AUTO"));
}

// ── ProtocolLock / parseProtocolOrder: the selection rules camera and speaker share ─────────────

TEST(ProtocolLockTest, FixedSettingIsUsedAndNeverUnlocks) {
    ProtocolLock lock;
    lock.setSetting("isapi");
    EXPECT_FALSE(lock.isAuto());
    EXPECT_EQ(ControlProtocol::ISAPI, lock.fixedOrLocked());
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 5));
    }
    EXPECT_EQ(ControlProtocol::ISAPI, lock.fixedOrLocked());
}

TEST(ProtocolLockTest, AutoProbesUntilConnectedThenStaysLocked) {
    ProtocolLock lock;
    lock.setSetting("AUTO");
    EXPECT_TRUE(lock.isAuto());
    EXPECT_EQ(ControlProtocol::NONE, lock.fixedOrLocked());
    EXPECT_FALSE(lock.onConnected(ControlProtocol::PSIA)) << "first connect is not a change";
    EXPECT_EQ(ControlProtocol::PSIA, lock.fixedOrLocked());
    EXPECT_FALSE(lock.onConnected(ControlProtocol::PSIA));
    EXPECT_TRUE(lock.onConnected(ControlProtocol::ISAPI));
    EXPECT_EQ(ControlProtocol::ISAPI, lock.last());
}

TEST(ProtocolLockTest, RestoredProtocolLocksAutoAtOnce) {
    ProtocolLock lock;
    lock.restore(ControlProtocol::ONVIF);
    lock.setSetting("");
    EXPECT_EQ(ControlProtocol::ONVIF, lock.fixedOrLocked());
    lock.restore(ControlProtocol::NONE);
    EXPECT_EQ(ControlProtocol::NONE, lock.fixedOrLocked());
}

TEST(ProtocolLockTest, OnlyConsecutiveNotThisProtocolUnlocks) {
    ProtocolLock lock;
    lock.restore(ControlProtocol::ONVIF);
    EXPECT_FALSE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 3));
    EXPECT_FALSE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 3));
    EXPECT_FALSE(lock.onConnectError(ConnectError::UNREACHABLE, 3)) << "resets the count";
    EXPECT_FALSE(lock.onConnectError(ConnectError::AUTH_FAILED, 3));
    EXPECT_FALSE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 3));
    EXPECT_FALSE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 3));
    EXPECT_TRUE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 3));
    lock.unlock();
    EXPECT_EQ(ControlProtocol::NONE, lock.fixedOrLocked());
    EXPECT_EQ(ControlProtocol::ONVIF, lock.last()) << "kept to detect the change on the next connect";
    // A successful connect resets the count too; 0 or less means "at the first one".
    lock.onConnected(ControlProtocol::ONVIF);
    EXPECT_TRUE(lock.onConnectError(ConnectError::NOT_THIS_PROTOCOL, 0));
}

TEST(ReconnectPacerTest, RejectedCredentialsWaitTheLongerInterval) {
    ReconnectPacer pacer;
    EXPECT_TRUE(pacer.due(1000, 60, 300)) << "first cycle runs at once";
    pacer.onAttempt(1000);
    EXPECT_FALSE(pacer.due(1059, 60, 300));
    EXPECT_TRUE(pacer.due(1060, 60, 300));

    pacer.onAttempt(1060);
    pacer.onResult(ConnectError::AUTH_FAILED);
    EXPECT_TRUE(pacer.authFailed());
    EXPECT_FALSE(pacer.due(1120, 60, 300));
    EXPECT_FALSE(pacer.due(1359, 60, 300));
    EXPECT_TRUE(pacer.due(1360, 60, 300));
    // An auth retry shorter than the normal interval (or 0) does not speed the cycle up.
    EXPECT_FALSE(pacer.due(1100, 60, 0));
    EXPECT_TRUE(pacer.due(1120, 60, 0));

    // Other failures and successes go back to the normal interval.
    pacer.onResult(ConnectError::UNREACHABLE);
    EXPECT_TRUE(pacer.due(1120, 60, 300));
    pacer.onResult(ConnectError::AUTH_FAILED);

    // skipWait keeps the verdict, reset drops it; both make the next cycle due at once.
    pacer.skipWait();
    EXPECT_TRUE(pacer.due(1061, 60, 300));
    EXPECT_TRUE(pacer.authFailed());
    pacer.onAttempt(1061);
    pacer.reset();
    EXPECT_TRUE(pacer.due(1062, 60, 300));
    EXPECT_FALSE(pacer.authFailed());
    // A clock that went backwards does not stall the cycle.
    pacer.onAttempt(5000);
    EXPECT_TRUE(pacer.due(4000, 60, 300));
}

TEST(ProtocolOrderTest, UnknownAndRepeatedNamesAreSkipped) {
    typedef std::vector<ControlProtocol> Order;
    EXPECT_EQ((Order{ControlProtocol::ISAPI, ControlProtocol::ONVIF}), parseProtocolOrder(" isapi , foo,ONVIF,isapi"));
    const Order fallback{ControlProtocol::ONVIF, ControlProtocol::ISAPI, ControlProtocol::PSIA};
    EXPECT_EQ(fallback, parseProtocolOrder(""));
    EXPECT_EQ(fallback, parseProtocolOrder("auto,none"));
}

// ── SpeakerController: same protocol selection as the camera ────────────────────────────────────

namespace managerkit {
class SpeakerControllerTestPeer {
public:
    static void cycle(SpeakerController &c) {
        c._pacer.skipWait();
        c.onManager();
    }
    static ControlProtocol deviceProtocol(SpeakerController &c) {
        return c._device_ctr ? c._device_ctr->protocol() : ControlProtocol::NONE;
    }
    static bool authRejected(SpeakerController &c) { return c._pacer.authFailed(); }
    static int notReadyCode(SpeakerController &c) { return c.notReadyError().getCustomCode(); }
};
} // namespace managerkit

namespace {
class ProtocolTestSpeaker : public SpeakerController {
public:
    ProtocolTestSpeaker(const DeviceTuple &tuple, const EventPoller::Ptr &poller, std::shared_ptr<ProtocolWorld> world)
        : SpeakerController(tuple, poller), _world(std::move(world)) {}

protected:
    DeviceControl::Ptr createDeviceControl(ControlProtocol protocol, const std::string &, const std::string &,
                                                 const std::string &) override {
        return std::make_shared<FakeProtocolControl>(protocol, _world);
    }

private:
    std::shared_ptr<ProtocolWorld> _world;
};
} // namespace

class SpeakerProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {
        _poller = EventPollerPool::Instance().getPoller();
        _world = std::make_shared<ProtocolWorld>();
        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = "speaker-controller-test";
        _speaker = std::make_shared<ProtocolTestSpeaker>(tuple, _poller, _world);
        _listener = std::make_shared<ReadyRecorder>(_world, _poller);
        _speaker->setListener(_listener);
    }

    void TearDown() override {
        auto speaker = _speaker;
        _poller->sync([speaker]() { speaker->stopController(); });
    }

    void setup(const std::string &protocol, const std::string &last = "", const std::string &manufacturer = "ONVIF",
               bool time_sync = true) {
        SpeakerOption option;
        option.manufacturer = manufacturer;
        option.enableTimeSync = time_sync;
        option.ip = "127.0.0.1";
        option.port = 1;
        option.controlProtocol = protocol;
        auto speaker = _speaker;
        _poller->sync([speaker, option, last]() {
            if (!last.empty()) speaker->setLastControlProtocol(last);
            speaker->setupController(option);
        });
    }

    void cycle() {
        auto speaker = _speaker;
        _poller->sync([speaker]() { SpeakerControllerTestPeer::cycle(*speaker); });
        size_t seen = _world->events().size();
        for (int quiet = 0, i = 0; quiet < 4 && i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            _poller->sync([]() {});
            size_t now = _world->events().size();
            quiet = now == seen ? quiet + 1 : 0;
            seen = now;
        }
    }

    ControlProtocol current() {
        ControlProtocol protocol = ControlProtocol::NONE;
        auto speaker = _speaker;
        _poller->sync([&]() { protocol = SpeakerControllerTestPeer::deviceProtocol(*speaker); });
        return protocol;
    }

    EventPoller::Ptr _poller;
    std::shared_ptr<ProtocolWorld> _world;
    std::shared_ptr<ProtocolTestSpeaker> _speaker;
    std::shared_ptr<ReadyRecorder> _listener;
};

TEST_F(SpeakerProtocolTest, AutoTriesOnvifFirstAndLocksTheFirstThatAnswers) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NOT_THIS_PROTOCOL);
    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    setup("");
    EXPECT_EQ(ControlProtocol::NONE, current()) << "no controller before the probe";
    cycle();
    auto events = _world->events();
    EXPECT_LT(indexOf(events, "create ONVIF"), indexOf(events, "create ISAPI"));
    EXPECT_LT(indexOf(events, "destroy ONVIF"), indexOf(events, "create ISAPI"));
    EXPECT_TRUE(contains(events, "ready 1 connected"));
    EXPECT_EQ(ControlProtocol::ISAPI, current());
    EXPECT_TRUE(_speaker->isReady());

    _world->clear();
    cycle();
    EXPECT_FALSE(contains(_world->events(), "create"));
    EXPECT_EQ(1, _world->count("connect ISAPI live=1"));
}

TEST_F(SpeakerProtocolTest, OnvifSpeakerStillConnectsOverOnvif) {
    setup("AUTO");
    cycle();
    EXPECT_EQ(1, _world->count("connect ONVIF live=1"));
    EXPECT_FALSE(contains(_world->events(), "create ISAPI"));
    EXPECT_EQ(ControlProtocol::ONVIF, current());
}

TEST_F(SpeakerProtocolTest, ReadySpeakerIsProbedInsteadOfReconnected) {
    _world->probe_ok = true;
    setup("ONVIF");
    for (int i = 0; i < 3; ++i) cycle();
    EXPECT_EQ(1, _world->count("connect ONVIF live=1"));
    EXPECT_EQ(2, _world->count("probe ONVIF"));

    _world->probe_ok = false; // answers no more: full connect again
    cycle();
    EXPECT_EQ(2, _world->count("connect ONVIF live=1"));
}

TEST_F(SpeakerProtocolTest, FixedProtocolAndSavedLockSkipTheProbe) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::NOT_THIS_PROTOCOL);
    _world->setResult(ControlProtocol::PSIA, ConnectError::NOT_THIS_PROTOCOL);
    setup("PSIA");
    EXPECT_EQ(ControlProtocol::PSIA, current());
    for (int i = 0; i < 6; ++i) cycle();
    EXPECT_FALSE(contains(_world->events(), "create ONVIF"));

    _world->clear();
    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    setup("AUTO", "ISAPI");
    EXPECT_EQ(ControlProtocol::NONE, current()) << "a reconfigured speaker probes again";
}

TEST_F(SpeakerProtocolTest, SavedProtocolLocksAutoAtStartup) {
    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    setup("AUTO", "ISAPI");
    EXPECT_EQ(ControlProtocol::ISAPI, current());
    cycle();
    EXPECT_FALSE(contains(_world->events(), "create ONVIF"));
    EXPECT_EQ(1, _world->count("connect ISAPI live=1"));
}

// The Bosch plugin provisions an ONVIF account and needs ONVIF for its backchannel.
TEST_F(SpeakerProtocolTest, RejectedPasswordSlowsDownAndReconnectNowLogsInAtOnce) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    setup("ONVIF");
    cycle();
    auto speaker = _speaker;
    bool rejected = false;
    _poller->sync([&]() { rejected = SpeakerControllerTestPeer::authRejected(*speaker); });
    EXPECT_TRUE(rejected);

    _world->setResult(ControlProtocol::ONVIF, ConnectError::NONE);
    ReconnectWait wait;
    _poller->sync([&]() { speaker->reconnectNow(wait.callback()); });
    ASSERT_TRUE(wait.wait());
    EXPECT_TRUE(wait.result.finished);
    EXPECT_TRUE(wait.result.connected);
    EXPECT_EQ("ONVIF", wait.result.protocol);
    EXPECT_EQ(2, _world->count("connect ONVIF live=1"));
    _poller->sync([&]() { rejected = SpeakerControllerTestPeer::authRejected(*speaker); });
    EXPECT_FALSE(rejected);
}

TEST_F(SpeakerProtocolTest, BoschSpeakerIsAlwaysOnvif) {
    setup("ISAPI", "", "Bosch");
    EXPECT_EQ(ControlProtocol::ONVIF, current());
    cycle();
    EXPECT_FALSE(contains(_world->events(), "create ISAPI"));
    EXPECT_EQ(1, _world->count("connect ONVIF live=1"));
}

// A command sent while the speaker is not ready says why the last attempt failed.
TEST_F(SpeakerProtocolTest, NotReadyErrorSaysWhyTheProbeFailed) {
    auto speaker = _speaker;
    int code = 0;
    setup("");
    _poller->sync([&]() { code = SpeakerControllerTestPeer::notReadyCode(*speaker); });
    EXPECT_EQ(ApiErrCode::CODE_DEVICE_OFFLINE, code) << "nothing tried yet";

    // ONVIF rejects the account, ISAPI and PSIA are not spoken: the ONVIF verdict is reported.
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::Unauthorized);
    _world->setResult(ControlProtocol::PSIA, ConnectError::NOT_THIS_PROTOCOL);
    cycle();
    EXPECT_FALSE(_speaker->isReady());
    EXPECT_EQ(ControlProtocol::NONE, current());
    _poller->sync([&]() { code = SpeakerControllerTestPeer::notReadyCode(*speaker); });
    EXPECT_EQ(ApiErrCode::CODE_DEVICE_UNAUTHORIZED, code);
}

TEST_F(SpeakerProtocolTest, NotReadyErrorSaysWhyTheLockedProtocolFailed) {
    auto speaker = _speaker;
    int code = 0;
    setup("ONVIF");
    _world->setResult(ControlProtocol::ONVIF, ConnectError::UNREACHABLE);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::Unreachable);
    cycle();
    EXPECT_FALSE(_speaker->isReady());
    _poller->sync([&]() { code = SpeakerControllerTestPeer::notReadyCode(*speaker); });
    EXPECT_EQ(ApiErrCode::CODE_DEVICE_UNREACHABLE, code);
}

// ── Camera clock: only ever set by the maintenance cycle ──────────────────────

namespace {
void setClockSyncEnable(bool on) {
    mINI::Instance()[OnvifCtrl::kClockSyncEnable] = on ? 1 : 0;
    NOTICE_EMIT(BroadcastReloadConfigArgs, mediakit::Broadcast::kBroadcastReloadConfig);
}

int countCalls(const std::shared_ptr<FakeOnvifControl> &fake, const std::string &name) {
    auto names = callNames(fake);
    return static_cast<int>(std::count(names.begin(), names.end(), name));
}
} // namespace

TEST_F(CameraControllerTest, ConnectRejectedForTheClockSetsItFromTheCycle) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(false, false);
    fake->setLastFailure(DeviceFailure::ClockSkew);

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });

    // The camera never connects until its clock is set, so the failed cycle sets it.
    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "SetCameraTime"); }));
    EXPECT_EQ(std::vector<std::string>({"Connect", "SetCameraTime"}), callNames(fake));
}

TEST_F(CameraControllerTest, ConnectRejectedForAnotherReasonDoesNotTouchTheClock) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(false, false);
    fake->setLastFailure(DeviceFailure::Unauthorized);

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });

    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "Connect"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(0, countCalls(fake, "SetCameraTime"));
}

TEST_F(CameraControllerTest, ClockOffFoundByACommandIsSetByTheNextCycleNotTheCommand) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(true, false);
    onPoller([&]() {
        CameraControllerTestPeer::setReady(*controller, true);
        CameraControllerTestPeer::allowNextCycle(*controller, true); // regular clock check not due
    });

    // A PTZ command found the clock off: it only flags it.
    fake->setClockSyncWanted(true);
    CbProbe ptz;
    onPoller([&]() { controller->PTZMove("left", 50, ptz.cb()); });
    ASSERT_TRUE(waitFor([&]() { return !ptz.get().empty(); }));
    EXPECT_EQ(0, countCalls(fake, "SetCameraTime"));

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });
    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "SetCameraTime"); }));

    // Flag consumed: the following cycle leaves the clock alone until the interval is due.
    onPoller([&]() { CameraControllerTestPeer::allowNextCycle(*controller, false); });
    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });
    ASSERT_TRUE(waitFor([&]() { return countCalls(fake, "Probe") == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(1, countCalls(fake, "SetCameraTime"));
}

TEST_F(CameraControllerTest, CameraOptionTurnsClockSyncOff) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(false, false);
    fake->setLastFailure(DeviceFailure::ClockSkew);
    CameraOption option;
    option.manufacturer = "ONVIF";
    option.ip = "127.0.0.1";
    option.port = 1;
    option.ptzMode = CameraOption::kPTZContinousMode;
    option.enableTimeSync = false;
    onPoller([&]() { controller->setupController(option); });

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });
    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "Connect"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(0, countCalls(fake, "SetCameraTime"));
}

TEST_F(CameraControllerTest, ServerConfigTurnsClockSyncOffForEveryCamera) {
    auto controller = _controller;
    auto fake = _fake;
    fake->setMaintenanceResults(false, false);
    fake->setLastFailure(DeviceFailure::ClockSkew);
    setClockSyncEnable(false);

    onPoller([&]() { CameraControllerTestPeer::onManager(*controller); });
    ASSERT_TRUE(waitFor([&]() { return sawCall(fake, "Connect"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(0, countCalls(fake, "SetCameraTime"));
    setClockSyncEnable(true);
}

// ── Speaker clock: set by the maintenance cycle like a camera's ──────────────

TEST_F(SpeakerProtocolTest, ClockIsSetOnConnectThenOnlyWhenDue) {
    setup("ONVIF");
    _world->probe_ok = true;
    cycle();
    EXPECT_TRUE(_speaker->isReady());
    EXPECT_EQ(1, _world->count("settime ONVIF"));
    // Still connected, interval not due: the cycle only probes.
    cycle();
    EXPECT_EQ(1, _world->count("probe ONVIF"));
    EXPECT_EQ(1, _world->count("settime ONVIF"));
}

TEST_F(SpeakerProtocolTest, ClockOffFoundByACommandIsSetByTheNextCycle) {
    setup("ONVIF");
    _world->probe_ok = true;
    cycle();
    _world->sync_wanted = true;
    cycle();
    EXPECT_EQ(2, _world->count("settime ONVIF"));
    cycle();
    EXPECT_EQ(2, _world->count("settime ONVIF")) << "flag consumed";
}

TEST_F(SpeakerProtocolTest, ConnectRejectedForTheClockSetsIt) {
    setup("ONVIF");
    _world->setResult(ControlProtocol::ONVIF, ConnectError::OTHER);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::ClockSkew);
    cycle();
    EXPECT_FALSE(_speaker->isReady());
    EXPECT_EQ(1, _world->count("settime ONVIF"));

    _world->clear();
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::Unauthorized);
    cycle();
    EXPECT_EQ(0, _world->count("settime ONVIF")) << "a wrong password leaves the clock alone";
}

TEST_F(SpeakerProtocolTest, AutoProbeRejectedForTheClockSetsItAndStaysOnOnvif) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::OTHER);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::ClockSkew);
    _world->setResult(ControlProtocol::ISAPI, ConnectError::NONE);
    setup("");
    cycle();
    auto events = _world->events();
    EXPECT_TRUE(contains(events, "settime ONVIF"));
    EXPECT_FALSE(contains(events, "create ISAPI"));
    EXPECT_EQ(ControlProtocol::NONE, current());
}

TEST_F(SpeakerProtocolTest, SpeakerOptionTurnsClockSyncOff) {
    setup("ONVIF", "", "ONVIF", false);
    cycle();
    _world->setResult(ControlProtocol::ONVIF, ConnectError::OTHER);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::ClockSkew);
    cycle();
    EXPECT_EQ(0, _world->count("settime ONVIF"));
}

TEST_F(SpeakerProtocolTest, ServerConfigTurnsClockSyncOffForSpeakers) {
    setClockSyncEnable(false);
    setup("ONVIF");
    cycle();
    EXPECT_TRUE(_speaker->isReady());
    EXPECT_EQ(0, _world->count("settime ONVIF"));
    setClockSyncEnable(true);
}

// ── /device/reconnect reports the cause of a failed attempt (data.code) ──────

TEST_F(SpeakerProtocolTest, ReconnectReportsWhyTheLockedProtocolFailed) {
    setup("ONVIF");
    _world->setResult(ControlProtocol::ONVIF, ConnectError::UNREACHABLE);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::Unreachable);
    auto speaker = _speaker;
    ReconnectWait wait;
    _poller->sync([&]() { speaker->reconnectNow(wait.callback()); });
    ASSERT_TRUE(wait.wait());
    EXPECT_TRUE(wait.result.finished);
    EXPECT_FALSE(wait.result.connected);
    EXPECT_EQ(DeviceFailure::Unreachable, wait.result.failure);
}

TEST_F(SpeakerProtocolTest, ReconnectReportsWhyTheProbeFailed) {
    _world->setResult(ControlProtocol::ONVIF, ConnectError::AUTH_FAILED);
    _world->setFailure(ControlProtocol::ONVIF, DeviceFailure::Unauthorized);
    _world->setResult(ControlProtocol::PSIA, ConnectError::NOT_THIS_PROTOCOL);
    setup("");
    auto speaker = _speaker;
    ReconnectWait wait;
    _poller->sync([&]() { speaker->reconnectNow(wait.callback()); });
    ASSERT_TRUE(wait.wait());
    EXPECT_FALSE(wait.result.connected);
    EXPECT_EQ(ConnectError::AUTH_FAILED, wait.result.error);
    EXPECT_EQ(DeviceFailure::Unauthorized, wait.result.failure) << "the ONVIF verdict, not PSIA's";
}

// ── Bosch vendor API: the cause of a failed request ──────────────────────────

TEST(BoschFailureTest, RequestFailureForTheApi) {
    EXPECT_EQ(DeviceFailure::Unauthorized, BoschIpSpeaker::classifyFailure(Err_success, 401));
    EXPECT_EQ(DeviceFailure::Unauthorized, BoschIpSpeaker::classifyFailure(Err_success, 403));
    EXPECT_EQ(DeviceFailure::None, BoschIpSpeaker::classifyFailure(Err_success, 204));
    EXPECT_EQ(DeviceFailure::Other, BoschIpSpeaker::classifyFailure(Err_success, 500));
    EXPECT_EQ(DeviceFailure::None, BoschIpSpeaker::classifyFailure(Err_other, 200)) << "answer came, body failed";
    EXPECT_EQ(DeviceFailure::Unreachable, BoschIpSpeaker::classifyFailure(Err_refused, 0));
    EXPECT_EQ(DeviceFailure::Unreachable, BoschIpSpeaker::classifyFailure(Err_dns, 0));
    EXPECT_EQ(DeviceFailure::Timeout, BoschIpSpeaker::classifyFailure(Err_timeout, 0));
    EXPECT_EQ(DeviceFailure::Timeout, BoschIpSpeaker::classifyFailure(Err_eof, 0));
}
