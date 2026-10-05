#include "SyncManager.h"
#include "Storage/TransactionSequence.h"
#include "Storage/TransactionLog.h"
#include "Storage/TransactionPeerAckLog.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResourceAssignment.h"
#include "Storage/BookmarkIndex.h"
#include "Storage/MiscData.h"
#include "Storage/SyncRowVersion.h"
#include "Common/StrUtil.h"
#include "Common/config.h"
#include "Extension/Resource.h"
#include "Thread/WorkThreadPool.h"
#include "Server/ClusterManager.h"
#include <random>

#ifdef ENABLE_SYNC_CHANGES

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(SyncManager)

SyncManager::SyncManager() {
    // Register sync handlers for all synced tables. Done at construction so that local mutations
    // (which record LWW versions through stampLocalMutation) work before start() is called.
    // To add a new synced table: implement makeSyncHandler() on its Imp class and add one line here.
    registerTable(EntityTraits<VmsResource>::tableName(),           VmsResourceImp::makeSyncHandler());
    registerTable(EntityTraits<VmsKvPair>::tableName(),             VmsKvPairImp::makeSyncHandler());
    registerTable(EntityTraits<VmsResourceStatus>::tableName(),     VmsResourceStatusImp::makeSyncHandler());
    registerTable(EntityTraits<VmsResourceAssignment>::tableName(), VmsResourceAssignmentImp::makeSyncHandler());
    registerTable(EntityTraits<BookmarkIndex>::tableName(),         BookmarkIndexImp::makeSyncHandler());
}

// ── TransactionSequence helpers ─────────────────────────────────────────────

// Workaround for TransactionSequenceImp::add upsert: call add() for insert,
// updateByPeerGuidAndDbGuid() directly for update (avoids the now-fixed but
// still-present risk if compiled against an older header).
static void upsertCursorSeq(const string &peer_guid, const string &db_guid, int sequence) {
    auto imp = make_shared<TransactionSequenceImp>();
    TransactionSequence ts;
    ts.peer_guid = peer_guid;
    ts.db_guid   = db_guid;
    ts.sequence  = sequence;
    imp->add(ts);
}

static void insertIfNotExistsCursorSeq(const string &peer_id, const string &db_guid, int sequence) {
    auto imp = std::make_shared<TransactionSequenceImp>();
    auto existing = imp->findDbGuidByPeerId(peer_id);
    if (existing.empty()) {
        TransactionSequence ts;
        ts.peer_guid = peer_id;
        ts.db_guid   = db_guid;
        ts.sequence  = 0;
        imp->add(ts);
        InfoL << "Registered cursor for peer=" << peer_id << " db_guid=" << db_guid;
    } else {
        TraceL << "Cursor already exists for peer=" << peer_id << " db_guid=" << existing << ", skipping";
    }
}

static int findCursorSeq(const string &peer_guid, const string &db_guid) {
    auto imp = make_shared<TransactionSequenceImp>();
    return imp->findSeqByPeerIdAndDbGuid(peer_guid, db_guid);
}

static std::string findDbGuid(const string &peer_id) {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    if (peer_id != mediaServerId) {
        auto imp = make_shared<TransactionSequenceImp>();
        auto guid = imp->findDbGuidByPeerId(peer_id);
        return guid;
    } else {
        auto imp = make_shared<MiscDataImp>();
        auto ret = imp->findByKey(MISC_DATA_DB_INSTANCE_ID_KEY);
        if (ret.size() > 0) {
            return ret[0].value;
        }
        return "";
    }
}

// ─── SnapshotBuilder ────────────────────────────────────────────────────────

SnapshotData SnapshotBuilder::build(const string &peer_id,
                                    const string &db_guid,
                                    const string &db_tag) {
    SnapshotData snap;
    snap.peer_id = peer_id;
    snap.db_guid = db_guid;
    // Preserve empty tables as JSON arrays. This lets the receiver distinguish
    // a valid empty snapshot from a malformed/missing snapshot field.
    snap.sequences           = Json::arrayValue;
    snap.vms_resource        = Json::arrayValue;
    snap.vms_kvpair          = Json::arrayValue;
    snap.vms_resource_status = Json::arrayValue;
    snap.resource_assignment = Json::arrayValue;
    snap.bookmark_index      = Json::arrayValue;
    snap.row_versions        = Json::arrayValue;

    auto executor = make_shared<SqliteQueryExecutor>(db_tag);

    // Read all tables in one transaction for snapshot consistency
    auto txn = executor->beginReadTxn();
    try {
        // Step 1: transaction_sequence – receiver uses this to detect stale data
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<TransactionSequence>::getColumns())
                           .from(EntityTraits<TransactionSequence>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.sequences.append(EntityTraits<TransactionSequence>::fromRow(r).toJson());
            }
        }
        // Step 2: vms_resource
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<VmsResource>::getColumns())
                           .from(EntityTraits<VmsResource>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.vms_resource.append(EntityTraits<VmsResource>::fromRow(r).toJson());
            }
        }
        // Step 3: vms_kvpair
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<VmsKvPair>::getColumns())
                           .from(EntityTraits<VmsKvPair>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.vms_kvpair.append(EntityTraits<VmsKvPair>::fromRow(r).toJson());
            }
        }
        // Step 4: vms_resource_status
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<VmsResourceStatus>::getColumns())
                           .from(EntityTraits<VmsResourceStatus>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.vms_resource_status.append(EntityTraits<VmsResourceStatus>::fromRow(r).toJson());
            }
        }
        // Step 5: vms_resource_assignment
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<VmsResourceAssignment>::getColumns())
                           .from(EntityTraits<VmsResourceAssignment>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.resource_assignment.append(EntityTraits<VmsResourceAssignment>::fromRow(r).toJson());
            }
        }
        // Step 6: bookmark_index
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<BookmarkIndex>::getColumns())
                           .from(EntityTraits<BookmarkIndex>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.bookmark_index.append(EntityTraits<BookmarkIndex>::fromRow(r).toJson());
            }
        }
        // Step 7: sync_row_version — the receiver needs the LWW state matching the snapshot data
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<SyncRowVersion>::getColumns())
                           .from(EntityTraits<SyncRowVersion>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.row_versions.append(EntityTraits<SyncRowVersion>::fromRow(r).toJson());
            }
        }
        txn->commit();
    } catch (const std::exception &ex) {
        WarnL << "[Snapshot] build failed: " << ex.what();
        snap.sequences = Json::arrayValue; // snap.empty() == true
    }
    return snap;
}

string SnapshotBuilder::serialize(const SnapshotData &snap) {
    Json::Value root;
    root["peer_id"]            = snap.peer_id;
    root["db_guid"]             = snap.db_guid;
    root["sequences"]           = snap.sequences;
    root["vms_resource"]        = snap.vms_resource;
    root["vms_kvpair"]          = snap.vms_kvpair;
    root["vms_resource_status"] = snap.vms_resource_status;
    root["resource_assignment"] = snap.resource_assignment;
    root["bookmark_index"]      = snap.bookmark_index;
    root["row_versions"]        = snap.row_versions;
    return StrJsonUtils::writeJsonString(root);
}

