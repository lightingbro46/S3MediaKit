#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "Server/ClusterManager.h"
#include "Server/HealthMonitor.h"
#include "Server/HealthEvaluator.h"
#include "Poller/EventPoller.h"
#ifdef ENABLE_SYNC_CHANGES
#include "Extension/SyncManager.h"
#endif

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

class ClusterManagerTestPeer {
public:
    static void reset(ClusterManager &manager) {
        lock_guard<mutex> lck(manager._mtx);
        manager._map_server_info.clear();
        manager._map_peer_url.clear();
        manager._map_peer_revision.clear();
        manager._next_peer_revision = 0;
    }

    static void seed(ClusterManager &manager, const string &id, const MediaServerInfo &info,
                     const string &url, uint64_t revision) {
        lock_guard<mutex> lck(manager._mtx);
        manager._map_server_info[id] = info;
        manager._map_peer_url[id] = url;
        manager._map_peer_revision[id] = revision;
        manager._next_peer_revision = revision;
    }

    static uint64_t revision(ClusterManager &manager, const string &id) {
        lock_guard<mutex> lck(manager._mtx);
        auto it = manager._map_peer_revision.find(id);
        return it == manager._map_peer_revision.end() ? 0 : it->second;
    }

    static void eraseState(ClusterManager &manager, const string &id) {
        lock_guard<mutex> lck(manager._mtx);
        manager._map_server_info.erase(id);
        manager._map_peer_url.erase(id);
        manager._map_peer_revision.erase(id);
    }

    static void applyResult(ClusterManager &manager, const string &id, const string &urls,
                            uint64_t revision, const string &err, int idx) {
        manager.onHealthCheckResult(id, urls, revision, err, idx);
    }
};

// Friend of HealthMonitor: runs the cluster collector alone.
class HealthMonitorTestPeer {
public:
    static HealthEvaluation collectCluster(Json::Value &summary) {
        HealthHysteresis hysteresis;
        HealthEvaluator ev(hysteresis, 1, time(nullptr));
        HealthMonitor::Instance().collectCluster(ev, summary, time(nullptr));
        return ev.finish();
    }

    static double sampleThreadCpu(const vector<string> &names, string &busiest) {
        return HealthMonitor::Instance().sampleThreadCpu(names, busiest);
    }
};

#ifdef ENABLE_SYNC_CHANGES
// Friend of SyncManager. test_sync_apply.cpp has its own definition, in another test binary.
class SyncManagerTestPeer {
public:
    static void markPullComplete(SyncManager &m, const string &peer_id, uint64_t sent_ms) {
        m.markPullComplete(peer_id, sent_ms);
    }
};
#endif

namespace {

MediaServerInfo peerInfo(const string &id, const string &ip) {
    MediaServerInfo info;
    info.id = id;
    info.name = id;
    info.ip = ip;
    info.httpPort = 8080;
    info.isAutoHttpPort = true;
    info.useWebDomain = false;
    info.useDomain = false;
    info.useCustomPath = false;
    info.serverGroupId = 1;
    return info;
}

class ClusterManagerRevisionTest : public testing::Test {
protected:
    void SetUp() override {
        ClusterManagerTestPeer::reset(ClusterManager::Instance());
    }

