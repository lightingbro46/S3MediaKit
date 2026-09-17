#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <unistd.h>

#include "Camera/CameraStatistic.h"
#include "Common/config.h"
#include "Local/StorageTier.h"
#include "Extension/Resource.h"
#include "Storage/TransactionSequence.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceAssignment.h"
#include "Storage/VmsResourceStatus.h"
#include "Util/util.h"

// Runs in the unit_sync_apply binary: the temporary ESC/mserver databases are created by the
// global environment registered in test_sync_apply.cpp.

using namespace managerkit;
using namespace toolkit;

namespace {

bool waitFor(const std::function<bool()> &pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

std::string kvValue(const std::string &guid, const std::string &name) {
    for (const auto &kv : std::make_shared<VmsKvPairImp>()->findAllKeyValue(guid)) {
        if (kv.name == name) return kv.value;
    }
    return "";
}

int ownCursor() {
    auto &rm = ResourceManager::Instance();
    return std::make_shared<TransactionSequenceImp>()->findSeqByPeerIdAndDbGuid(rm.getSelfNodeId(), rm.getSelfDbGuid());
}

// Waits until no ESC write happened for `quiet_ms`, i.e. the per-camera ESC queue has drained.
void waitEscIdle(int quiet_ms = 300) {
    int last = ownCursor();
    auto stable_since = std::chrono::steady_clock::now();
    auto deadline = stable_since + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int now = ownCursor();
        if (now != last) {
            last = now;
            stable_since = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - stable_since >= std::chrono::milliseconds(quiet_ms)) {
            return;
        }
    }
}

class CameraStatisticSyncTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/s3mk_cam_stats_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        _dir = dir;
    }

    void TearDown() override {
        std::string cmd = "rm -rf '" + _dir + "'";
        (void)std::system(cmd.c_str());
    }

    CameraStatisticImp::Ptr makeCamera(const std::string &device_id, bool sync_mode = true, bool with_secondary = false) {
        auto stats = std::make_shared<CameraStatisticImp>(_dir + "/" + device_id);
        stats->setSyncMode(sync_mode);

        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = device_id;
        tuple.name = device_id;
        stats->setDeviceTuple(tuple);

        std::unordered_map<int, StreamTuple> streams;
        StreamTuple main;
        main.vhost = DEFAULT_VHOST;
        main.device_id = device_id;
        main.name = getStreamTypeString(PrimaryStream);
        main.stream_id = device_id + "-main";
        main.full_url = "rtsp://127.0.0.1:9/" + device_id;
        streams[PrimaryStream] = main;
        if (with_secondary) {
            StreamTuple sub = main;
            sub.name = getStreamTypeString(SecondaryStream);
            sub.stream_id = device_id + "-sub";
            sub.full_url = "rtsp://127.0.0.1:9/" + device_id + "/sub";
            streams[SecondaryStream] = sub;
        }
        stats->setStreamTuples(streams);
        return stats;
    }

    static CameraOption activeOption(uint64_t keep_max_sec) {
        CameraOption option;
        option.name = "camera";
        option.enableActive = true;
        option.keepArchivedMaxFor = keep_max_sec;
        return option;
    }

    std::string _dir;
};

} // namespace

// Regression: config changes within the 30s throttle window used to be skipped and only
// reached ESC on a later statistic save (or never, if sync mode was turned off meanwhile).
TEST_F(CameraStatisticSyncTest, ConfigChangeIsSyncedImmediately) {
    const std::string guid = "cam-stat-config";
    auto stats = makeCamera(guid);

    stats->setCameraOption(activeOption(3600));
    ASSERT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "3600"; }));
    ASSERT_TRUE(waitFor([&] {
        return !std::make_shared<VmsResourceAssignmentImp>()->findCurrentAssignment(guid).empty();
    }));

    // Second change right away: must not wait for the throttle interval.
    stats->setCameraOption(activeOption(7200));
    EXPECT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "7200"; }))
        << "keepArchivedMaxFor=" << kvValue(guid, "keepArchivedMaxFor");
}