SnapshotData SnapshotBuilder::deserialize(const string &json_body) {
    SnapshotData snap;
    Json::Value root;
    if (!StrJsonUtils::readJsonString(json_body, root)) {
        WarnL << "[Snapshot] deserialize: invalid JSON";
        return snap; // snap.empty() == true
    }
    snap.peer_id             = root["peer_id"].asString();
    snap.db_guid             = root["db_guid"].asString();
    snap.sequences           = root["sequences"];
    snap.vms_resource        = root["vms_resource"];
    snap.vms_kvpair          = root["vms_kvpair"];
    snap.vms_resource_status = root["vms_resource_status"];
    snap.resource_assignment = root["resource_assignment"];
    snap.bookmark_index      = root["bookmark_index"];
    snap.row_versions        = root["row_versions"]; // null for snapshots from older nodes
    return snap;
}

// ─── SyncManager ────────────────────────────────────────────────────────────

void SyncManager::start() {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    GET_CONFIG(bool,   enable_sync,   Database::kEnableSyncDb);
    GET_CONFIG(int,    interval_sec,  Database::kPullIntervalSec);

    if (!enable_sync) {
        WarnL << "Sync database opration disabled by config";
        return;
    }

    auto worker = getWorker();
    {
        lock_guard<mutex> lock(_mtx);
        _self_node_id = ResourceManager::Instance().getSelfNodeId();
        _self_db_id   = ResourceManager::Instance().getSelfDbGuid();
        _running      = true;
        _peers.clear();
        _bootstrapping.clear();
        // Peers are populated by ClusterManager::addPeer() after health checks.
        // ClusterManager::healthCheckPersistedPeers() handles re-connecting to
        // known peers on restart without SyncManager loading the list directly.
    }

    weak_ptr<SyncManager> weak_self = shared_from_this();
    float _interval_sec = interval_sec > 0 ? interval_sec * 1.0f : 30.0f; // default to 30s if config is invalid
    _timer = make_shared<Timer>(_interval_sec, [weak_self]() {
        auto self = weak_self.lock();
        if (self) self->onTick();
        return true;
    }, worker); // all sync DB work runs on the single sync worker, never on a network poller

    // Resume directly in pull mode when a previous bootstrap was persisted.
    if (!needsBootstrap()) {
        lock_guard<mutex> lock(_mtx);
        _bootstrapped = true;
        InfoL << "Database already populated, skipping bootstrap";
    }

    // Entries this node could not apply with an older build (unknown table/op) are retried now.
    weak_ptr<SyncManager> weak_retry = shared_from_this();
    worker->async([weak_retry]() {
        auto self = weak_retry.lock();
        if (!self) return;
        try {
            self->reapplyDeferredEntries();
        } catch (const std::exception &ex) {
            WarnL << "Failed to re-apply deferred sync entries: " << ex.what();
        }
    }, false);

    InfoL << "Sync database started, node=" << mediaServerId << ", bootstrapped=" << _bootstrapped << ", interval=" << interval_sec << "s";
}

toolkit::EventPoller::Ptr SyncManager::getWorker() {
    lock_guard<mutex> lock(_mtx);
    if (!_worker) {
        _worker = WorkThreadPool::Instance().getPoller();
    }
    return _worker;
}

void SyncManager::registerTable(const std::string &table_name, TableSyncHandler handler) {
    _table_handlers[table_name] = std::move(handler);
}

void SyncManager::stop() {
    lock_guard<mutex> lock(_mtx);
    _running      = false;
    _bootstrapped = false;
    _timer.reset();
    _peers.clear();
    _bootstrapping.clear();
    _last_bootstrap_peer.clear();
    _bootstrap_probes.clear();
    InfoL << "Sync manager has been stopped";
}

void SyncManager::addPeer(const string &peer_id, const string &base_url) {
    {
        lock_guard<mutex> lock(_mtx);
        if (peer_id == _self_node_id) {
            WarnL << "Ignore adding self to peer list";
            return;
        }
        _peers[peer_id] = base_url;
    }
    InfoL << "Added peer: " << peer_id << " (" << base_url << ")";
}

void SyncManager::removePeer(const string &peer_id) {
    lock_guard<mutex> lock(_mtx);
    _peers.erase(peer_id);
    _bootstrapping.erase(peer_id);
    _bootstrap_probes.erase(peer_id);
    if (_last_bootstrap_peer == peer_id) {
        _last_bootstrap_peer.clear();
    }
    InfoL << "Removed peer: " << peer_id;
    // Note: Must NOT delete transaction_sequence entries for the removed peer, because:
    // 1) If the peer is re-added later, the old cursor is still valid and can be resumed from.
    // 2) If the peer is permanently removed, the transaction_sequence entries for that peer will 
    // be orphaned but harmless (they won't match any incoming peer_id) and can be ignored or cleaned up manually if desired.
}

bool SyncManager::isBootstrapped() {
    lock_guard<mutex> lock(_mtx);
    return _bootstrapped;
}

SnapshotData SyncManager::buildLocalSnapshot() {
    string node_id, db_id;
    {
        lock_guard<mutex> lock(_mtx);
        node_id = _self_node_id;
        db_id   = _self_db_id;
    }
    return SnapshotBuilder::build(node_id, db_id, Database::kEdgeStorageControllerDb);
}

// ─── Tick ────────────────────────────────────────────────────────────────────
//
// State machine:
//   !_bootstrapped → bootstrap from ONE peer (first available), then return.
//                    Subsequent ticks retry until a peer succeeds.
//    _bootstrapped → incremental pull from ALL peers every tick.

