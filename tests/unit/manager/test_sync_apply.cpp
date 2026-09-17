#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <unistd.h>

#include "Common/config.h"
#include "Extension/Resource.h"
#include "Extension/SyncManager.h"
#include "Storage/BookmarkIndex.h"
#include "Storage/MigrationHistory.h"
#include "Storage/MiscData.h"
#include "Storage/TransactionLog.h"
#include "Storage/TransactionSequence.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceAssignment.h"
#include "Storage/VmsResourceStatus.h"
#include "Util/mini.h"
#include "Util/util.h"

using namespace managerkit;
using namespace toolkit;

namespace managerkit {

// Test-only access to SyncManager internals (declared friend in SyncManager.h).
class SyncManagerTestPeer {
public:
    static void setup(SyncManager &m, const std::string &node_id, const std::string &db_id) {
        {
            std::lock_guard<std::mutex> lock(m._mtx);
            m._self_node_id = node_id;
            m._self_db_id   = db_id;
        }
        m.registerTable(EntityTraits<VmsResource>::tableName(),           VmsResourceImp::makeSyncHandler());
        m.registerTable(EntityTraits<VmsKvPair>::tableName(),             VmsKvPairImp::makeSyncHandler());
        m.registerTable(EntityTraits<VmsResourceStatus>::tableName(),     VmsResourceStatusImp::makeSyncHandler());
        m.registerTable(EntityTraits<VmsResourceAssignment>::tableName(), VmsResourceAssignmentImp::makeSyncHandler());
        m.registerTable(EntityTraits<BookmarkIndex>::tableName(),         BookmarkIndexImp::makeSyncHandler());
    }

    static void applyBatch(SyncManager &m, const Json::Value &rows) { m.applyBatch(rows); }

    static void applySnapshot(SyncManager &m, const SnapshotData &snap) { m.applySnapshot(snap); }
};

} // namespace managerkit

namespace {

const char *kRemotePeer = "node-remote";
const char *kRemoteDb   = "db-remote";

std::string g_db_dir;

// Creates a throwaway ESC database, applies the real migrations and seeds misc data
// before any singleton (SqlitePoolMap, ResourceManager) reads the configuration.
class SyncDbEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        char tmpl[] = "/tmp/s3mk_sync_test_XXXXXX";
        char *dir = mkdtemp(tmpl);
        ASSERT_NE(nullptr, dir);
        g_db_dir = dir;

        mINI::Instance()[Database::kDbSavePath]      = g_db_dir;
        mINI::Instance()[mediakit::General::kMediaServerId] = "node-local";

        // ResourceManager also touches the local (mserver) DB, e.g. LocalResource.
        std::make_shared<MigrationHistoryImp>(Database::kMediaServerDb)->migrate(S3_MSERVER_MIGRATION_DIR);
        auto migrate = std::make_shared<MigrationHistoryImp>(Database::kEdgeStorageControllerDb);
        migrate->migrate(S3_ESC_MIGRATION_DIR);
        std::make_shared<MiscDataImp>()->initMiscData();
    }

    void TearDown() override {
        if (!g_db_dir.empty()) {
            std::string cmd = "rm -rf '" + g_db_dir + "'";
            (void)std::system(cmd.c_str());
        }
    }
};

::testing::Environment *const g_env = ::testing::AddGlobalTestEnvironment(new SyncDbEnvironment);

Json::Value makeRow(int seq, const std::string &tran_guid, const std::string &table,
                    const std::string &op, const Json::Value &payload, int64_t ts_ms) {
    Json::Value tran_data;
    tran_data["table"]   = table;
    tran_data["op"]      = op;
    tran_data["payload"] = payload;

    Json::Value row;
    row["peer_guid"]    = kRemotePeer;
    row["db_guid"]      = kRemoteDb;
    row["sequence"]     = seq;
    row["timestamp"]    = static_cast<Json::Int64>(ts_ms);
    row["tran_guid"]    = tran_guid;
    row["tran_data"]    = Json::FastWriter().write(tran_data);
    row["tran_type"]    = static_cast<int>(TranType::DataMutation);
    row["timestamp_hi"] = 0;
    return row;
}

VmsResourceAssignment makeAssignment(const std::string &assignment_guid, const std::string &resource_guid,
                                     int64_t assigned_at, int64_t released_at) {
    VmsResourceAssignment a;
    a.assignment_guid = assignment_guid;
    a.resource_guid   = resource_guid;
    a.owner_peer_id   = kRemotePeer;
    a.owner_db_guid   = kRemoteDb;
    a.assigned_at     = assigned_at;
    a.released_at     = released_at;
    a.assign_type     = static_cast<int>(ResourceAssignType::PRIMARY);
    a.prev_peer_id    = "";
    return a;
}

