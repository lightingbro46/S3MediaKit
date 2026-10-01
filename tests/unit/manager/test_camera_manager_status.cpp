#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include "Camera/CameraManager.h"
#include "Common/DeviceSource.h"
#include "Common/config.h"

using namespace managerkit;
using namespace toolkit;
using namespace mediakit;

namespace {

const char kReportDelayKey[] = "manager.deviceStatusReportDelaySec";

class TestDeviceSource : public DeviceSource {
public:
    explicit TestDeviceSource(const std::string &device_id)
        : DeviceSource(GENERIC_RTSP_CAMERA_SCHEMA, DeviceTuple{"tenant", device_id, device_id}) {}
};

class StatusReports {
public:
    StatusReports() {
        NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastDeviceStatsChanged, [this](BroadcastDeviceStatsChangedArgs) {
            std::set<std::string> ids;
            for (const auto &device : devices) {
                ids.insert(device->getDeviceTuple().device_id);
            }
            std::lock_guard<std::mutex> lck(_mtx);
            _reports.push_back(ids);
            _cv.notify_all();
        });
    }

    ~StatusReports() { NoticeCenter::Instance().delListener(this, Broadcast::kBroadcastDeviceStatsChanged); }

    bool waitFor(size_t count, int timeout_ms) {
        std::unique_lock<std::mutex> lck(_mtx);
        return _cv.wait_for(lck, std::chrono::milliseconds(timeout_ms), [&]() { return _reports.size() >= count; });
    }

    std::vector<std::set<std::string>> reports() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _reports;
    }

private:
    std::mutex _mtx;
    std::condition_variable _cv;
    std::vector<std::set<std::string>> _reports;
};

void setReportDelay(int sec) {
    mINI::Instance()[kReportDelayKey] = sec;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
}

} // namespace

TEST(CameraManagerStatusTest, CollectsChangesInWindowIntoOneReport) {
    setReportDelay(1);
    StatusReports listener;
    auto cam_a = std::make_shared<TestDeviceSource>("status-cam-a");
    auto cam_b = std::make_shared<TestDeviceSource>("status-cam-b");
    auto removed = std::make_shared<TestDeviceSource>("status-cam-removed");

    auto &manager = CameraManager::Instance();
    manager.notifyStatusChanged(cam_a);
    manager.notifyStatusChanged(cam_b);
    manager.notifyStatusChanged(cam_a); // second stream or flapping of the same camera
    manager.notifyStatusChanged(removed);
    removed.reset();

    EXPECT_FALSE(listener.waitFor(1, 300)) << "changes must wait for the window";
    ASSERT_TRUE(listener.waitFor(1, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto reports = listener.reports();
    ASSERT_EQ(1U, reports.size());
    EXPECT_EQ((std::set<std::string>{"status-cam-a", "status-cam-b"}), reports[0]);

    // A change after the flush opens a new window
    manager.notifyStatusChanged(cam_b);
    ASSERT_TRUE(listener.waitFor(2, 3000));
    EXPECT_EQ((std::set<std::string>{"status-cam-b"}), listener.reports()[1]);
}

TEST(CameraManagerStatusTest, ReportsAtOnceWhenDelayIsZero) {
    setReportDelay(0);
    StatusReports listener;
    auto cam = std::make_shared<TestDeviceSource>("status-cam-now");

    CameraManager::Instance().notifyStatusChanged(cam);
    ASSERT_TRUE(listener.waitFor(1, 500));
    EXPECT_EQ((std::set<std::string>{"status-cam-now"}), listener.reports()[0]);
    setReportDelay(1);
}