void SyncManager::onTick() {
    unordered_map<string, string> peers_copy;
    bool bootstrapped;
    bool bootstrap_in_progress;
    {
        lock_guard<mutex> lock(_mtx);
        if (!_running) return;
        peers_copy           = _peers;
        bootstrapped         = _bootstrapped;
        bootstrap_in_progress = !_bootstrapping.empty();
    }

    if (!peers_copy.size()) {
        WarnL << "No peers to sync with";
        return;
    }

    if (!bootstrapped) {
        // ── Bootstrap phase: try exactly ONE peer ──────────────────────────
        if (bootstrap_in_progress) {
            return; // already waiting for an in-flight bootstrap response
        }

        // Only a peer that completed its own bootstrap may serve the snapshot. When no peer has
        // (a new cluster starting at once), the lowest node id among the answering ones seeds it.
        {
            GET_CONFIG(int, seed_rounds, Database::kBootstrapSeedRounds);
            string self_id;
            vector<string> peer_ids;
            unordered_map<string, BootstrapProbe> probes;
            {
                lock_guard<mutex> lock(_mtx);
                self_id = _self_node_id;
                probes = _bootstrap_probes;
            }
            for (const auto &p : peers_copy) {
                peer_ids.push_back(p.first);
            }
            if (shouldSeedBootstrap(self_id, peer_ids, probes, seed_rounds > 0 ? seed_rounds : 3)) {
                InfoL << "No peer has completed bootstrap after " << seed_rounds
                      << " rounds; seeding the cluster database from this node (" << self_id << ")";
                markBootstrapSeeded(self_id);
                lock_guard<mutex> lock(_mtx);
                _bootstrapped = true;
                _last_bootstrap_peer.clear();
                _bootstrap_probes.clear();
                return;
            }
        }

        // Sort peer IDs for deterministic rotation order
        vector<string> peer_keys;
        for (const auto &p : peers_copy) {
            peer_keys.push_back(p.first);
        }
        sort(peer_keys.begin(), peer_keys.end());

        string target_id, target_url;
        {
            lock_guard<mutex> lock(_mtx);
            // Find the peer AFTER the last failed one (wrap around if needed)
            bool seen_last = _last_bootstrap_peer.empty();
            for (size_t i = 0; i < peer_keys.size(); ++i) {
                const string &key = peer_keys[i];
                if (!seen_last) {
                    if (key == _last_bootstrap_peer) seen_last = true;
                    continue;
                }
                if (_peers.count(key)) {
                    target_id  = key;
                    target_url = _peers.at(key);
                    break;
                }
            }
            // Wrap around: no peer found after last → restart from beginning
            if (target_id.empty() && !peer_keys.empty()) {
                const string &key = peer_keys[0];
                if (_peers.count(key)) {
                    target_id  = key;
                    target_url = _peers.at(key);
                }
            }
            if (!target_id.empty()) {
                _bootstrapping.insert(target_id);
                _last_bootstrap_peer = target_id; // remember for next failure
            }
        }

        if (target_id.empty()) return; // no peers available
        InfoL << "Need bootstrap from peer " << target_id << " (rotating)";
        weak_ptr<SyncManager> weak_self = shared_from_this();
        doBootstrap(target_id, target_url, [weak_self, target_id](BootstrapResult result) {
            auto self = weak_self.lock();
            if (!self) return;
            {
                lock_guard<mutex> lock(self->_mtx);
                self->_bootstrapping.erase(target_id);
                if (result == BootstrapResult::Done) {
                    self->_bootstrapped = true;
                    self->_last_bootstrap_peer.clear(); // reset on success
                    self->_bootstrap_probes.clear();
                } else if (result == BootstrapResult::NotReady) {
                    ++self->_bootstrap_probes[target_id].not_ready;
                } else {
                    ++self->_bootstrap_probes[target_id].failed;
                }
                // otherwise _last_bootstrap_peer = target_id stays
                // → next tick picks the peer AFTER target_id
            }
            if (result == BootstrapResult::Done) {
                InfoL << "Bootstrap done, peer=" << target_id;
            } else if (result == BootstrapResult::NotReady) {
                InfoL << "Peer " << target_id << " has not completed bootstrap yet, will try next peer on next tick";
            } else {
                WarnL << "Bootstrap failed, peer=" << target_id << ", will try next peer on next tick";
            }
        });
        return; // don't pull yet; wait for bootstrap to complete
    }

    // ── Pull phase: gossip relay ───────────────────────────────────────────
    // Every bootstrapped node pulls from ALL configured peers using the full
    // local cursor map. Each peer returns log entries for all peers it has
    // relayed, so after 1-2 ticks every node converges to the same state.
    // No mini-bootstrap needed: new peer discovery is implicit via relay data.
    GET_CONFIG(int, fanout, Database::kGossipFanout);
    vector<pair<string,string>> peers_vec(peers_copy.begin(), peers_copy.end());

    // Shuffle to choose random peers when fanout is enabled (fanout=0 means all peers). This also helps distribute load when multiple nodes start simultaneously.
    static std::mt19937 rng(std::random_device{}());
    std::shuffle(peers_vec.begin(), peers_vec.end(), rng);
    int n = (fanout > 0) ? min((int)peers_vec.size(), fanout) : (int)peers_vec.size();
    for (int i = 0; i < n; ++i) {
        const auto &pair = peers_vec[i];
        pullFromRelay(pair.first, pair.second);
    }
    // Periodically prune logs every hour (only if there are peers, to avoid pruning healthy logs on a node that's temporarily isolated)
    if (_last_prune_time == 0 || time(nullptr) - _last_prune_time >= 3600) {
        maybePruneLog();
        _last_prune_time = time(nullptr);
    }
}

// ─── Bootstrap ───────────────────────────────────────────────────────────────

bool SyncManager::needsBootstrap() {
    auto imp = make_shared<MiscDataImp>();
    auto ret = imp->findByKey(MISC_DATA_DB_BOOTSTRAP_DONE);
    if (ret.empty()) return true;

    // Keep accepting the old value during development so an existing local DB
    // does not unexpectedly bootstrap again after upgrading this code.
    if (ret[0].value == "1") return false;

    Json::Value state;
    if (!StrJsonUtils::readJsonString(ret[0].value, state) || !state.isObject()) {
        WarnL << "Invalid persisted bootstrap state; bootstrap will run again";
        return true;
    }
    return state["status"].asString() != "completed";
}

void SyncManager::markBootstrapDone(const SnapshotData &snap) {
    Json::Value counts;
    counts["transaction_sequence"]   = static_cast<Json::UInt64>(snap.sequences.size());
    counts["vms_resource"]           = static_cast<Json::UInt64>(snap.vms_resource.size());
    counts["vms_kvpair"]             = static_cast<Json::UInt64>(snap.vms_kvpair.size());
    counts["vms_resource_assignment"] = static_cast<Json::UInt64>(snap.resource_assignment.size());
    counts["bookmark_index"]         = static_cast<Json::UInt64>(snap.bookmark_index.size());
    counts["sync_row_version"]       = static_cast<Json::UInt64>(snap.row_versions.size());

    Json::Value state;
    state["schema_version"]    = 1;
    state["status"]            = "completed";
    state["source_peer_id"]    = snap.peer_id;
    state["source_db_guid"]    = snap.db_guid;
    state["completed_at_ms"]   = static_cast<Json::Int64>(toolkit::getCurrentMillisecond(true));
    state["snapshot_row_counts"] = counts;

    auto imp = make_shared<MiscDataImp>();
    MiscData data;
    data.key = MISC_DATA_DB_BOOTSTRAP_DONE;
    data.value = StrJsonUtils::writeJsonString(state);
    imp->add(data);
}

