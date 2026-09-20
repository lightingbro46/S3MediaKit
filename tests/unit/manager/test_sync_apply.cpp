#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <unordered_set>
#include <thread>
#include <unistd.h>

#include "Common/config.h"
#include "Extension/Resource.h"
#include "Extension/SyncManager.h"
#include "Storage/BookmarkIndex.h"
#include "Storage/ExtractJob.h"
#include "Storage/MigrationHistory.h"
#include "Storage/MiscData.h"
#include "Storage/SyncRowVersion.h"
#include "Storage/TransactionPeerAckLog.h"
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

    static std::vector<SyncManager::SyncGap> applyBatch(SyncManager &m, const Json::Value &rows) { return m.applyBatch(rows); }

    static void handleGaps(SyncManager &m, const std::vector<SyncManager::SyncGap> &gaps, uint64_t now_ms) {
        m.handleGaps(gaps, now_ms);
    }

    static bool needsBootstrap(SyncManager &m) { return m.needsBootstrap(); }

    static void markBootstrapped(SyncManager &m) {
        SnapshotData snap;
        snap.sequences = Json::arrayValue;
        m.markBootstrapDone(snap);
        std::lock_guard<std::mutex> lock(m._mtx);
        m._bootstrapped = true;
    }

    static bool isBootstrapped(SyncManager &m) {
        std::lock_guard<std::mutex> lock(m._mtx);
        return m._bootstrapped;
    }

    static size_t reapplyDeferred(SyncManager &m) { return m.reapplyDeferredEntries(); }

    // Simulates an older build that does not know `table`.
    static TableSyncHandler unregisterTable(SyncManager &m, const std::string &table) {
        auto handler = m._table_handlers[table];
        m._table_handlers.erase(table);
        return handler;
    }

    static void registerTable(SyncManager &m, const std::string &table, const TableSyncHandler &h) {
        m.registerTable(table, h);
    }

    static std::unordered_set<std::string> prunePeers(const std::unordered_set<std::string> &healthy,
                                                      const std::vector<std::string> &known,
                                                      const std::unordered_map<std::string, int64_t> &last_ack,
                                                      int64_t now_sec, int64_t ttl_sec) {
        return SyncManager::selectPruneWatermarkPeers(healthy, known, last_ack, now_sec, ttl_sec);
    }

    static void applySnapshot(SyncManager &m, const SnapshotData &snap) { m.applySnapshot(snap); }

    static void requestResync(SyncManager &m, const std::string &reason) { m.requestResync(reason); }

    static SyncManager::BootstrapResult bootstrapResponse(SyncManager &m, const Json::Value &data) {
        auto result = std::make_shared<SyncManager::BootstrapResult>(SyncManager::BootstrapResult::Failed);
        m.onBootstrapResponse("node-bootstrap-src", "", data, [result](SyncManager::BootstrapResult r) { *result = r; });
        return *result;
    }
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

Json::Value makeRowFrom(const std::string &peer, const std::string &db, int seq, const std::string &tran_guid,
                        const std::string &table, const std::string &op, const Json::Value &payload, int64_t ts_ms);

Json::Value makeRow(int seq, const std::string &tran_guid, const std::string &table,
                    const std::string &op, const Json::Value &payload, int64_t ts_ms) {
    return makeRowFrom(kRemotePeer, kRemoteDb, seq, tran_guid, table, op, payload, ts_ms);
}

Json::Value makeRowFrom(const std::string &peer, const std::string &db, int seq, const std::string &tran_guid,
                        const std::string &table, const std::string &op, const Json::Value &payload, int64_t ts_ms) {
    Json::Value tran_data;
    tran_data["table"]   = table;
    tran_data["op"]      = op;
    tran_data["payload"] = payload;

    Json::Value row;
    row["peer_guid"]    = peer;
    row["db_guid"]      = db;
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
    auto txn = imp->getExecutor()->beginWriteTxn();
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

// Regression: applyBatch used a deferred transaction that reads before it writes. If another
// connection committed in between, the write failed immediately with "database is locked"
// (SQLITE_BUSY_SNAPSHOT) instead of waiting for busy_timeout.
TEST_F(SyncApplyTest, BatchWaitsForConcurrentWriter) {
    const std::string table = EntityTraits<VmsResourceStatus>::tableName();
    const int base = remoteCursor();
    Json::Value status;
    status["guid"]   = "cam-lock";
    status["status"] = 2;
    Json::Value rows = Json::arrayValue;
    rows.append(makeRow(base + 1, makeUuidStr(), table, TRAN_DATA_OP_UPSERT, status, 5000000));

    // Another connection holds the write lock and commits a change while the batch is starting.
    auto writer_ready = std::make_shared<std::promise<void>>();
    auto ready_future = writer_ready->get_future();
    std::thread writer([writer_ready]() {
        auto executor = std::make_shared<SqliteQueryExecutor>(Database::kEdgeStorageControllerDb);
        auto txn = executor->beginWriteTxn();
        txn->execScript("INSERT INTO misc_data (key, data) VALUES ('sync_lock_test', '1');");
        writer_ready->set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        txn->commit();
    });
    ready_future.wait();

    EXPECT_NO_THROW(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows));
    writer.join();
    EXPECT_EQ(base + 1, remoteCursor());
    EXPECT_EQ(2, std::make_shared<VmsResourceStatusImp>()->findStatus("cam-lock"));
}

