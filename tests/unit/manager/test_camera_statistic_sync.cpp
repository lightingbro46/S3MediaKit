#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <unistd.h>

#include "Camera/CameraStatistic.h"
#include "Common/config.h"
#include "Extension/Resource.h"
#include "Storage/TransactionSequence.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceAssignment.h"
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

    CameraStatisticImp::Ptr makeCamera(const std::string &device_id) {
        auto stats = std::make_shared<CameraStatisticImp>(_dir + "/" + device_id);
        stats->setSyncMode(true);

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