void SyncManager::markBootstrapSeeded(const string &self_id) {
    Json::Value state;
    state["schema_version"]  = 1;
    state["status"]          = "completed";
    state["seeded"]          = true;
    state["source_peer_id"]  = self_id;
    state["completed_at_ms"] = static_cast<Json::Int64>(toolkit::getCurrentMillisecond(true));

    auto imp = make_shared<MiscDataImp>();
    MiscData data;
    data.key = MISC_DATA_DB_BOOTSTRAP_DONE;
    data.value = StrJsonUtils::writeJsonString(state);
    imp->add(data);
}

bool SyncManager::shouldSeedBootstrap(const string &self_id, const vector<string> &peers,
                                      const unordered_map<string, BootstrapProbe> &probes, int rounds) {
    if (self_id.empty() || peers.empty()) {
        return false;
    }
    bool any_not_ready = false;
    for (const auto &peer : peers) {
        auto it = probes.find(peer);
        if (it == probes.end() || it->second.not_ready + it->second.failed < rounds) {
            return false; // not every peer has been asked often enough yet
        }
        if (it->second.not_ready > 0) {
            any_not_ready = true;
            if (peer < self_id) {
                return false; // a lower answering peer is the seed
            }
        }
    }
    return any_not_ready;
}

void SyncManager::doBootstrap(const string &peer_id, const string &base_url, DoneCb cb) {
    InfoL << "Bootstrapping from " << peer_id << " (base_url=" << base_url << ")";

    weak_ptr<SyncManager> weak_self = shared_from_this();
    Broadcast::OnResInvoker onRes = [weak_self, peer_id, cb](const string &err, const int &idx, const Json::Value &data) {
        auto self = weak_self.lock();
        if (!self) return;
        // HTTP callbacks run on a network poller; applying the snapshot is blocking SQLite work.
        self->getWorker()->async([weak_self, peer_id, cb, err, data]() {
            auto self = weak_self.lock();
            if (!self) return;
            self->onBootstrapResponse(peer_id, err, data, cb);
        }, false);
    };

    auto flag = NOTICE_EMIT(BroadcastSyncSnapshotArgs, Broadcast::kBroadcastSyncSnapshot, base_url, peer_id, onRes);
    if (!flag) {
        WarnL << "No listener for kBroadcastSyncSnapshot, cannot bootstrap from " << peer_id;
        if (cb) cb(BootstrapResult::Failed);
    }
}

void SyncManager::onBootstrapResponse(const string &peer_id, const string &err, const Json::Value &data, const DoneCb &cb) {
    if (!err.empty()) {
        WarnL << "Bootstrap request to " << peer_id << " failed: " << err;
        if (cb) cb(BootstrapResult::Failed);
        return;
    }
    // A peer that has not completed its own bootstrap answers without a snapshot; its data may be
    // empty or partial. Responses of older builds have no such field and are complete.
    if (data.isMember("bootstrapped") && !data["bootstrapped"].asBool()) {
        if (cb) cb(BootstrapResult::NotReady);
        return;
    }
    try {
        auto snap = SnapshotBuilder::deserialize(data["data"].asString());
        if (snap.peer_id.empty() || snap.db_guid.empty() ||
            !snap.sequences.isArray() || !snap.vms_resource.isArray() ||
            !snap.vms_kvpair.isArray() || !snap.resource_assignment.isArray() ||
            !snap.vms_resource_status.isArray() ||
            !snap.bookmark_index.isArray()) {
            throw std::runtime_error("bootstrap response contains an invalid snapshot");
        }
        applySnapshot(snap);
        markBootstrapDone(snap);
        InfoL << "Bootstrap from " << peer_id << " done:"
              << " peer_id=" << snap.peer_id
              << " db_guid=" << snap.db_guid
              << " seq=" << snap.sequences.size()
              << " resource=" << snap.vms_resource.size()
              << " kv="  << snap.vms_kvpair.size();
        if (cb) cb(BootstrapResult::Done);
    } catch (std::exception &ex) {
        WarnL << "Failed to apply snapshot from " << peer_id << ": " << ex.what();
        if (cb) cb(BootstrapResult::Failed);
    }
}

// Mini-bootstrap removed: peer discovery in gossip relay is implicit.
// When a node receives log entries via relay, TransactionLogImp::add() automatically
// registers the source peer's cursor in transaction_sequence, so new peers are
// discovered without a dedicated bootstrap round-trip.

void SyncManager::applySnapshot(const SnapshotData &snap) {
    lock_guard<mutex> apply_lock(_apply_mtx);
    // Map named SnapshotData fields to their table names, then dispatch via registry.
    const std::pair<std::string, const Json::Value *> snap_tables[] = {
        {EntityTraits<VmsResource>::tableName(),           &snap.vms_resource},
        {EntityTraits<VmsKvPair>::tableName(),             &snap.vms_kvpair},
        {EntityTraits<VmsResourceStatus>::tableName(),     &snap.vms_resource_status},
        {EntityTraits<VmsResourceAssignment>::tableName(), &snap.resource_assignment},
        {EntityTraits<BookmarkIndex>::tableName(),         &snap.bookmark_index},
    };

    // Snapshot versions, keyed by table + row key (absent in snapshots from older nodes).
    unordered_map<string, SyncRowVersion> snap_versions;
    if (snap.row_versions.isArray()) {
        for (const auto &v : snap.row_versions) {
            auto version = SyncRowVersion::fromJson(v);
            snap_versions[version.table_name + "\n" + version.row_key] = version;
        }
    }

    auto version_imp = make_shared<SyncRowVersionImp>();
    for (const auto &entry : snap_tables) {
        const Json::Value &arr = *entry.second;
        if (arr.empty() || !arr.isArray()) continue;
        auto hit = _table_handlers.find(entry.first);
        if (hit == _table_handlers.end() || !hit->second.onSnapshot) continue;
        const TableSyncHandler &h = hit->second;

        // A row this node already holds a version for (written locally before the bootstrap
        // finished) is kept unless the snapshot carries a newer version of it.
        Json::Value rows = Json::arrayValue;
        {
            auto txn = version_imp->getExecutor()->beginReadTxn();
            for (const auto &row : arr) {
                string key = h.rowKey ? h.rowKey(row) : string();
                SyncRowVersion local;
                if (!key.empty() && version_imp->findEffectiveWithTxn(entry.first, key, h.groupKey ? h.groupKey(row) : string(), local, txn)) {
                    auto sv = snap_versions.find(entry.first + "\n" + key);
                    if (sv == snap_versions.end() || !SyncRowVersionImp::isNewer(sv->second.ts_ms, sv->second.peer_guid, local)) {
                        TraceL << "applySnapshot keeps local row " << entry.first << ":" << key;
                        continue;
                    }
                }
                rows.append(row);
            }
            txn->commit();
        }
        if (!rows.empty()) {
            h.onSnapshot(rows);
        }
    }

    // Adopt the snapshot's LWW state (only versions newer than what is stored locally).
    if (!snap_versions.empty()) {
        auto txn = version_imp->getExecutor()->beginWriteTxn();
        for (const auto &v : snap_versions) {
            version_imp->recordWithTxn(v.second, txn);
        }
        txn->commit();
    }

    // Sequences are infrastructure state — not dispatched through the table registry.
    if (!snap.sequences.empty() && snap.sequences.isArray()) {
        string self_node, self_db;
        {
            lock_guard<mutex> lock(_mtx);
            self_node = _self_node_id;
            self_db   = _self_db_id;
        }
        auto imp = std::make_shared<TransactionSequenceImp>();
        for (const auto &v : snap.sequences) {
            auto seq = TransactionSequence::fromJson(v);
            // Never take our own cursor from a peer: a lower value would make the next
            // local mutation reuse an existing (peer_guid, db_guid, sequence).
            if (seq.peer_guid == self_node && seq.db_guid == self_db) {
                continue;
            }
            // Only move cursors forward.
            if (seq.sequence <= imp->findSeqByPeerIdAndDbGuid(seq.peer_guid, seq.db_guid)) {
                continue;
            }
            imp->add(seq);
        }
    }
}

