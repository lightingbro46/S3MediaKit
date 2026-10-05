#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <unistd.h>

#include "Camera/CameraStatistic.h"
#include "Speaker/SpeakerStatistic.h"
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

// Sets a sync_config_to_db key for the scope, restores it to 1 (the default) afterwards.
class SyncConfigGuard {
public:
    SyncConfigGuard(const std::string &key, int value) : _key(key) { set(value); }
    ~SyncConfigGuard() { set(1); }

private:
    void set(int value) {
        mINI::Instance()[_key] = value;
        NOTICE_EMIT(BroadcastReloadConfigArgs, mediakit::Broadcast::kBroadcastReloadConfig);
    }

    std::string _key;
};

bool escHasResource(const std::string &guid) {
    return !std::make_shared<VmsResourceImp>()->findByGuid(guid).empty();
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

// Status history (SQLite) is written on the camera's ESC queue instead of the camera poller; the
// gap still carries the time of the event, not the time the queued write ran.
TEST_F(CameraStatisticSyncTest, StreamStatusHistoryIsWrittenFromEscQueue) {
    const std::string guid = "cam-stat-history";
    auto stats = makeCamera(guid, false);
    const uint64_t before = static_cast<uint64_t>(time(nullptr));
    stats->addStreamStatistic(PrimaryStream, false, "connection timeout");
    const uint64_t after = static_cast<uint64_t>(time(nullptr));

    std::vector<LocalResourceStatusGap> gaps;
    ASSERT_TRUE(waitFor([&] {
        gaps = ResourceStatusManager::Instance().getResourceStatusGaps(guid + "-main", before - 10, after + 10);
        return !gaps.empty();
    }));
    EXPECT_GE(gaps[0].disconnect_time, before);
    EXPECT_LE(gaps[0].disconnect_time, after);
}

#ifdef ENABLE_FFMPEG
TEST(ReduceQualityCapabilityTest, BuildsStableProfilesFromTwoStreams) {
    CameraOption option;
    option.enableReduceQualityOnView = true;
    std::unordered_map<int, StreamTuple> streams;
    StreamTuple main;
    main.stream_id = "main";
    streams[PrimaryStream] = main;
    StreamTuple sub;
    sub.stream_id = "sub";
    streams[SecondaryStream] = sub;

    std::unordered_map<int, StreamStatistic> stats;
    StreamStatistic main_stats;
    main_stats.live = true;
    main_stats.has_video = true;
    main_stats.vcodec = "H264";
    main_stats.width = 3840;
    main_stats.height = 2160;
    main_stats.bitrate = 8000000;
    main_stats.fps = 30;
    stats[PrimaryStream] = main_stats;
    StreamStatistic sub_stats = main_stats;
    sub_stats.width = 1280;
    sub_stats.height = 720;
    sub_stats.bitrate = 1500000;
    sub_stats.fps = 20;
    stats[SecondaryStream] = sub_stats;

    const auto capability = ReduceQualityCapabilityBuilder::build(option, streams, stats);
    ASSERT_TRUE(capability.supported);
    ASSERT_FALSE(capability.profiles.empty());
    EXPECT_EQ("main", capability.profiles.front().source_stream_id);
    EXPECT_EQ(1920, capability.profiles.front().width);
    EXPECT_EQ(1080, capability.profiles.front().height);
    for (size_t i = 1; i < capability.profiles.size(); ++i) {
        EXPECT_GE(static_cast<long long>(capability.profiles[i - 1].width) * capability.profiles[i - 1].height,
                  static_cast<long long>(capability.profiles[i].width) * capability.profiles[i].height);
    }

    const Json::Value json = ReduceQualityCapabilityBuilder::toJson(capability);
    EXPECT_TRUE(json["supported"].asBool());
    EXPECT_EQ(capability.profiles.size(), json["profiles"].size());
}

TEST(ReduceQualityCapabilityTest, DisabledOptionPublishesNoProfiles) {
    CameraOption option;
    const auto capability = ReduceQualityCapabilityBuilder::build(
        option, std::unordered_map<int, StreamTuple>(), std::unordered_map<int, StreamStatistic>());
    EXPECT_FALSE(capability.supported);
    EXPECT_TRUE(capability.profiles.empty());
}

TEST(ReduceQualityCapabilityTest, BuildsProfilesWhenSourceFpsIsInitiallyUnknown) {
    CameraOption option;
    option.enableReduceQualityOnView = true;
    std::unordered_map<int, StreamTuple> streams;
    StreamTuple main;
    main.stream_id = "main";
    streams[PrimaryStream] = main;

    std::unordered_map<int, StreamStatistic> stats;
    StreamStatistic source;
    source.live = true;
    source.has_video = true;
    source.vcodec = "H264";
    source.width = 1920;
    source.height = 1080;
    source.bitrate = 4000000;
    source.fps = 0;
    stats[PrimaryStream] = source;

    const auto capability = ReduceQualityCapabilityBuilder::build(option, streams, stats);
    ASSERT_TRUE(capability.supported);
    ASSERT_FALSE(capability.profiles.empty());
    EXPECT_EQ("main", capability.profiles.front().source_stream_id);
    EXPECT_GT(capability.profiles.front().fps, 0);
}

TEST(ReduceQualityCapabilityTest, CapsProfileFpsWhenSourceFpsBecomesKnown) {
    CameraOption option;
    option.enableReduceQualityOnView = true;
    std::unordered_map<int, StreamTuple> streams;
    StreamTuple main;
    main.stream_id = "main";
    streams[PrimaryStream] = main;

    std::unordered_map<int, StreamStatistic> stats;
    StreamStatistic source;
    source.live = true;
    source.has_video = true;
    source.vcodec = "H264";
    source.width = 1920;
    source.height = 1080;
    source.bitrate = 4000000;
    source.fps = 8;
    stats[PrimaryStream] = source;

    const auto capability = ReduceQualityCapabilityBuilder::build(option, streams, stats);
    ASSERT_FALSE(capability.profiles.empty());
    for (const auto &profile : capability.profiles) {
        EXPECT_LE(profile.fps, 8);
    }
}

#endif

TEST_F(CameraStatisticSyncTest, ReduceQualityOptionPersistsAndSyncs) {
    std::string guid = "cam-stat-reduce-quality";
    {
        auto stats = makeCamera(guid);
        CameraOption option = activeOption(3600);
        option.enableReduceQualityOnView = true;
        stats->setCameraOption(option);
        ASSERT_TRUE(waitFor([&] { return kvValue(guid, "enableReduceQualityOnView") == "1"; }));
    }

    auto reloaded = std::make_shared<CameraStatisticImp>(_dir + "/" + guid);
    EXPECT_TRUE(reloaded->getParams().option.enableReduceQualityOnView);

    CameraStatistic from_esc;
    ASSERT_TRUE(CameraStatisticImp::syncFromEsc(guid, from_esc));
    EXPECT_TRUE(from_esc.option.enableReduceQualityOnView);
}

// camera.sync_config_to_db=0 keeps the config out of ESC (ownership is still written); turning it
// back on publishes the next change.
TEST_F(CameraStatisticSyncTest, ConfigSyncOffSkipsEscConfig) {
    const std::string guid = "cam-stat-sync-off";
    CameraStatisticImp::Ptr stats;
    {
        SyncConfigGuard guard(mediakit::Camera::kSyncConfigToDb, 0);
        stats = makeCamera(guid);
        stats->setCameraOption(activeOption(3600));
        ASSERT_TRUE(waitFor([&] {
            return !std::make_shared<VmsResourceAssignmentImp>()->findCurrentAssignment(guid).empty();
        }));
        waitEscIdle();
        EXPECT_FALSE(escHasResource(guid));
        EXPECT_EQ("", kvValue(guid, "keepArchivedMaxFor"));
    }

    stats->setCameraOption(activeOption(7200));
    EXPECT_TRUE(waitFor([&] { return kvValue(guid, "keepArchivedMaxFor") == "7200"; }))
        << "keepArchivedMaxFor=" << kvValue(guid, "keepArchivedMaxFor");
}

namespace {

class SpeakerStatisticSyncTest : public CameraStatisticSyncTest {
protected:
    SpeakerStatisticImp::Ptr makeSpeaker(const std::string &device_id, const std::string &media_server, bool sync_mode = true) {
        auto stats = std::make_shared<SpeakerStatisticImp>(_dir + "/" + device_id);
        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = device_id;
        tuple.name = device_id;
        stats->setDeviceTuple(tuple);

        SpeakerOption option;
        option.name = "speaker";
        option.ip = "127.0.0.1";
        option.port = 80;
        option.preferedMediaServer = media_server;
        option.audioPlaybackMode = SpeakerOption::kAudioPlaybackModeOnvifBackchannel;
        stats->setSpeakerOption(option);
        stats->setSyncMode(sync_mode);
        return stats;
    }
};

} // namespace

TEST_F(SpeakerStatisticSyncTest, ConfigIsSyncedAndRemoved) {
    const std::string guid = "spk-stat-config";
    auto stats = makeSpeaker(guid, ResourceManager::Instance().getSelfNodeId());
    // The kv pairs land one by one: wait for the last one written.
    ASSERT_TRUE(waitFor([&] { return kvValue(guid, "enableTimeSync") == "1"; }));
    EXPECT_EQ("speaker", kvValue(guid, "name"));
    EXPECT_EQ("1", kvValue(guid, "audioPlaybackMode"));

    EXPECT_EQ("127.0.0.1", kvValue(guid, "ip"));
    auto resources = std::make_shared<VmsResourceImp>()->findByGuid(guid);
    ASSERT_EQ(1u, resources.size());
    EXPECT_EQ(ResourceManager::Instance().getSelfNodeId(), resources[0].parent_guid);
    EXPECT_EQ("http://127.0.0.1:80", resources[0].url);

    auto option = stats->getParams().option;
    option.name = "speaker-renamed";
    stats->setSpeakerOption(option);
    EXPECT_TRUE(waitFor([&] { return kvValue(guid, "name") == "speaker-renamed"; }));

    stats->remove();
    EXPECT_TRUE(waitFor([&] { return !escHasResource(guid); }));
}

// Sync mode off (speaker loaded from info.txt, or leaving this node) or speaker.sync_config_to_db=0:
// nothing is written.
TEST_F(SpeakerStatisticSyncTest, NoWriteWhenSyncIsOff) {
    const std::string local_guid = "spk-stat-local";
    auto local = makeSpeaker(local_guid, ResourceManager::Instance().getSelfNodeId(), false);

    const std::string off_guid = "spk-stat-config-off";
    {
        SyncConfigGuard guard(mediakit::Speaker::kSyncConfigToDb, 0);
        auto off = makeSpeaker(off_guid, ResourceManager::Instance().getSelfNodeId());
        waitEscIdle();
    }
    EXPECT_FALSE(escHasResource(local_guid));
    EXPECT_FALSE(escHasResource(off_guid));
}

// A speaker moved to another media server: the old node must not delete the new owner's record.
TEST_F(SpeakerStatisticSyncTest, RemoveKeepsRecordOfOtherMediaServer) {
    const std::string guid = "spk-stat-moved";
    auto stats = makeSpeaker(guid, "other-media-server");
    ASSERT_TRUE(waitFor([&] { return escHasResource(guid); }));

    stats->remove();
    waitEscIdle();
    EXPECT_TRUE(escHasResource(guid));
}
