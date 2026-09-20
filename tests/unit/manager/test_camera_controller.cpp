#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Camera/CameraController.h"
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
    static void syncMediaProfile(CameraController &c, const OnvifControl::Ptr &ctr) { c.syncMediaProfile(ctr); }

    static bool profileConfig(CameraController &c, const std::string &token, VideoEncoderConfig &out) {
        auto it = c._profileConfigMap.find(token);
        if (it == c._profileConfigMap.end()) return false;
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
    bool setVideoEncoderConfigByToken(const std::string &token, const VideoEncoderConfig &) override {
        record("SetVideoEncoderConfig", token);
        std::lock_guard<std::mutex> lck(_mtx);
        return token != _failing_token;
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
    OnvifMediaProfileMap _profiles;
    std::string _failing_token;
    std::atomic<int> _concurrent{0};
    std::atomic<int> _max_concurrent{0};
};

class TestCameraController : public CameraController {
public:
    TestCameraController(const DeviceTuple &tuple, const EventPoller::Ptr &poller, std::shared_ptr<FakeOnvifControl> fake)
        : CameraController(tuple, poller), _fake(std::move(fake)) {}

protected:
    OnvifControl::Ptr createOnvifControl(const std::string &, const std::string &, const std::string &) override {
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