// ─── incremental pull ──────────────────────────────────────────────────────

Json::Value SyncManager::getCurrentCursors(const Json::Value &cursors_json, int limit) {
    std::vector<TransactionSequence> cursors;
    std::unordered_set<std::string> peer_pairs;
    if (!cursors_json.empty() && cursors_json.isArray()) {
        for (const auto &c : cursors_json) {
            cursors.push_back(TransactionSequence::fromJson(c));
            peer_pairs.insert(c["peer_guid"].asString() + "_" + c["db_guid"].asString());
        }
    }

    // If there is no cursor for the peer/db pair, add a default cursor with sequence 0, 
    // otherwise the peer will never get any log because the server doesn't know the peer's cursor and thinks the peer has already got all logs
    auto seq_impl = make_shared<TransactionSequenceImp>();
    auto log_impl = make_shared<TransactionLogImp>();
    auto txn = log_impl->getExecutor()->beginReadTxn();
    Json::Value log_rows = Json::arrayValue;

    try {
        auto seq_ret = seq_impl->findAllWithTxn(txn);
        for (const auto &s : seq_ret) {
            std::string key = s.peer_guid + "_" + s.db_guid;
            if (peer_pairs.find(key) == peer_pairs.end()) {
                TransactionSequence seq;
                seq.peer_guid = s.peer_guid;
                seq.db_guid = s.db_guid;
                seq.sequence = 0;
                cursors.push_back(seq);
            }
        }  

        auto ret = log_impl->findAllSinceWithTxn(cursors, limit, txn);

        for (const TransactionLog &b : ret) {
            Json::Value item;
            item["sequence"] = b.sequence;
            item["peer_guid"] = b.peer_guid;
            item["db_guid"] = b.db_guid;
            item["timestamp"] = b.timestamp;
            item["tran_guid"] = b.tran_guid;
            item["tran_data"] = b.tran_data;
            item["tran_type"] = b.tran_type;
            item["timestamp_hi"] = b.timestamp_hi;
            log_rows.append(item);
        }
        txn->commit();
    } catch (const std::exception &ex) {
        WarnL << "Failed to get current cursors: " << ex.what();
    }

    return log_rows;
}

// ─── Gossip relay pull ────────────────────────────────────────────────────
//
// Sends the full local cursor map (peer_guid|db_guid → since_seq) to the relay
// peer. The relay returns log entries for ALL peers in its transaction_log that
// are newer than the corresponding cursor. This means:
//   • After round 1 (full-mesh pull) every node has a complete cursor map.
//   • From round 2 onwards, pulling from a single relay that already has the
//     full dataset is sufficient for convergence within 1-2 ticks.
void SyncManager::pullFromRelay(const string &relay_peer_id, const string &base_url) {
    // Build cursor map from all locally known sequences.
    auto seq_imp = make_shared<TransactionSequenceImp>();
    auto all_seqs = seq_imp->findAll();

    TraceL << "Relay pull from " << relay_peer_id << " with " << all_seqs.size() << " cursors";

    weak_ptr<SyncManager> weak_self = shared_from_this();
    Broadcast::OnResInvoker onRes = [weak_self, relay_peer_id](const string &err, const int &idx, const Json::Value &data) {
        auto self = weak_self.lock();
        if (!self) return;

        if (!err.empty()) {
            WarnL << "Relay pull from " << relay_peer_id << " failed: " << err;
            return;
        }

        const Json::Value &rows = data["data"];
        if (!rows.isArray() || rows.empty()) return;

        InfoL << "Relay received " << rows.size() << " entries from " << relay_peer_id << " (first tran_seq=" << rows[0]["sequence"].asString() << " last tran_seq=" << rows[rows.size()-1]["sequence"].asString() << ")";
        // Apply on the sync worker: blocking SQLite work must not run on the HTTP client poller,
        // and responses from several relays are applied one after another.
        Json::Value rows_copy = rows;
        self->getWorker()->async([weak_self, relay_peer_id, rows_copy]() {
            auto self = weak_self.lock();
            if (!self) return;
            try {
                self->handleGaps(self->applyBatch(rows_copy), toolkit::getCurrentMillisecond(true));
            } catch (const std::exception &e) {
                WarnL << "Failed to apply batch from " << relay_peer_id << ": " << e.what();
            }
        }, false);
    };

    GET_CONFIG(int, batch_limit, Database::kBatchLimit);
    Json::Value all_seqs_json = Json::arrayValue;
    Json::Value all_acks_json = Json::arrayValue;

    for (const auto &seq : all_seqs) {
        // Note: we send the full cursor map on every pull, even for peers that are not the source of the relay data. This is because:
        // 1) It allows all nodes to converge to the same cursor map after 1-2 ticks, even if they pull from different subsets of peers due to fanout or dynamic peer changes.
        // 2) The cursor map is relatively small (one entry per peer/db), so the overhead is minimal compared to the benefit of faster convergence and simpler logic without needing to track which peers have which data.
        all_seqs_json.append(seq.toJson());
        // For each cursor we send, we can also include an ack log entry indicating the last sequence we've successfully received from that peer/db. 
        // This allows the relay peer to update its ack logs and potentially prune old data sooner. Note that the acked_seq is based on what we've applied, not just what we've pulled, so it reflects the actual state of our database.
        PeerAckLog ack;
        ack.peer_guid = _self_node_id;
        ack.db_guid = _self_db_id;
        ack.src_peer_guid = seq.peer_guid;
        ack.src_db_guid = seq.db_guid;
        ack.acked_seq = seq.sequence;
        ack.updated_at = time(nullptr);
        all_acks_json.append(ack.toJson());
    }

    auto flag = NOTICE_EMIT(BroadcastSyncChangesArgs, Broadcast::kBroadcastSyncChanges,
                            base_url, _self_node_id, _self_db_id, all_seqs_json, all_acks_json, batch_limit, onRes);
    if (!flag) {
        WarnL << "No listener for kBroadcastSyncChanges, cannot relay pull from " << relay_peer_id;
    }
}