// ─── Last-writer-wins (sync_row_version) ────────────────────────────────────

namespace {

const char *kPeer1 = "node-p1";
const char *kDb1   = "db-p1";
const char *kPeer2 = "node-p2";
const char *kDb2   = "db-p2";

// Emits rows for one peer with consecutive sequence numbers after its current cursor.
class PeerFeed {
public:
    PeerFeed(const std::string &peer, const std::string &db) : _peer(peer), _db(db) {
        _seq = std::make_shared<TransactionSequenceImp>()->findSeqByPeerIdAndDbGuid(peer, db);
    }

    Json::Value row(const std::string &table, const std::string &op, const Json::Value &payload, int64_t ts_ms) {
        return makeRowFrom(_peer, _db, ++_seq, makeUuidStr(), table, op, payload, ts_ms);
    }

    int lastSeq() const { return _seq; }

private:
    std::string _peer;
    std::string _db;
    int _seq = 0;
};

const std::string kStatusTable = "vms_resource_status";
const std::string kKvTable = "vms_kvpair";

Json::Value statusPayload(const std::string &guid, int status) {
    Json::Value p;
    p["guid"] = guid;
    p["status"] = status;
    return p;
}

Json::Value guidPayload(const std::string &guid) {
    Json::Value p;
    p["guid"] = guid;
    return p;
}

Json::Value kvBatch(const std::string &resource, const std::vector<std::pair<std::string, std::string>> &items) {
    Json::Value p;
    p["resource_guid"] = resource;
    p["items"] = Json::arrayValue;
    for (const auto &kv : items) {
        Json::Value item;
        item["id"] = "";
        item["resource_guid"] = resource;
        item["name"] = kv.first;
        item["value"] = kv.second;
        p["items"].append(item);
    }
    return p;
}

std::map<std::string, std::string> kvOf(const std::string &resource) {
    std::map<std::string, std::string> ret;
    for (const auto &kv : std::make_shared<VmsKvPairImp>()->findAllKeyValue(resource)) {
        ret[kv.name] = kv.value;
    }
    return ret;
}

int statusOf(const std::string &guid) {
    return std::make_shared<VmsResourceStatusImp>()->findStatus(guid);
}

void apply(const Json::Value &row) {
    Json::Value rows = Json::arrayValue;
    rows.append(row);
    SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows);
}

int auditFlag(const std::string &peer, const std::string &db, int seq) {
    auto logs = std::make_shared<TransactionLogImp>()->findSinceSeq(peer, db, seq - 1, 1);
    return logs.empty() ? -1 : logs[0].timestamp_hi;
}

int64_t nowMs() {
    return static_cast<int64_t>(toolkit::getCurrentMillisecond(true));
}

} // namespace

// Regression: the in-memory LWW map only knew remote writes, so an older remote write could
// overwrite a newer local one.
TEST_F(SyncApplyTest, StaleRemoteUpsertLosesToNewerLocalWrite) {
    VmsResourceStatus local;
    local.guid = "lww-local";
    local.status = 2;
    std::make_shared<VmsResourceStatusImp>()->add(local);

    PeerFeed p1(kPeer1, kDb1);
    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-local", 1), nowMs() - 60000));
    EXPECT_EQ(2, statusOf("lww-local"));
    EXPECT_EQ(2, auditFlag(kPeer1, kDb1, p1.lastSeq())); // recorded as stale

    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-local", 3), nowMs() + 60000));
    EXPECT_EQ(3, statusOf("lww-local"));
    EXPECT_EQ(1, auditFlag(kPeer1, kDb1, p1.lastSeq())); // conflict winner
}