int remoteCursor() {
    return std::make_shared<TransactionSequenceImp>()->findSeqByPeerIdAndDbGuid(kRemotePeer, kRemoteDb);
}

class SyncApplyTest : public ::testing::Test {
protected:
    void SetUp() override {
        SyncManagerTestPeer::setup(SyncManager::Instance(),
                                   ResourceManager::Instance().getSelfNodeId(),
                                   ResourceManager::Instance().getSelfDbGuid());
    }
};

} // namespace

// Regression: removeResource() used to call releaseResource() while holding the
// same non-recursive mutex, which dead-locked the calling thread forever.
TEST_F(SyncApplyTest, RemoveResourceDoesNotDeadlock) {
    auto &rm = ResourceManager::Instance();
    const std::string guid = "cam-deadlock";
    rm.assignResource(guid, rm.getSelfNodeId(), rm.getSelfDbGuid(), ResourceAssignType::PRIMARY);

    std::packaged_task<void()> task([&rm, guid]() { rm.removeResource(guid); });
    auto done = task.get_future();
    std::thread worker(std::move(task));
    if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        worker.detach();
        ADD_FAILURE() << "ResourceManager::removeResource did not return within 5s (deadlock)";
        std::fflush(nullptr);
        _exit(1); // the mutex stays locked; later tests would hang
    }
    worker.join();
    ASSERT_NO_THROW(done.get());

    VmsResourceAssignment last;
    ASSERT_TRUE(rm.getLastResourceAssignment(guid, last));
    EXPECT_NE(0, last.released_at);
}

// Regression: assign + release of the same assignment_guid in one batch used to
// re-INSERT the row (the existence check did not see the uncommitted insert),
// failing the whole batch on every retry.
TEST_F(SyncApplyTest, BatchInsertThenReleaseSameAssignment) {
    const std::string table = EntityTraits<VmsResourceAssignment>::tableName();
    const int base = remoteCursor();
    auto active   = makeAssignment("asg-batch", "cam-batch", 1000, 0);
    auto released = makeAssignment("asg-batch", "cam-batch", 1000, 2000);

    Json::Value rows = Json::arrayValue;
    rows.append(makeRow(base + 1, makeUuidStr(), table, TRAN_DATA_OP_UPSERT, active.toJson(), 1000000));
    rows.append(makeRow(base + 2, makeUuidStr(), table, TRAN_DATA_OP_UPSERT, released.toJson(), 2000000));

    ASSERT_NO_THROW(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows));

    auto found = std::make_shared<VmsResourceAssignmentImp>()->findLastAssignment("cam-batch");
    ASSERT_EQ(1u, found.size());
    EXPECT_EQ("asg-batch", found[0].assignment_guid);
    EXPECT_EQ(2000, found[0].released_at);
    EXPECT_EQ(base + 2, remoteCursor());

    auto log_imp = std::make_shared<TransactionLogImp>();
    EXPECT_TRUE(log_imp->existsByTranGuid(rows[0]["tran_guid"].asString()));
    EXPECT_TRUE(log_imp->existsByTranGuid(rows[1]["tran_guid"].asString()));
}

// A failed batch must leave neither data, cursor nor LWW state behind, so that the
// same entries are applied normally when the batch is retried.
TEST_F(SyncApplyTest, RolledBackBatchIsAppliedOnRetry) {
    const std::string table = EntityTraits<VmsResourceAssignment>::tableName();
    const int base = remoteCursor();
    auto assign = makeAssignment("asg-retry", "cam-retry", 3000, 0);
    auto good = makeRow(base + 1, makeUuidStr(), table, TRAN_DATA_OP_UPSERT, assign.toJson(), 3000000);
    // Same (peer, db, sequence) with another tran_guid violates idx_transaction_key.
    auto conflicting = makeRow(base + 1, makeUuidStr(), table, TRAN_DATA_OP_UPSERT,
                               makeAssignment("asg-other", "cam-other", 3000, 0).toJson(), 3000001);

    Json::Value bad_batch = Json::arrayValue;
    bad_batch.append(good);
    bad_batch.append(conflicting);
    EXPECT_ANY_THROW(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), bad_batch));

    auto assign_imp = std::make_shared<VmsResourceAssignmentImp>();
    EXPECT_TRUE(assign_imp->findLastAssignment("cam-retry").empty());
    EXPECT_EQ(base, remoteCursor());

    Json::Value retry = Json::arrayValue;
    retry.append(good);
    ASSERT_NO_THROW(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), retry));

    EXPECT_EQ(1u, assign_imp->findLastAssignment("cam-retry").size());
    EXPECT_EQ(base + 1, remoteCursor());

    // The retried entry must be recorded as a normal apply, not as a stale skip (timestamp_hi=2).
    auto logs = std::make_shared<TransactionLogImp>()->findSinceSeq(kRemotePeer, kRemoteDb, base, 10);
    ASSERT_EQ(1u, logs.size());
    EXPECT_EQ(0, logs[0].timestamp_hi);
}