std::vector<SyncManager::SyncGap> SyncManager::applyBatch(const Json::Value &rows) {
    // Responses from several relays (gossip fanout) may arrive concurrently and carry
    // the same entries; apply them one batch at a time.
    lock_guard<mutex> apply_lock(_apply_mtx);
    auto log_imp = make_shared<TransactionLogImp>();
    auto version_imp = make_shared<SyncRowVersionImp>();
    // Wrap the entire batch in one SQLite transaction: data rows, row versions (LWW state),
    // transaction_log entries and cursors are committed atomically, or not at all.
    auto txn = log_imp->getExecutor()->beginWriteTxn();

    // Per source (peer_guid|db_guid): committed cursor and the next sequence this batch may apply.
    // Cursors only advance on commit, so a row at or below its cursor was already applied
    // (typically the same entries returned by several relays in one gossip round).
    unordered_map<string, int> cursors;
    for (const auto &c : make_shared<TransactionSequenceImp>()->findAllWithTxn(txn)) {
        cursors[c.peer_guid + "|" + c.db_guid] = c.sequence;
    }
    unordered_map<string, int> next_seq;
    unordered_set<string> blocked; // sources with a gap in this batch
    vector<SyncGap> gaps;
    size_t skipped = 0;

    for (const auto &it : rows) {
        const string peer = it["peer_guid"].asString();
        const string db   = it["db_guid"].asString();
        const string src  = peer + "|" + db;
        const int seq     = it["sequence"].asInt();
        if (blocked.count(src)) {
            continue;
        }
        auto cur = cursors.find(src);
        const int cursor = cur != cursors.end() ? cur->second : 0;
        if (seq <= cursor) {
            ++skipped;
            continue;
        }
        auto expected = next_seq.find(src);
        const int want = expected != next_seq.end() ? expected->second : cursor + 1;
        if (seq > want) {
            // Entries want..seq-1 are missing from this relay (pruned, or never received by it).
            // Applying seq would move the cursor past them and lose them for good.
            WarnL << "SyncDB gap from " << peer << "/" << db << ": expected seq " << want << ", got " << seq
                  << "; entries of this source are not applied";
            SyncGap gap;
            gap.peer_guid = peer;
            gap.db_guid = db;
            gap.cursor = want - 1;
            gap.first_seq = seq;
            gaps.push_back(gap);
            blocked.insert(src);
            continue;
        }
        next_seq[src] = seq + 1;

        // Dedup — exact duplicate check before any work (e.g. restart replay,
        // or same entry received via multiple relay paths).
        string tran_guid = it["tran_guid"].asString();
        if (!tran_guid.empty() && log_imp->existsByTranGuidWithTxn(tran_guid, txn)) {
            TraceL << "applyBatch dedup: tran_guid=" << tran_guid << " already exists, skipping";
            continue;
        }

        // Record the transaction in the local log (for relaying) and advance the source cursor,
        // whatever the outcome: stale and deferred entries are kept too (see TranAuditFlag).
        auto log = TransactionLog::fromJson(it);
        log.timestamp_hi = applyEntryWithTxn(log, *version_imp, txn);
        log_imp->addWithTxn(log, txn);
    }
    txn->commit(); // on exception the transaction rolls back when it goes out of scope
    if (skipped) {
        TraceL << "applyBatch skipped " << skipped << " already-applied entries";
    }
    return gaps;
}

int SyncManager::applyEntryWithTxn(const TransactionLog &log, SyncRowVersionImp &versions, SqliteTransaction::Ptr txn) {
    if (log.tran_type != static_cast<int>(TranType::DataMutation)) {
        WarnL << "SyncDB: unknown tran_type " << log.tran_type << " (tran_guid=" << log.tran_guid << "), deferred";
        return TRAN_AUDIT_DEFERRED;
    }
    // tran_data is a JSON string: {"table":"...","op":"UPSERT|UPSERT_BATCH|DELETE","payload":{...}}
    Json::Value tran_data;
    if (!StrJsonUtils::readJsonString(log.tran_data, tran_data)) {
        WarnL << "SyncDB: invalid tran_data JSON (tran_guid=" << log.tran_guid << "), deferred";
        return TRAN_AUDIT_DEFERRED;
    }
    const string table          = tran_data["table"].asString();
    const string op             = tran_data["op"].asString();
    const Json::Value &payload  = tran_data["payload"];

    auto hit = _table_handlers.find(table);
    if (hit == _table_handlers.end()) {
        WarnL << "SyncDB: unknown table '" << table << "' (tran_guid=" << log.tran_guid << "), deferred";
        return TRAN_AUDIT_DEFERRED;
    }
    const TableSyncHandler &h = hit->second;
    if (op != TRAN_DATA_OP_UPSERT && op != TRAN_DATA_OP_UPSERT_BATCH && op != TRAN_DATA_OP_DELETE) {
        WarnL << "SyncDB: unknown op '" << op << "' for " << table << ", deferred";
        return TRAN_AUDIT_DEFERRED;
    }
    if (op == TRAN_DATA_OP_UPSERT_BATCH && !h.onUpsertBatchWithTxn && !h.onUpsertBatch) {
        WarnL << "SyncDB: table '" << table << "' has no UPSERT_BATCH handler, deferred";
        return TRAN_AUDIT_DEFERRED;
    }

    Json::Value apply_payload;
    set<string> keep_keys;
    ApplyDecision decision = resolveConflict(table, h, op, payload, log.timestamp, log.peer_guid,
                                             versions, txn, apply_payload, keep_keys);
    if (decision == ApplyDecision::Skip) {
        return TRAN_AUDIT_STALE;
    }
    if (op == TRAN_DATA_OP_UPSERT) {
        if (h.onUpsertWithTxn) h.onUpsertWithTxn(apply_payload, txn);
        else if (h.onUpsert) h.onUpsert(apply_payload);
    } else if (op == TRAN_DATA_OP_UPSERT_BATCH) {
        if (h.onUpsertBatchWithTxn) h.onUpsertBatchWithTxn(apply_payload, txn);
        else h.onUpsertBatch(apply_payload);
    } else if (h.groupKey && h.onGroupDeleteWithTxn) {
        h.onGroupDeleteWithTxn(apply_payload, keep_keys, txn);
    } else {
        if (h.onDeleteWithTxn) h.onDeleteWithTxn(apply_payload, txn);
        else if (h.onDelete) h.onDelete(apply_payload);
    }
    return decision == ApplyDecision::ApplyWinner ? TRAN_AUDIT_WINNER : TRAN_AUDIT_APPLIED;
}