// Statistic updates (archive size, bookmarks, transports) must not rewrite ESC.
TEST_F(CameraStatisticSyncTest, StatisticSavesDoNotWriteEsc) {
    const std::string guid = "cam-stat-stats";
    auto stats = makeCamera(guid);
    stats->setCameraOption(activeOption(3600));
    ASSERT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "3600"; }));
    waitEscIdle();

    const int before = ownCursor();
    uint64_t future = static_cast<uint64_t>(time(nullptr)) + 100;
    for (int i = 0; i < 5; ++i) {
        stats->addBookmarkCount(future + i, 1024, true);
        stats->addArchiveSize(guid + "-main", 1, 4096, future, future + i + 1, true);
    }
    TransportStats transports;
    transports.liveTransports.push_back("rtsp");
    stats->setTransportStats(transports);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(before, ownCursor());
}

// ESC writes are queued per camera; a remove() issued right after a config change must win,
// otherwise the pending upsert would recreate the resource after it was deleted.
TEST_F(CameraStatisticSyncTest, RemoveRunsAfterPendingSync) {
    const std::string guid = "cam-stat-remove";
    auto stats = makeCamera(guid);
    stats->setCameraOption(activeOption(3600));
    stats->remove(false);

    // removeResource() releases the assignment; that is the last queued ESC write.
    ASSERT_TRUE(waitFor([&] {
        auto last = std::make_shared<VmsResourceAssignmentImp>()->findLastAssignment(guid);
        return !last.empty() && last[0].released_at != 0;
    }));
    waitEscIdle();
    EXPECT_TRUE(std::make_shared<VmsResourceImp>()->findByGuid(guid).empty());
    EXPECT_TRUE(std::make_shared<VmsKvPairImp>()->findAllKeyValue(guid).empty());
}

namespace {

bool hasActiveAssignment(const std::string &guid) {
    return !std::make_shared<VmsResourceAssignmentImp>()->findCurrentAssignment(guid).empty();
}

std::string escUrl(const std::string &guid) {
    auto rows = std::make_shared<VmsResourceImp>()->findByGuid(guid);
    return rows.empty() ? "" : rows[0].url;
}

} // namespace

// Regression: after a restart the camera option is loaded with sync mode off; when the service
// API re-sends the same config the camera skips setCameraOption, so enabling sync mode must
// publish ownership and config by itself.
TEST_F(CameraStatisticSyncTest, EnablingSyncModePublishesActiveCamera) {
    const std::string guid = "cam-stat-resync";
    auto stats = makeCamera(guid, false);
    stats->setCameraOption(activeOption(3600));
    waitEscIdle();
    EXPECT_TRUE(kvValue(guid, "keepArchivedMaxFor").empty());
    EXPECT_FALSE(hasActiveAssignment(guid));

    stats->setSyncMode(true);
    EXPECT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "3600"; }));
    EXPECT_TRUE(waitFor([&] { return hasActiveAssignment(guid); }));
}

// Regression: a camera leaving this node published its local "enableActive=0" option, which
// overwrote the config of the node taking the camera over.
TEST_F(CameraStatisticSyncTest, DetachReleasesWithoutPublishingConfig) {
    const std::string guid = "cam-stat-detach";
    auto stats = makeCamera(guid);
    stats->setCameraOption(activeOption(3600));
    ASSERT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "3600" && hasActiveAssignment(guid); }));

    stats->detachFromCluster();
    ASSERT_TRUE(waitFor([&] { return !hasActiveAssignment(guid); }));
    waitEscIdle();

    const int before = ownCursor();
    CameraOption inactive = activeOption(7200);
    inactive.enableActive = false;
    inactive.enableFailover = true;
    stats->setCameraOption(inactive);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(before, ownCursor());
    EXPECT_EQ("1", kvValue(guid, "enableActive"));
    EXPECT_EQ("3600", kvValue(guid, "keepArchivedMaxFor"));

    // Taking the camera back publishes again.
    stats->setSyncMode(true);
    stats->setCameraOption(activeOption(7200));
    EXPECT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "7200" && hasActiveAssignment(guid); }));
}

