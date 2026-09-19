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
