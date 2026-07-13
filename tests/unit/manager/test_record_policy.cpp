#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

#include "Camera/RecordPolicy.h"
#include "Poller/EventPoller.h"

using namespace managerkit;

namespace {

class CapturingDeviceEvent : public DeviceSourceEvent {
public:
    std::atomic<int> mode_calls{0};
    std::atomic<int> quality_calls{0};
    std::atomic<int> last_mode{0};
    std::atomic<int> last_start{0};
    std::atomic<int> last_fps{0};
    std::atomic<int> last_quality{0};

    void onRecordModeChange(DeviceSource &, int mode, bool start) override {
        last_mode = mode;
        last_start = start ? 1 : 0;
        ++mode_calls;
    }
    void onImageQualityChange(DeviceSource &, int fps, int quality) override {
        last_fps = fps;
        last_quality = quality;
        ++quality_calls;
    }
};

std::string currentSchedule(RecordMode mode, int fps, const std::string &quality) {
    std::time_t now = std::time(nullptr);
    std::tm local = *std::localtime(&now);
    int day = (local.tm_wday + 6) % 7;
    return "[{\"dh\":\"" + std::to_string(day) + "," + std::to_string(local.tm_hour) +
           "\",\"ty\":" + std::to_string(static_cast<int>(mode)) +
           ",\"fps\":" + std::to_string(fps) + ",\"q\":\"" + quality + "\"}]";
}

bool waitFor(const std::atomic<int> &value, int expected, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        if (value.load() >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return value.load() >= expected;
}

} // namespace

TEST(RecordPolicyTest, ConvertsEnumsToStableStrings) {
    EXPECT_EQ("NoRecord", getRecordModeString(RecordMode::NoRecord));
    EXPECT_EQ("RecordLowResAndMotion", getRecordModeString(RecordMode::RecordLowResAndMotion));
    EXPECT_EQ("RecordOnlyMotion", getRecordModeString(RecordMode::RecordOnlyMotion));
    EXPECT_EQ("RecordAlways", getRecordModeString(RecordMode::RecordAlways));
    EXPECT_EQ("Unknown", getRecordModeString(RecordMode::RecordModeMax));
    EXPECT_EQ("Low", getImageQualityString(ImageQuality::Low));
    EXPECT_EQ("Medium", getImageQualityString(ImageQuality::Medium));
    EXPECT_EQ("High", getImageQualityString(ImageQuality::High));
    EXPECT_EQ("Unknown", getImageQualityString(ImageQuality::QualityMax));
    EXPECT_EQ("Unknown", getRecordEventTypeString(RecordEventType::Unknown));
    EXPECT_EQ("Motion", getRecordEventTypeString(RecordEventType::Motion));
    EXPECT_EQ("Unknown", getRecordEventTypeString(static_cast<RecordEventType>(99)));
}

TEST(RecordPolicyTest, DefaultsToNoRecordWithoutActiveTimerSlot) {
    RecordScheduler scheduler(DeviceTuple{"vhost", "camera", "name"}, "", nullptr);
    EXPECT_EQ("", scheduler.getProfile());
    EXPECT_FALSE(scheduler.isEventActive());
    EXPECT_FALSE(scheduler.setupRecordEvent(RecordEventType::Motion, true));
    EXPECT_TRUE(scheduler.isEventActive());
    scheduler.stopTimer();

    RecordScheduler malformed(DeviceTuple{"vhost", "bad", "name"}, "not-json", nullptr);
    EXPECT_FALSE(malformed.setupRecordEvent(RecordEventType::Motion, false));
    RecordScheduler non_array(DeviceTuple{"vhost", "object", "name"}, "{}", nullptr);
    EXPECT_FALSE(non_array.setupRecordEvent(RecordEventType::Motion, true));
}

TEST(RecordPolicyTest, AppliesCurrentScheduleAndMotionTransitions) {
    auto listener = std::make_shared<CapturingDeviceEvent>();
    auto poller = toolkit::EventPollerPool::Instance().getPoller();
    auto profile = currentSchedule(RecordMode::RecordOnlyMotion, 12, "H");
    auto scheduler = RecordScheduler::create(DeviceTuple{"vhost", "camera-motion", "name"}, profile, poller);
    scheduler->setListener(listener);

    ASSERT_TRUE(waitFor(listener->mode_calls, 1, 1800));
    EXPECT_EQ(static_cast<int>(RecordMode::RecordOnlyMotion), listener->last_mode.load());
    EXPECT_EQ(0, listener->last_start.load());
    EXPECT_EQ(12, listener->last_fps.load());
    EXPECT_EQ(static_cast<int>(ImageQuality::High), listener->last_quality.load());

    EXPECT_FALSE(scheduler->setupRecordEvent(RecordEventType::Unknown, true));
    EXPECT_TRUE(scheduler->setupRecordEvent(RecordEventType::Motion, true));
    EXPECT_TRUE(scheduler->isEventActive());
    EXPECT_EQ(1, listener->last_start.load());
    EXPECT_TRUE(scheduler->setupRecordEvent(RecordEventType::Motion, false));
    EXPECT_FALSE(scheduler->isEventActive());
    EXPECT_EQ(0, listener->last_start.load());
    scheduler->stopTimer();
}