size_t SyncManager::reapplyDeferredEntries() {
    lock_guard<mutex> apply_lock(_apply_mtx);
    auto log_imp = make_shared<TransactionLogImp>();
    auto version_imp = make_shared<SyncRowVersionImp>();
    auto txn = log_imp->getExecutor()->beginWriteTxn();
    size_t applied = 0;
    // Entries are applied in source order; LWW versions make the late apply safe even though
    // newer entries of the same rows may already be in place.
    for (const auto &log : log_imp->findByAuditFlagWithTxn(TRAN_AUDIT_DEFERRED, 0, txn)) {
        int flag = applyEntryWithTxn(log, *version_imp, txn);
        if (flag != TRAN_AUDIT_DEFERRED) {
            log_imp->updateAuditFlagWithTxn(log.tran_guid, flag, txn);
            ++applied;
        }
    }
    txn->commit();
    if (applied) {
        InfoL << "SyncDB re-applied " << applied << " deferred entries";
    }
    return applied;
}

void SyncManager::handleGaps(const std::vector<SyncGap> &gaps, uint64_t now_ms) {
    if (gaps.empty()) return;
    GET_CONFIG(int, interval_sec, Database::kPullIntervalSec);
    // Another relay may still hold the missing entries; only resync if the cursor stays stuck.
    const uint64_t resync_after_ms = static_cast<uint64_t>(interval_sec > 0 ? interval_sec : 30) * 3 * 1000;
    for (const auto &gap : gaps) {
        const string src = gap.peer_guid + "|" + gap.db_guid;
        auto it = _gap_state.find(src);
        if (it == _gap_state.end() || it->second.first != gap.cursor) {
            _gap_state[src] = std::make_pair(gap.cursor, now_ms);
            continue;
        }
        if (now_ms - it->second.second >= resync_after_ms) {
            _gap_state.clear();
            requestResync(StrPrinter << "entries " << (gap.cursor + 1) << ".." << (gap.first_seq - 1) << " of "
                                     << gap.peer_guid << "/" << gap.db_guid << " are no longer available from peers");
            return;
        }
    }
}

void SyncManager::requestResync(const string &reason) {
    WarnL << "SyncDB full resync requested: " << reason;
    Json::Value state;
    state["schema_version"] = 1;
    state["status"] = "pending";
    state["reason"] = reason;
    state["requested_at_ms"] = static_cast<Json::Int64>(toolkit::getCurrentMillisecond(true));
    auto imp = make_shared<MiscDataImp>();
    MiscData data;
    data.key = MISC_DATA_DB_BOOTSTRAP_DONE;
    data.value = StrJsonUtils::writeJsonString(state);
    imp->add(data);

    lock_guard<mutex> lock(_mtx);
    _bootstrapped = false;
    _last_bootstrap_peer.clear();
    _bootstrap_probes.clear();
}

std::unordered_set<std::string> SyncManager::selectPruneWatermarkPeers(
    const std::unordered_set<std::string> &healthy, const std::vector<std::string> &known,
    const std::unordered_map<std::string, int64_t> &last_ack_sec, int64_t now_sec, int64_t offline_ttl_sec) {
    std::unordered_set<std::string> peers = healthy;
    for (const auto &peer : known) {
        auto it = last_ack_sec.find(peer);
        // An offline peer holds the watermark back while it can still resume incrementally.
        // A peer that never acked, or stayed away longer than the TTL, resyncs from a snapshot
        // when it returns (gap detection), so it must not block pruning.
        if (it != last_ack_sec.end() && now_sec - it->second <= offline_ttl_sec) {
            peers.insert(peer);
        }
    }
    return peers;
}

SyncManager::ApplyDecision SyncManager::resolveConflict(const string &table, const TableSyncHandler &h,
                                                        const string &op, const Json::Value &payload,
                                                        int64_t ts, const string &peer,
                                                        SyncRowVersionImp &versions, SqliteTransaction::Ptr txn,
                                                        Json::Value &apply_payload, set<string> &keep_keys) {
    apply_payload = payload;
    if (!h.rowKey) {
        return ApplyDecision::Apply; // table without LWW tracking
    }

    // UPSERT_BATCH: every item is its own row; stale items are dropped from the payload.
    if (op == TRAN_DATA_OP_UPSERT_BATCH) {
        const Json::Value &items = payload["items"];
        if (!items.isArray()) {
            return ApplyDecision::Apply;
        }
        Json::Value kept = Json::arrayValue;
        bool conflict = false;
        for (const auto &item : items) {
            string key = h.rowKey(item);
            string group = h.groupKey ? h.groupKey(item) : string();
            if (key.empty()) {
                kept.append(item);
                continue;
            }
            SyncRowVersion prev;
            bool has_prev = versions.findEffectiveWithTxn(table, key, group, prev, txn);
            if (has_prev && !SyncRowVersionImp::isNewer(ts, peer, prev)) {
                TraceL << "resolveConflict skip stale item: " << table << ":" << key << " incoming=" << ts << "/" << peer
                       << " stored=" << prev.ts_ms << "/" << prev.peer_guid;
                continue;
            }
            conflict = conflict || has_prev;
            kept.append(item);
            versions.recordWithTxn(makeVersion(table, key, group, ts, peer, false), txn);
        }
        if (kept.empty()) {
            return ApplyDecision::Skip;
        }
        apply_payload["items"] = kept;
        return conflict ? ApplyDecision::ApplyWinner : ApplyDecision::Apply;
    }

    // Group DELETE (e.g. all kv of a resource): compared with the group tombstone; rows written
    // after the delete survive it.
    if (op == TRAN_DATA_OP_DELETE && h.groupKey) {
        string group = h.groupKey(payload);
        if (group.empty()) {
            WarnL << "resolveConflict: missing group key for table=" << table << " op=" << op;
            return ApplyDecision::Skip;
        }
        const string tomb_key = SyncRowVersionImp::groupRowKey(group);
        SyncRowVersion prev;
        bool has_prev = versions.findWithTxn(table, tomb_key, prev, txn);
        if (has_prev && !SyncRowVersionImp::isNewer(ts, peer, prev)) {
            return ApplyDecision::Skip;
        }
        keep_keys = versions.liveKeysNewerThanWithTxn(table, group, ts, peer, txn);
        versions.recordWithTxn(makeVersion(table, tomb_key, group, ts, peer, true), txn);
        return has_prev ? ApplyDecision::ApplyWinner : ApplyDecision::Apply;
    }

    // Single-row UPSERT / DELETE.
    string key = h.rowKey(payload);
    if (key.empty()) {
        WarnL << "resolveConflict: missing row key for table=" << table << " op=" << op;
        return ApplyDecision::Skip;
    }
    string group = h.groupKey ? h.groupKey(payload) : string();
    SyncRowVersion prev;
    bool has_prev = versions.findEffectiveWithTxn(table, key, group, prev, txn);
    if (has_prev && !SyncRowVersionImp::isNewer(ts, peer, prev)) {
        TraceL << "resolveConflict skip stale: " << table << ":" << key << " incoming=" << ts << "/" << peer
               << " stored=" << prev.ts_ms << "/" << prev.peer_guid << (prev.deleted ? " (deleted)" : "");
        return ApplyDecision::Skip;
    }
    versions.recordWithTxn(makeVersion(table, key, group, ts, peer, op == TRAN_DATA_OP_DELETE), txn);
    return has_prev ? ApplyDecision::ApplyWinner : ApplyDecision::Apply;
}