// Regression: the replicated status depended on unordered_map iteration order and flapped between
// UNAUTHORIZED and OFFLINE.
TEST_F(CameraStatisticSyncTest, UnauthorizedStatusWinsOverOffline) {
    const std::vector<std::pair<int, int>> orders = {{PrimaryStream, SecondaryStream}, {SecondaryStream, PrimaryStream}};
    for (size_t i = 0; i < orders.size(); ++i) {
        const std::string guid = "cam-stat-status-" + std::to_string(i);
        auto stats = makeCamera(guid, true, true);
        stats->setCameraOption(activeOption(3600));
        stats->addStreamStatistic(orders[i].first, false, "401 Unauthorized");
        stats->addStreamStatistic(orders[i].second, false, "connection timeout");
        EXPECT_TRUE(waitFor([&] {
            return std::make_shared<VmsResourceStatusImp>()->findStatus(guid) == static_cast<int>(ResourceStatus::UNAUTHORIZED);
        })) << "order " << i;
    }
}

// Regression: the ESC url chose between webPort and port using autoMediaPort.
TEST_F(CameraStatisticSyncTest, EscUrlFollowsWebPortSetting) {
    const std::string guid = "cam-stat-url";
    auto stats = makeCamera(guid);
    CameraOption option = activeOption(3600);
    option.ip = "10.0.0.5";
    option.port = 80;
    option.webPort = 8080;
    option.autoWebPort = false;
    option.autoMediaPort = true;
    stats->setCameraOption(option);
    EXPECT_TRUE(waitFor([&] { return escUrl(guid) == "http://10.0.0.5:8080"; })) << escUrl(guid);

    option.autoWebPort = true;
    option.autoMediaPort = false;
    stats->setCameraOption(option);
    EXPECT_TRUE(waitFor([&] { return escUrl(guid) == "http://10.0.0.5:80"; })) << escUrl(guid);
}

TEST_F(CameraStatisticSyncTest, SyncFromEscKeepsOnlyConfiguredStreams) {
    std::string guid = "cam-stat-streams";
    auto stats = makeCamera(guid);
    stats->setCameraOption(activeOption(3600));
    ASSERT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "3600"; }));
    waitEscIdle();

    CameraStatistic from_esc;
    ASSERT_TRUE(CameraStatisticImp::syncFromEsc(guid, from_esc));
    EXPECT_EQ(1u, from_esc.stream_map.count(PrimaryStream));
    EXPECT_EQ(0u, from_esc.stream_map.count(SecondaryStream));
    EXPECT_EQ(3600u, from_esc.option.keepArchivedMaxFor);
}

// Regression: load() dropped the motion and tier keep thresholds saved in info.txt.
TEST_F(CameraStatisticSyncTest, ReloadKeepsMotionAndTierThresholds) {
    const std::string guid = "cam-stat-reload";
    {
        auto stats = makeCamera(guid, false);
        stats->addMotionKeepThreshold(true, 123);
        stats->addMotionKeepThreshold(false, 456);
#ifdef ENABLE_TIER_STORAGE
        stats->addTierKeepThreshold(HotTier, true, 789);
#endif
    }
    auto reloaded = std::make_shared<CameraStatisticImp>(_dir + "/" + guid);
    auto params = reloaded->getParams();
    EXPECT_EQ(123u, params.motion_stats.archiveStartTime);
    EXPECT_EQ(456u, params.motion_stats.archiveEndTime);
#ifdef ENABLE_TIER_STORAGE
    EXPECT_EQ(789u, params.tier_storage_map[HotTier].archiveStartTime);
#endif
}