// The same entries delivered by several relays (gossip fanout) are applied once.
TEST_F(SyncApplyTest, DuplicateBatchIsIdempotent) {
    const std::string table = EntityTraits<VmsResourceStatus>::tableName();
    const int base = remoteCursor();
    Json::Value status;
    status["guid"]   = "cam-dup";
    status["status"] = 2;

    Json::Value rows = Json::arrayValue;
    rows.append(makeRow(base + 1, makeUuidStr(), table, TRAN_DATA_OP_UPSERT, status, 4000000));

    ASSERT_NO_THROW(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows));
    ASSERT_NO_THROW(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows));
    EXPECT_EQ(base + 1, remoteCursor());
    EXPECT_EQ(2, std::make_shared<VmsResourceStatusImp>()->findStatus("cam-dup"));
}

TEST_F(SyncApplyTest, CursorNeverMovesBackwardsInTxn) {
    auto imp = std::make_shared<TransactionSequenceImp>();
    auto txn = imp->getExecutor()->execTxn();
    TransactionSequence seq;
    seq.peer_guid = "node-cursor";
    seq.db_guid   = "db-cursor";
    seq.sequence  = 10;
    imp->addWithTxn(seq, txn);
    seq.sequence = 4;
    imp->addWithTxn(seq, txn);
    txn->commit();
    EXPECT_EQ(10, imp->findSeqByPeerIdAndDbGuid("node-cursor", "db-cursor"));
}

// A snapshot from a peer must not rewind this node's own cursor, otherwise the next
// local mutation reuses an existing (peer_guid, db_guid, sequence).
TEST_F(SyncApplyTest, SnapshotDoesNotRewindOwnCursor) {
    auto &rm = ResourceManager::Instance();
    auto log_imp = std::make_shared<TransactionLogImp>();
    Json::Value payload;
    payload["guid"] = "cam-self";
    log_imp->appendLocalDataMutation(EntityTraits<VmsResourceStatus>::tableName(), TRAN_DATA_OP_DELETE, payload);
    log_imp->appendLocalDataMutation(EntityTraits<VmsResourceStatus>::tableName(), TRAN_DATA_OP_DELETE, payload);

    auto seq_imp = std::make_shared<TransactionSequenceImp>();
    const int own = seq_imp->findSeqByPeerIdAndDbGuid(rm.getSelfNodeId(), rm.getSelfDbGuid());
    ASSERT_GE(own, 2);

    SnapshotData snap;
    snap.peer_id = kRemotePeer;
    snap.db_guid = kRemoteDb;
    snap.sequences           = Json::arrayValue;
    snap.vms_resource        = Json::arrayValue;
    snap.vms_kvpair          = Json::arrayValue;
    snap.vms_resource_status = Json::arrayValue;
    snap.resource_assignment = Json::arrayValue;
    snap.bookmark_index      = Json::arrayValue;

    TransactionSequence stale_self;
    stale_self.peer_guid = rm.getSelfNodeId();
    stale_self.db_guid   = rm.getSelfDbGuid();
    stale_self.sequence  = 1;
    snap.sequences.append(stale_self.toJson());

    TransactionSequence other;
    other.peer_guid = "node-snapshot";
    other.db_guid   = "db-snapshot";
    other.sequence  = 7;
    snap.sequences.append(other.toJson());

    SyncManagerTestPeer::applySnapshot(SyncManager::Instance(), snap);

    EXPECT_EQ(own, seq_imp->findSeqByPeerIdAndDbGuid(rm.getSelfNodeId(), rm.getSelfDbGuid()));
    EXPECT_EQ(7, seq_imp->findSeqByPeerIdAndDbGuid("node-snapshot", "db-snapshot"));
    // Next local mutation must still succeed.
    EXPECT_NO_THROW(log_imp->appendLocalDataMutation(EntityTraits<VmsResourceStatus>::tableName(), TRAN_DATA_OP_DELETE, payload));
    EXPECT_EQ(own + 1, seq_imp->findSeqByPeerIdAndDbGuid(rm.getSelfNodeId(), rm.getSelfDbGuid()));
}
