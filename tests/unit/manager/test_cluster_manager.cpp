#include <gtest/gtest.h>

#include "Server/ClusterManager.h"

using namespace std;

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

} // namespace
} // namespace managerkit