// Regression: UPSERT_BATCH skipped LWW, so the last batch to arrive won for every item.
TEST_F(SyncApplyTest, KvBatchResolvesPerItemRegardlessOfOrder) {
    PeerFeed p1(kPeer1, kDb1);
    PeerFeed p2(kPeer2, kDb2);
    const std::vector<std::pair<std::string, std::string>> older = {{"a", "p1"}, {"b", "p1"}};
    const std::vector<std::pair<std::string, std::string>> newer = {{"a", "p2"}, {"c", "p2"}};

    // Resource 1: newer batch first. Resource 2: older batch first.
    apply(p2.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-kv-1", newer), 2000));
    apply(p1.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-kv-1", older), 1000));
    apply(p1.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-kv-2", older), 1000));
    apply(p2.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-kv-2", newer), 2000));

    std::map<std::string, std::string> expected = {{"a", "p2"}, {"b", "p1"}, {"c", "p2"}};
    EXPECT_EQ(expected, kvOf("lww-kv-1"));
    EXPECT_EQ(expected, kvOf("lww-kv-2"));
}

TEST_F(SyncApplyTest, EqualTimestampsAreResolvedByPeerGuid) {
    PeerFeed p1(kPeer1, kDb1);
    PeerFeed p2(kPeer2, kDb2);
    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-tie-1", 1), 5000));
    apply(p2.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-tie-1", 2), 5000));
    apply(p2.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-tie-2", 2), 5000));
    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-tie-2", 1), 5000));

    // "node-p2" > "node-p1": the node-p2 write wins in both orders.
    EXPECT_EQ(2, statusOf("lww-tie-1"));
    EXPECT_EQ(2, statusOf("lww-tie-2"));
}

// Regression: DELETE was always applied and left no trace, so a stale upsert resurrected the row
// and a stale delete removed a newer one.
TEST_F(SyncApplyTest, DeleteTombstoneBlocksOlderUpsert) {
    PeerFeed p1(kPeer1, kDb1);
    PeerFeed p2(kPeer2, kDb2);
    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-del", 1), 100));
    apply(p2.row(kStatusTable, TRAN_DATA_OP_DELETE, guidPayload("lww-del"), 300));
    EXPECT_EQ(0, statusOf("lww-del"));

    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-del", 2), 200));
    EXPECT_EQ(0, statusOf("lww-del")) << "stale upsert resurrected a deleted row";

    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-del", 3), 400));
    EXPECT_EQ(3, statusOf("lww-del"));

    apply(p2.row(kStatusTable, TRAN_DATA_OP_DELETE, guidPayload("lww-del"), 350));
    EXPECT_EQ(3, statusOf("lww-del")) << "stale delete removed a newer row";
}

TEST_F(SyncApplyTest, GroupDeleteKeepsItemsWrittenAfterIt) {
    PeerFeed p1(kPeer1, kDb1);
    PeerFeed p2(kPeer2, kDb2);
    apply(p1.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-group", {{"a", "1"}, {"b", "1"}}), 100));
    apply(p2.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-group", {{"b", "2"}}), 300));

    Json::Value del;
    del["resource_guid"] = "lww-group";
    apply(p1.row(kKvTable, TRAN_DATA_OP_DELETE, del, 200));

    std::map<std::string, std::string> expected = {{"b", "2"}};
    EXPECT_EQ(expected, kvOf("lww-group"));

    // An item older than the group delete must not come back.
    apply(p1.row(kKvTable, TRAN_DATA_OP_UPSERT_BATCH, kvBatch("lww-group", {{"a", "old"}}), 150));
    EXPECT_EQ(expected, kvOf("lww-group"));
}

// A node whose clock lags behind an accepted remote version still produces a winning local write.
TEST_F(SyncApplyTest, LocalWriteIsStampedAfterKnownVersion) {
    PeerFeed p1(kPeer1, kDb1);
    const int64_t future = nowMs() + 10 * 60 * 1000;
    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("lww-clock", 1), future));

    VmsResourceStatus local;
    local.guid = "lww-clock";
    local.status = 2;
    std::make_shared<VmsResourceStatusImp>()->add(local);

    auto &rm = ResourceManager::Instance();
    int own = std::make_shared<TransactionSequenceImp>()->findSeqByPeerIdAndDbGuid(rm.getSelfNodeId(), rm.getSelfDbGuid());
    auto logs = std::make_shared<TransactionLogImp>()->findSinceSeq(rm.getSelfNodeId(), rm.getSelfDbGuid(), own - 1, 1);
    ASSERT_EQ(1u, logs.size());
    EXPECT_GT(logs[0].timestamp, future);
}

TEST_F(SyncApplyTest, SnapshotAppliesRowVersionsAndKeepsNewerLocalRows) {
    VmsResourceStatus local;
    local.guid = "snap-local";
    local.status = 2;
    std::make_shared<VmsResourceStatusImp>()->add(local);

    SnapshotData snap;
    snap.peer_id = kPeer1;
    snap.db_guid = kDb1;
    snap.sequences           = Json::arrayValue;
    snap.vms_resource        = Json::arrayValue;
    snap.vms_kvpair          = Json::arrayValue;
    snap.vms_resource_status = Json::arrayValue;
    snap.resource_assignment = Json::arrayValue;
    snap.bookmark_index      = Json::arrayValue;
    snap.row_versions        = Json::arrayValue;
    snap.vms_resource_status.append(statusPayload("snap-local", 1));
    snap.vms_resource_status.append(statusPayload("snap-new", 3));

    SyncRowVersion old_version;
    old_version.table_name = kStatusTable;
    old_version.row_key = "snap-local";
    old_version.ts_ms = 1;
    old_version.peer_guid = kPeer1;
    SyncRowVersion new_version = old_version;
    new_version.row_key = "snap-new";
    new_version.ts_ms = 7;
    SyncRowVersion tombstone = old_version;
    tombstone.row_key = "snap-gone";
    tombstone.ts_ms = 9;
    tombstone.deleted = 1;
    snap.row_versions.append(old_version.toJson());
    snap.row_versions.append(new_version.toJson());
    snap.row_versions.append(tombstone.toJson());

    // Row versions survive the wire format.
    auto decoded = SnapshotBuilder::deserialize(SnapshotBuilder::serialize(snap));
    ASSERT_TRUE(decoded.row_versions.isArray());
    ASSERT_EQ(3u, decoded.row_versions.size());

    SyncManagerTestPeer::applySnapshot(SyncManager::Instance(), decoded);
    EXPECT_EQ(2, statusOf("snap-local")) << "snapshot overwrote a newer local row";
    EXPECT_EQ(3, statusOf("snap-new"));

    // The snapshot tombstone blocks a stale upsert received later.
    PeerFeed p2(kPeer2, kDb2);
    apply(p2.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("snap-gone", 5), 5));
    EXPECT_EQ(0, statusOf("snap-gone"));
}

// ─── Gaps, deferred entries, prune watermark (phase 4) ─────────────────────

namespace {

Json::Value rowAt(const std::string &peer, const std::string &db, int seq, const std::string &guid, int status) {
    return makeRowFrom(peer, db, seq, makeUuidStr(), kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload(guid, status),
                       1000 + seq);
}

int cursorOf(const std::string &peer, const std::string &db) {
    return std::make_shared<TransactionSequenceImp>()->findSeqByPeerIdAndDbGuid(peer, db);
}

} // namespace

// Regression: a relay whose log was pruned returned entries after the cursor with a hole; applying
// them moved the cursor past the missing entries, which were then lost silently.
TEST_F(SyncApplyTest, EntriesAfterGapAreNotApplied) {
    const std::string peer = "node-gap", db = "db-gap";
    Json::Value rows = Json::arrayValue;
    rows.append(rowAt(peer, db, 2, "gap-2", 2));
    auto gaps = SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows);
    ASSERT_EQ(1u, gaps.size());
    EXPECT_EQ(0, gaps[0].cursor);
    EXPECT_EQ(2, gaps[0].first_seq);
    EXPECT_EQ(0, statusOf("gap-2"));
    EXPECT_EQ(0, cursorOf(peer, db));

    // A relay that still has the full history fills the hole.
    Json::Value full = Json::arrayValue;
    full.append(rowAt(peer, db, 1, "gap-1", 1));
    full.append(rowAt(peer, db, 2, "gap-2", 2));
    EXPECT_TRUE(SyncManagerTestPeer::applyBatch(SyncManager::Instance(), full).empty());
    EXPECT_EQ(1, statusOf("gap-1"));
    EXPECT_EQ(2, statusOf("gap-2"));
    EXPECT_EQ(2, cursorOf(peer, db));
}

TEST_F(SyncApplyTest, HoleInsideBatchStopsThatSourceOnly) {
    const std::string peer = "node-hole", db = "db-hole";
    const std::string other = "node-hole-ok", other_db = "db-hole-ok";
    Json::Value rows = Json::arrayValue;
    rows.append(rowAt(peer, db, 1, "hole-1", 1));
    rows.append(rowAt(peer, db, 3, "hole-3", 3));   // seq 2 missing
    rows.append(rowAt(peer, db, 4, "hole-4", 4));
    rows.append(rowAt(other, other_db, 1, "hole-other", 1));
    auto gaps = SyncManagerTestPeer::applyBatch(SyncManager::Instance(), rows);
    ASSERT_EQ(1u, gaps.size());
    EXPECT_EQ(1, gaps[0].cursor);
    EXPECT_EQ(1, statusOf("hole-1"));
    EXPECT_EQ(0, statusOf("hole-3"));
    EXPECT_EQ(0, statusOf("hole-4"));
    EXPECT_EQ(1, cursorOf(peer, db));
    EXPECT_EQ(1, statusOf("hole-other"));
    EXPECT_EQ(1, cursorOf(other, other_db));
}

TEST_F(SyncApplyTest, PersistentGapRequestsResync) {
    auto &m = SyncManager::Instance();
    SyncManagerTestPeer::markBootstrapped(m);
    ASSERT_FALSE(SyncManagerTestPeer::needsBootstrap(m));

    SyncManager::SyncGap gap;
    gap.peer_guid = "node-stuck";
    gap.db_guid = "db-stuck";
    gap.cursor = 5;
    gap.first_seq = 9;
    const uint64_t t0 = 1000000;
    const uint64_t delay = static_cast<uint64_t>(static_cast<int>(mINI::Instance()[Database::kPullIntervalSec])) * 3 * 1000;

    SyncManagerTestPeer::handleGaps(m, {gap}, t0);
    SyncManagerTestPeer::handleGaps(m, {gap}, t0 + delay - 1);
    EXPECT_TRUE(SyncManagerTestPeer::isBootstrapped(m)) << "resync requested too early";

    // Cursor moved (another relay filled part of the hole): the timer restarts.
    gap.cursor = 6;
    SyncManagerTestPeer::handleGaps(m, {gap}, t0 + delay);
    EXPECT_TRUE(SyncManagerTestPeer::isBootstrapped(m));

    SyncManagerTestPeer::handleGaps(m, {gap}, t0 + 2 * delay);
    EXPECT_FALSE(SyncManagerTestPeer::isBootstrapped(m));
    EXPECT_TRUE(SyncManagerTestPeer::needsBootstrap(m));

    SyncManagerTestPeer::markBootstrapped(m);
}

// Regression: entries of a table unknown to this build were dropped and not relayed, while the
// cursor moved past them, so they were lost even after upgrading.
TEST_F(SyncApplyTest, UnknownTableEntriesAreKeptAndReappliedAfterUpgrade) {
    auto &m = SyncManager::Instance();
    PeerFeed p1(kPeer1, kDb1);
    auto handler = SyncManagerTestPeer::unregisterTable(m, kStatusTable);
    apply(p1.row(kStatusTable, TRAN_DATA_OP_UPSERT, statusPayload("deferred-1", 4), nowMs()));
    EXPECT_EQ(0, statusOf("deferred-1"));
    EXPECT_EQ(p1.lastSeq(), cursorOf(kPeer1, kDb1));
    EXPECT_EQ(TRAN_AUDIT_DEFERRED, auditFlag(kPeer1, kDb1, p1.lastSeq())); // kept for relaying

    SyncManagerTestPeer::registerTable(m, kStatusTable, handler); // "upgrade"
    EXPECT_GE(SyncManagerTestPeer::reapplyDeferred(m), 1u);
    EXPECT_EQ(4, statusOf("deferred-1"));
    EXPECT_EQ(TRAN_AUDIT_APPLIED, auditFlag(kPeer1, kDb1, p1.lastSeq()));
}

// Regression: the ack key ignored src_db_guid, so after a source recreated its database the new
// acks were rejected and its log was never pruned.
TEST_F(SyncApplyTest, PeerAckIsKeptPerSourceDatabase) {
    auto imp = std::make_shared<PeerAckLogImp>();
    PeerAckLog ack;
    ack.peer_guid = "acker";
    ack.db_guid = "acker-db";
    ack.src_peer_guid = "src";
    ack.src_db_guid = "src-db-old";
    ack.acked_seq = 50;
    ack.updated_at = 1000;
    imp->add(ack);

    ack.src_db_guid = "src-db-new";
    ack.acked_seq = 3;
    ack.updated_at = 1001;
    imp->add(ack);

    std::unordered_set<std::string> peers = {"acker"};
    std::map<std::string, int> watermark;
    for (const auto &s : imp->findMinAckedSeq(peers)) {
        if (s.peer_guid == "src") watermark[s.db_guid] = s.sequence;
    }
    EXPECT_EQ(50, watermark["src-db-old"]);
    EXPECT_EQ(3, watermark["src-db-new"]);

    // The acker recreated its own database: its lower ack replaces the old one.
    ack.db_guid = "acker-db-2";
    ack.acked_seq = 1;
    ack.updated_at = 1002;
    imp->add(ack);
    watermark.clear();
    for (const auto &s : imp->findMinAckedSeq(peers)) {
        if (s.peer_guid == "src") watermark[s.db_guid] = s.sequence;
    }
    EXPECT_EQ(1, watermark["src-db-new"]);

    EXPECT_EQ(1002, imp->findLastAckTimes()["acker"]);
}

// Regression: only healthy peers bounded pruning, so a peer that was briefly offline lost entries.
TEST_F(SyncApplyTest, OfflinePeerHoldsPruneWatermarkUntilTtl) {
    const int64_t now = 1000000, ttl = 7 * 24 * 3600;
    auto peers = SyncManagerTestPeer::prunePeers(
        {"healthy"},
        {"healthy", "offline-recent", "offline-old", "never-acked"},
        {{"offline-recent", now - 3600}, {"offline-old", now - ttl - 1}},
        now, ttl);
    std::unordered_set<std::string> expected = {"healthy", "offline-recent"};
    EXPECT_EQ(expected, peers);
}

// Regression: recording a stream/device disconnection used a deferred read-then-write transaction on
// the MediaServer DB and failed with "database is locked" when another connection committed meanwhile
// (seen in the 2-node run while tier storage was writing).
namespace {

// Another connection takes the write lock of `db_tag`, and commits a change after `hold_ms`,
// i.e. after the code under test has started its own transaction. Returns once the lock is held.
std::thread holdWriteLock(const std::string &db_tag, int hold_ms) {
    auto ready = std::make_shared<std::promise<void>>();
    auto ready_future = ready->get_future();
    std::thread writer([db_tag, hold_ms, ready]() {
        auto executor = std::make_shared<SqliteQueryExecutor>(db_tag);
        auto txn = executor->beginWriteTxn();
        txn->execScript("INSERT INTO migrationhistory (app_name, migration, applied) VALUES ('lock-test', 'lock-test', 'now');");
        ready->set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        txn->commit();
    });
    ready_future.wait();
    return writer;
}

} // namespace

TEST_F(SyncApplyTest, StatusGapWaitsForConcurrentWriter) {
    auto writer = holdWriteLock(Database::kMediaServerDb, 300);

    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    EXPECT_TRUE(ResourceStatusManager::Instance().onDisconnected("gap-lock-cam", "timeout", now));
    writer.join();
    EXPECT_TRUE(ResourceStatusManager::Instance().onReconnected("gap-lock-cam", now + 1));
    auto gaps = ResourceStatusManager::Instance().getResourceStatusGaps("gap-lock-cam", now - 10, now + 10);
    ASSERT_EQ(1u, gaps.size());
    EXPECT_EQ(now + 1, gaps[0].reconnect_time);
}

// Regression: createOrFind looked the job up in a deferred transaction and then failed to insert it
// ("database is locked") when another connection committed meanwhile; the job was reported FAILED.
TEST_F(SyncApplyTest, ExtractJobCreateWaitsForConcurrentWriter) {
    ExtractJob job;
    job.file_id = "lock-test-file";
    job.request_hash = "hash";
    job.camera_id = "cam";
    job.stream_id = "cam-main";
    job.format = "mp4";

    auto writer = holdWriteLock(Database::kMediaServerDb, 300);
    ExtractJob stored;
    auto result = std::make_shared<ExtractJobImp>()->createOrFind(job, stored);
    writer.join();
    EXPECT_EQ(ExtractJobRepository::CreateResult::CREATED, result);
    EXPECT_EQ(ExtractJobRepository::CreateResult::EXISTING, std::make_shared<ExtractJobImp>()->createOrFind(job, stored));
}

// A peer that has not completed its own bootstrap must not seed this node: its answer is "not
// ready", nothing is applied and this node stays pending. Old responses (no field) still apply.
TEST_F(SyncApplyTest, BootstrapSkipsPeerThatIsNotBootstrapped) {
    auto &m = SyncManager::Instance();
    SyncManagerTestPeer::requestResync(m, "test");
    ASSERT_TRUE(SyncManagerTestPeer::needsBootstrap(m));

    Json::Value not_ready;
    not_ready["code"] = 0;
    not_ready["bootstrapped"] = false;
    EXPECT_EQ(SyncManager::BootstrapResult::NotReady, SyncManagerTestPeer::bootstrapResponse(m, not_ready));
    EXPECT_TRUE(SyncManagerTestPeer::needsBootstrap(m));

    SnapshotData snap;
    snap.peer_id = "node-bootstrap-src";
    snap.db_guid = "db-bootstrap-src";
    snap.sequences = Json::arrayValue;
    snap.vms_resource = Json::arrayValue;
    snap.vms_kvpair = Json::arrayValue;
    snap.vms_resource_status = Json::arrayValue;
    snap.resource_assignment = Json::arrayValue;
    snap.bookmark_index = Json::arrayValue;
    snap.row_versions = Json::arrayValue;
    Json::Value old_format;
    old_format["code"] = 0;
    old_format["data"] = SnapshotBuilder::serialize(snap);
    EXPECT_EQ(SyncManager::BootstrapResult::Done, SyncManagerTestPeer::bootstrapResponse(m, old_format));
    EXPECT_FALSE(SyncManagerTestPeer::needsBootstrap(m));

    SyncManagerTestPeer::requestResync(m, "test");
    Json::Value ready = old_format;
    ready["bootstrapped"] = true;
    EXPECT_EQ(SyncManager::BootstrapResult::Done, SyncManagerTestPeer::bootstrapResponse(m, ready));
    EXPECT_FALSE(SyncManagerTestPeer::needsBootstrap(m));
    SyncManagerTestPeer::markBootstrapped(m);
}

TEST(SyncBootstrapSeedTest, LowestAnsweringNodeSeedsAfterEnoughRounds) {
    using Probe = SyncManager::BootstrapProbe;
    auto probe = [](int not_ready, int failed) {
        Probe p;
        p.not_ready = not_ready;
        p.failed = failed;
        return p;
    };
    const std::vector<std::string> peers = {"node-b", "node-c"};

    // Not every peer tried often enough yet.
    EXPECT_FALSE(SyncManager::shouldSeedBootstrap("node-a", peers, {{"node-b", probe(3, 0)}, {"node-c", probe(2, 0)}}, 3));
    EXPECT_FALSE(SyncManager::shouldSeedBootstrap("node-a", peers, {{"node-b", probe(3, 0)}}, 3));
    // Lowest id among the answering nodes seeds; the others wait for it.
    EXPECT_TRUE(SyncManager::shouldSeedBootstrap("node-a", peers, {{"node-b", probe(3, 0)}, {"node-c", probe(3, 0)}}, 3));
    EXPECT_FALSE(SyncManager::shouldSeedBootstrap("node-d", peers, {{"node-b", probe(3, 0)}, {"node-c", probe(3, 0)}}, 3));
    // An unreachable lower peer does not block the election; failures count as attempts.
    EXPECT_TRUE(SyncManager::shouldSeedBootstrap("node-c", {"node-a", "node-d"}, {{"node-a", probe(0, 3)}, {"node-d", probe(1, 2)}}, 3));
    // Only connection failures: nobody answered, no seed (this node may simply be isolated).
    EXPECT_FALSE(SyncManager::shouldSeedBootstrap("node-a", peers, {{"node-b", probe(0, 3)}, {"node-c", probe(0, 5)}}, 3));
    // No peers or unknown self id.
    EXPECT_FALSE(SyncManager::shouldSeedBootstrap("node-a", {}, {}, 3));
    EXPECT_FALSE(SyncManager::shouldSeedBootstrap("", peers, {{"node-b", probe(3, 0)}, {"node-c", probe(3, 0)}}, 3));
}