    void TearDown() override {
        ClusterManagerTestPeer::reset(ClusterManager::Instance());
    }
};

TEST_F(ClusterManagerRevisionTest, UpdatingPeerInvalidatesHealthyUrlAndStartsNewRevision) {
    auto &manager = ClusterManager::Instance();
    ClusterManagerTestPeer::seed(manager, "peer-a", peerInfo("peer-a", "10.0.0.1"),
                                 "http://10.0.0.1:8080", 7);

    manager.addMediaServer("peer-a", peerInfo("peer-a", "10.0.0.2"), true);

    EXPECT_EQ("10.0.0.2", manager.getMediaServer("peer-a").ip);
    EXPECT_TRUE(manager.getPeerUrl("peer-a").empty());
    EXPECT_GT(ClusterManagerTestPeer::revision(manager, "peer-a"), 7u);
}

TEST_F(ClusterManagerRevisionTest, StaleResultsCannotOverwriteCurrentPeerState) {
    auto &manager = ClusterManager::Instance();
    ClusterManagerTestPeer::seed(manager, "peer-a", peerInfo("peer-a", "10.0.0.2"),
                                 "http://10.0.0.2:8080", 2);

    ClusterManagerTestPeer::applyResult(manager, "peer-a", "http://10.0.0.1:8080", 1, "", 0);
    EXPECT_EQ("http://10.0.0.2:8080", manager.getPeerUrl("peer-a"));

    ClusterManagerTestPeer::applyResult(manager, "peer-a", "http://10.0.0.1:8080", 1, "timeout", -1);
    EXPECT_EQ("http://10.0.0.2:8080", manager.getPeerUrl("peer-a"));
}

TEST_F(ClusterManagerRevisionTest, CurrentFailureInvalidatesHealthyUrl) {
    auto &manager = ClusterManager::Instance();
    ClusterManagerTestPeer::seed(manager, "peer-a", peerInfo("peer-a", "10.0.0.2"),
                                 "http://10.0.0.2:8080", 2);

    ClusterManagerTestPeer::applyResult(manager, "peer-a", "http://10.0.0.2:8080", 2, "timeout", -1);

    EXPECT_TRUE(manager.getPeerUrl("peer-a").empty());
}

TEST_F(ClusterManagerRevisionTest, RemovedPeerCannotBeRestoredByInflightResult) {
    auto &manager = ClusterManager::Instance();
    ClusterManagerTestPeer::seed(manager, "peer-a", peerInfo("peer-a", "10.0.0.1"),
                                 "http://10.0.0.1:8080", 3);
    ClusterManagerTestPeer::eraseState(manager, "peer-a");

    ClusterManagerTestPeer::applyResult(manager, "peer-a", "http://10.0.0.1:8080", 3, "", 0);

    EXPECT_TRUE(manager.getPeerUrl("peer-a").empty());
    EXPECT_TRUE(manager.getMediaServer("peer-a").id.empty());
}

TEST_F(ClusterManagerRevisionTest, SameEndpointChangeKeepsHealthyUrlAndRevision) {
    auto &manager = ClusterManager::Instance();
    ClusterManagerTestPeer::seed(manager, "peer-a", peerInfo("peer-a", "10.0.0.1"),
                                 "http://10.0.0.1:8080", 4);

    auto info = peerInfo("peer-a", "10.0.0.1");
    info.name = "renamed";
    info.hasFailover = true;
    info.rtspPort = 1554;
    manager.addMediaServer("peer-a", info, true);

    EXPECT_EQ("renamed", manager.getMediaServer("peer-a").name);
    EXPECT_EQ("http://10.0.0.1:8080", manager.getPeerUrl("peer-a"));
    EXPECT_EQ(4u, ClusterManagerTestPeer::revision(manager, "peer-a"));
}

TEST_F(ClusterManagerRevisionTest, HttpPortChangeInvalidatesHealthyUrl) {
    auto &manager = ClusterManager::Instance();
    ClusterManagerTestPeer::seed(manager, "peer-a", peerInfo("peer-a", "10.0.0.1"),
                                 "http://10.0.0.1:8080", 4);

    auto info = peerInfo("peer-a", "10.0.0.1");
    info.httpPort = 8081;
    manager.addMediaServer("peer-a", info, true);

    EXPECT_TRUE(manager.getPeerUrl("peer-a").empty());
    EXPECT_GT(ClusterManagerTestPeer::revision(manager, "peer-a"), 4u);
}

TEST_F(ClusterManagerRevisionTest, SyncPeersAreHealthyPeersOfThisGroupOnly) {
    auto &manager = ClusterManager::Instance();
    const int self_group = mINI::Instance()[Cluster::kServerLocationId];
    const string self_id = mINI::Instance()[General::kMediaServerId];

    auto same_group = peerInfo("peer-a", "10.0.0.1");
    same_group.serverGroupId = self_group;
    ClusterManagerTestPeer::seed(manager, "peer-a", same_group, "http://10.0.0.1:8080", 1);

    auto other_group = peerInfo("peer-b", "10.0.0.2");
    other_group.serverGroupId = self_group + 1;
    ClusterManagerTestPeer::seed(manager, "peer-b", other_group, "http://10.0.0.2:8080", 2);

    auto self_info = peerInfo(self_id, "10.0.0.3");
    self_info.serverGroupId = self_group;
    ClusterManagerTestPeer::seed(manager, self_id, self_info, "http://10.0.0.3:8080", 3);

    auto unhealthy = peerInfo("peer-c", "10.0.0.4");
    unhealthy.serverGroupId = self_group;
    ClusterManagerTestPeer::seed(manager, "peer-c", unhealthy, "http://10.0.0.4:8080", 4);
    ClusterManagerTestPeer::applyResult(manager, "peer-c", "http://10.0.0.4:8080", 4, "timeout", -1);

    auto peers = manager.getSyncPeerUrls();
    ASSERT_EQ(1u, peers.size());
    EXPECT_EQ("http://10.0.0.1:8080", peers["peer-a"]);
}

TEST_F(ClusterManagerRevisionTest, GroupChangeAppliesToSyncPeersWithoutNewHealthCheck) {
    auto &manager = ClusterManager::Instance();
    const int self_group = mINI::Instance()[Cluster::kServerLocationId];

    auto info = peerInfo("peer-a", "10.0.0.1");
    info.serverGroupId = self_group + 1;
    ClusterManagerTestPeer::seed(manager, "peer-a", info, "http://10.0.0.1:8080", 5);
    EXPECT_TRUE(manager.getSyncPeerUrls().empty());

    info.serverGroupId = self_group;
    manager.addMediaServer("peer-a", info, true);

    EXPECT_EQ(5u, ClusterManagerTestPeer::revision(manager, "peer-a"));
    EXPECT_EQ("http://10.0.0.1:8080", manager.getSyncPeerUrls()["peer-a"]);
}

// serverLocationId 0 = standalone server: it has no sync group, servers with group 0 are not its peers.
TEST_F(ClusterManagerRevisionTest, StandaloneServerHasNoSyncPeers) {
    auto &manager = ClusterManager::Instance();
    const int self_group = mINI::Instance()[Cluster::kServerLocationId];
    auto set_group = [](int group) {
        mINI::Instance()[Cluster::kServerLocationId] = group;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    };

    auto zero_group = peerInfo("peer-a", "10.0.0.1");
    zero_group.serverGroupId = 0;
    ClusterManagerTestPeer::seed(manager, "peer-a", zero_group, "http://10.0.0.1:8080", 1);
    auto group_one = peerInfo("peer-b", "10.0.0.2");
    group_one.serverGroupId = 1;
    ClusterManagerTestPeer::seed(manager, "peer-b", group_one, "http://10.0.0.2:8080", 2);

    set_group(0);
    EXPECT_FALSE(ClusterManager::inSyncGroup());
    EXPECT_FALSE(ClusterManager::isSyncGroup(0));
    EXPECT_TRUE(manager.getSyncPeerUrls().empty());

    set_group(1);
    EXPECT_TRUE(ClusterManager::inSyncGroup());
    EXPECT_TRUE(ClusterManager::isSyncGroup(1));
    auto peers = manager.getSyncPeerUrls();
    ASSERT_EQ(1u, peers.size());
    EXPECT_EQ("http://10.0.0.2:8080", peers["peer-b"]);
    set_group(self_group);
}

TEST(MediaServerInfoJson, ServerGroupSurvivesSaveAndLoad) {
    auto info = peerInfo("peer-a", "10.0.0.1");
    info.serverGroupId = 7;

    EXPECT_TRUE(MediaServerInfo::fromJson(info.toJson()) == info);

    auto old_format = info.toJson();
    old_format.removeMember("serverGroupId");
    EXPECT_EQ(0, MediaServerInfo::fromJson(old_format).serverGroupId);
}

#ifdef ENABLE_SYNC_CHANGES
TEST_F(ClusterManagerRevisionTest, SyncPeersFollowClusterAndKeepStateWhileUnreachable) {
    auto &manager = ClusterManager::Instance();
    auto &sync = SyncManager::Instance();
    const int self_group = mINI::Instance()[Cluster::kServerLocationId];

    auto same_group = peerInfo("peer-a", "10.0.0.1");
    same_group.serverGroupId = self_group;
    ClusterManagerTestPeer::seed(manager, "peer-a", same_group, "http://10.0.0.1:8080", 1);
    auto other_group = peerInfo("peer-b", "10.0.0.2");
    other_group.serverGroupId = self_group + 1;
    ClusterManagerTestPeer::seed(manager, "peer-b", other_group, "http://10.0.0.2:8080", 2);
    SyncManagerTestPeer::markPullComplete(sync, "peer-a", 100);
    SyncManagerTestPeer::markPullComplete(sync, "peer-b", 200);

    auto view = sync.healthView();
    ASSERT_EQ(1u, view.last_complete_pull_ms.size());
    EXPECT_EQ(100u, view.last_complete_pull_ms["peer-a"]);

    ClusterManagerTestPeer::applyResult(manager, "peer-a", "http://10.0.0.1:8080", 1, "timeout", -1);
    EXPECT_TRUE(sync.healthView().last_complete_pull_ms.empty());

    ClusterManagerTestPeer::applyResult(manager, "peer-a", "http://10.0.0.1:8080", 1, "", 0);
    view = sync.healthView();
    ASSERT_EQ(1u, view.last_complete_pull_ms.size());
    EXPECT_EQ(100u, view.last_complete_pull_ms["peer-a"]);
}
#endif

#ifdef ENABLE_SYNC_CHANGES
const HealthReason *findReason(const HealthEvaluation &eval, const string &metric) {
    for (const auto &r : eval.reasons) {
        if (r.metric == metric) {
            return &r;
        }
    }
    return nullptr;
}

// A decommissioned node has its sync stopped (onServerNotQualified, or not started at start-up): the
// critical reason must still be reported.
TEST(HealthMonitorClusterTest, DecommissionedIsCriticalWhileSyncIsStopped) {
    ClusterManagerTestPeer::reset(ClusterManager::Instance());
    auto &sync = SyncManager::Instance();
    ASSERT_FALSE(sync.isRunning());
    sync.setDecommissioned(true);
    Json::Value summary;
    auto eval = HealthMonitorTestPeer::collectCluster(summary);
    sync.setDecommissioned(false);

    const auto &st = summary["cluster"]["sync"];
    EXPECT_FALSE(st["enabled"].asBool());
    EXPECT_TRUE(st["decommissioned"].asBool());
    EXPECT_EQ("decommissioned", st["state"].asString());
    auto reason = findReason(eval, "cluster.decommissioned");
    ASSERT_NE(nullptr, reason);
    EXPECT_EQ(HealthLevel::CRITICAL, reason->level);

    Json::Value back;
    eval = HealthMonitorTestPeer::collectCluster(back);
    EXPECT_EQ(nullptr, findReason(eval, "cluster.decommissioned"));
    EXPECT_EQ("stopped", back["cluster"]["sync"]["state"].asString());
}

// serverLocationId 0 is a valid setup: state standalone, no group peer, nothing raised.
TEST(HealthMonitorClusterTest, StandaloneServerIsReportedWithoutReasons) {
    ClusterManagerTestPeer::reset(ClusterManager::Instance());
    const int self_group = mINI::Instance()[Cluster::kServerLocationId];
    auto set_group = [](int group) {
        mINI::Instance()[Cluster::kServerLocationId] = group;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    };
    set_group(0);
    Json::Value summary;
    auto eval = HealthMonitorTestPeer::collectCluster(summary);
    set_group(self_group);

    EXPECT_EQ("standalone", summary["cluster"]["sync"]["state"].asString());
    EXPECT_EQ(0, summary["cluster"]["group_peers"].asInt());
    for (const auto &r : eval.reasons) {
        EXPECT_NE("cluster", r.component) << r.metric;
    }
}
#endif

// pollers.cpu_max_pct is the CPU the busiest poller thread used, read from /proc, not the time the
// poller was awake (ThreadLoadCounter), which also counts the time it waits in a blocking syscall.
TEST(HealthMonitorPollerTest, CpuOfThePollerThreadsComesFromProc) {
    vector<string> names;
    EventPollerPool::Instance().for_each([&](const TaskExecutor::Ptr &executor) {
        names.emplace_back(std::static_pointer_cast<EventPoller>(executor)->getThreadName());
    });
    ASSERT_FALSE(names.empty());
    string busiest;
    HealthMonitorTestPeer::sampleThreadCpu(names, busiest); // baseline

    auto poller = EventPollerPool::Instance().getPoller();
    std::atomic<bool> done(false);
    poller->async([&done]() {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < until) {
        }
        done = true;
    });
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    double pct = HealthMonitorTestPeer::sampleThreadCpu(names, busiest);
    EXPECT_GT(pct, 40.0);
    EXPECT_EQ(poller->getThreadName().substr(0, 15), busiest);

    // A poller asleep in a blocking call uses no CPU.
    done = false;
    poller->async([&done]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        done = true;
    });
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_LT(HealthMonitorTestPeer::sampleThreadCpu(names, busiest), 20.0);
}

} // namespace
} // namespace managerkit
