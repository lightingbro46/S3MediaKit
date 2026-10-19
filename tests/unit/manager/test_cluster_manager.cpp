#include <gtest/gtest.h>

#include "Common/config.h"
#include "Server/ClusterManager.h"
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

} // namespace
} // namespace managerkit