SyncRowVersion SyncManager::makeVersion(const string &table, const string &key, const string &group,
                                        int64_t ts, const string &peer, bool deleted) {
    SyncRowVersion v;
    v.table_name = table;
    v.row_key    = key;
    v.group_key  = group;
    v.ts_ms      = ts;
    v.peer_guid  = peer;
    v.deleted    = deleted ? 1 : 0;
    return v;
}

int64_t SyncManager::stampLocalMutation(const string &table, const string &op, const Json::Value &payload,
                                        int64_t now_ms, const string &self_peer) {
    auto hit = _table_handlers.find(table);
    if (hit == _table_handlers.end() || !hit->second.rowKey) {
        return now_ms;
    }
    const TableSyncHandler &h = hit->second;
    vector<SyncRowVersion> keys;
    if (op == TRAN_DATA_OP_UPSERT_BATCH) {
        for (const auto &item : payload["items"]) {
            string key = h.rowKey(item);
            if (!key.empty()) {
                keys.push_back(makeVersion(table, key, h.groupKey ? h.groupKey(item) : string(), 0, self_peer, false));
            }
        }
    } else if (op == TRAN_DATA_OP_DELETE && h.groupKey) {
        string group = h.groupKey(payload);
        if (!group.empty()) {
            keys.push_back(makeVersion(table, SyncRowVersionImp::groupRowKey(group), group, 0, self_peer, true));
        }
    } else {
        string key = h.rowKey(payload);
        if (!key.empty()) {
            keys.push_back(makeVersion(table, key, h.groupKey ? h.groupKey(payload) : string(), 0, self_peer,
                                       op == TRAN_DATA_OP_DELETE));
        }
    }
    if (keys.empty()) {
        return now_ms;
    }
    return make_shared<SyncRowVersionImp>()->stampLocal(keys, now_ms, self_peer);
}

// ─── GC watermark ────────────────────────────────────────────────────────────

void SyncManager::recordRelayAck(const Json::Value &ack_cursors_json) {
    std::vector<PeerAckLog> ack_cursors;
    if (!ack_cursors_json.empty() && ack_cursors_json.isArray()) {
        for (const auto &c : ack_cursors_json) {
            ack_cursors.push_back(PeerAckLog::fromJson(c));
        }
    }

    if (ack_cursors.empty()) return;
    
    auto ack_imp = make_shared<PeerAckLogImp>();
    for (auto &log : ack_cursors) {
        ack_imp->add(log);
    }
}

void SyncManager::maybePruneLog() {
    GET_CONFIG(int, log_keep_num, Database::kTransactionLogKeepLast)
    GET_CONFIG(int, tombstone_ttl_days, Database::kRowVersionTombstoneTtlDays)
    if (tombstone_ttl_days > 0) {
        int64_t cutoff_ms = static_cast<int64_t>(toolkit::getCurrentMillisecond(true)) -
                            static_cast<int64_t>(tombstone_ttl_days) * 24 * 3600 * 1000;
        try {
            make_shared<SyncRowVersionImp>()->pruneTombstones(cutoff_ms);
        } catch (const std::exception &ex) {
            WarnL << "Failed to prune row version tombstones: " << ex.what();
        }
    }
    if (_single_node)  {
        auto seq_imp = make_shared<TransactionSequenceImp>();
        auto current_seq = seq_imp->findSeqByPeerIdAndDbGuid(_self_node_id, _self_db_id);
        auto safe_seq = current_seq - log_keep_num;
        if (safe_seq > 0) {
            auto log_imp = make_shared<TransactionLogImp>();
            log_imp->pruneAckedLogs(_self_node_id, _self_db_id, safe_seq);
            TraceL << "Pruned logs for single node mode up to seq=" << safe_seq;
        }
        return;
    }
    // Watermark peers: healthy peers plus known peers that are offline but may still resume
    // incrementally (see selectPruneWatermarkPeers).
    unordered_set<string> healthy;
    string self_id;
    {
        lock_guard<mutex> lock(_mtx);
        for (const auto &pair : _peers) {
            healthy.insert(pair.first);
        }
        self_id = _self_node_id;
    }
    GET_CONFIG(int, self_group_id, Cluster::kServerLocationId);
    GET_CONFIG(int, offline_ttl_days, Database::kPeerOfflineTtlDays);
    vector<string> known;
    for (const auto &id : ClusterManager::Instance().getMediaServerIds()) {
        if (id != self_id && ClusterManager::Instance().getMediaServer(id).serverGroupId == self_group_id) {
            known.push_back(id);
        }
    }
    auto ack_imp = make_shared<PeerAckLogImp>();
    auto active_peers = selectPruneWatermarkPeers(healthy, known, ack_imp->findLastAckTimes(),
                                                  static_cast<int64_t>(time(nullptr)),
                                                  static_cast<int64_t>(offline_ttl_days) * 24 * 3600);
    if (active_peers.empty()) return;

    auto safe_seqs = ack_imp->findMinAckedSeq(active_peers);

    auto log_imp = make_shared<TransactionLogImp>();

    for (const auto &seq : safe_seqs) {
        auto safe_seq = seq.sequence - log_keep_num;
        if (safe_seq <= 0) continue; // nothing to prune
        log_imp->pruneAckedLogs(seq.peer_guid, seq.db_guid, safe_seq);
        TraceL << "Pruned logs for source peer=" << seq.peer_guid << " db_guid=" << seq.db_guid << " up to seq=" << safe_seq;
    }
}

void SyncManager::setSingleNodeMode(bool single_node) {
    _single_node = single_node;
    InfoL << "SyncManager single node mode set to " << single_node;
}

Json::Value SyncManager::getMiscData() {
    auto imp = std::make_shared<MiscDataImp>();
    auto ret = imp->findAll();
    Json::Value misc_json;
    for (const auto &r : ret) {
        if (r.key.empty() || r.value.empty()) {
            continue;
        }
        misc_json[r.key] = r.value;
    }
    return misc_json;
}

} // namespace managerkit

#endif // ENABLE_SYNC_CHANGES
