#include "SyncManager.h"
#include "Storage/TransactionSequence.h"
#include "Storage/TransactionLog.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResourceAssignment.h"
#include "Storage/MiscData.h"
#include "Common/StrUtil.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(SyncManager)

// ─── Entity ↔ JSON helpers ──────────────────────────────────────────────────

static Json::Value transactionSequenceToJson(const TransactionSequence &seq) {
    Json::Value v;
    v["peer_guid"] = seq.peer_guid;
    v["db_guid"]   = seq.db_guid;
    v["sequence"]  = seq.sequence;
    return v;
}

static TransactionSequence jsonToTransactionSequence(const Json::Value &v) {
    TransactionSequence seq;
    seq.peer_guid = v["peer_guid"].asString();
    seq.db_guid   = v["db_guid"].asString();
    seq.sequence  = v["sequence"].asInt();
    return seq;
}

static Json::Value vmsResourceToJson(const VmsResource &r) {
    Json::Value v;
    v["id"]          = r.id;
    v["guid"]        = r.guid;
    v["parent_guid"] = r.parent_guid;
    v["name"]        = r.name;
    v["url"]         = r.url;
    v["xtype_guid"]  = r.xtype_guid;
    return v;
}

static VmsResource jsonToVmsResource(const Json::Value &v) {
    VmsResource r;
    r.id          = v["id"].asString();
    r.guid        = v["guid"].asString();
    r.parent_guid = v["parent_guid"].asString();
    r.name        = v["name"].asString();
    r.url         = v["url"].asString();
    r.xtype_guid  = v["xtype_guid"].asString();
    return r;
}

static Json::Value vmsKvPairToJson(const VmsKvPair &kv) {
    Json::Value v;
    v["id"]            = kv.id;
    v["resource_guid"] = kv.resource_guid;
    v["name"]          = kv.name;
    v["value"]         = kv.value;
    return v;
}

static VmsKvPair jsonToVmsKvPair(const Json::Value &v) {
    VmsKvPair kv;
    kv.id            = v["id"].asString();
    kv.resource_guid = v["resource_guid"].asString();
    kv.name          = v["name"].asString();
    kv.value         = v["value"].asString();
    return kv;
}

static VmsResourceAssignment jsonToVmsResourceAssignment(const Json::Value &v) {
    VmsResourceAssignment assign;
    assign.assignment_guid = v["assignment_guid"].asString();
    assign.resource_guid   = v["resource_guid"].asString();
    assign.owner_peer_id   = v["owner_peer_id"].asString();
    assign.owner_db_guid   = v["owner_db_guid"].asString();
    assign.assigned_at     = v["assigned_at"].asInt64();
    assign.released_at     = v["released_at"].asInt64();
    assign.assign_type     = v["assign_type"].asInt();
    assign.prev_peer_id    = v["prev_peer_id"].asString();
    return assign;
}

static Json::Value vmsResourceAssignmentToJson(const VmsResourceAssignment &assign) {
    Json::Value v;
    v["assignment_guid"] = assign.assignment_guid;
    v["resource_guid"]   = assign.resource_guid;
    v["owner_peer_id"]   = assign.owner_peer_id;
    v["owner_db_guid"]   = assign.owner_db_guid;
    v["assigned_at"]     = assign.assigned_at;
    v["released_at"]     = assign.released_at;
    v["assign_type"]     = assign.assign_type;
    v["prev_peer_id"]    = assign.prev_peer_id;
    return v;
}

static Json::Value transactionLogToJson(const TransactionLog &log) {
    Json::Value v;
    v["peer_guid"]  = log.peer_guid;
    v["db_guid"]    = log.db_guid;
    v["sequence"]   = log.sequence;
    v["timestamp"]   = log.timestamp;
    v["tran_guid"]  = log.tran_guid;
    v["tran_data"] = log.tran_data;
    v["peer_guid"]  = log.peer_guid;
    v["timestamp_hi"]  = log.timestamp_hi;
    return v;
}

static TransactionLog jsonToTransactionLog(const Json::Value &v) {
    TransactionLog log;
    log.peer_guid  = v["peer_guid"].asString();
    log.db_guid    = v["db_guid"].asString();
    log.sequence   = v["sequence"].asInt();
    log.timestamp   = v["timestamp"].asInt64();
    log.tran_guid  = v["tran_guid"].asString();
    log.tran_data  = v["tran_data"].asString();
    log.tran_type  = v["tran_type"].asInt();
    log.timestamp_hi = v["timestamp_hi"].asInt();
    return log;
}

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

    auto executor = make_shared<SqliteQueryExecutor>(db_tag);

    // Read all tables in one transaction for snapshot consistency
    auto txn = executor->execTxn();
    try {
        // Step 1: transaction_sequence – receiver uses this to detect stale data
        {
            auto sql = QueryBuilder()
                           .select(EntityTraits<TransactionSequence>::getColumns())
                           .from(EntityTraits<TransactionSequence>::tableName())
                           .build();
            auto rows = executor->executeRawWithTxn(txn, sql);
            for (const auto &r : rows) {
                snap.sequences.append(transactionSequenceToJson(EntityTraits<TransactionSequence>::fromRow(r)));
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
                snap.vms_resource.append(vmsResourceToJson(EntityTraits<VmsResource>::fromRow(r)));
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
                snap.vms_kvpair.append(vmsKvPairToJson(EntityTraits<VmsKvPair>::fromRow(r)));
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
    root["peer_id"]               = snap.peer_id;
    root["db_guid"]               = snap.db_guid;
    root["sequences"] = snap.sequences;
    root["vms_resource"]          = snap.vms_resource;
    root["vms_kvpair"]            = snap.vms_kvpair;
    return StrJsonUtils::writeJsonString(root);
}

SnapshotData SnapshotBuilder::deserialize(const string &json_body) {
    SnapshotData snap;
    Json::Value root;
    if (!StrJsonUtils::readJsonString(json_body, root)) {
        WarnL << "[Snapshot] deserialize: invalid JSON";
        return snap; // snap.empty() == true
    }
    snap.peer_id               = root["peer_id"].asString();
    snap.db_guid               = root["db_guid"].asString();
    snap.sequences             = root["sequences"];
    snap.vms_resource          = root["vms_resource"];
    snap.vms_kvpair            = root["vms_kvpair"];
    return snap;
}

// ─── SyncManager ────────────────────────────────────────────────────────────

void SyncManager::start() {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    GET_CONFIG(bool,   enable_sync,   Database::kEnableSyncDb);
    GET_CONFIG(int,    interval_sec,  Database::kPullIntervalSec);
    GET_CONFIG(string, peer_list,     Database::kPeerList);

    if (!enable_sync) {
        WarnL << "Sync database opration disabled by config";
        return;
    }

    {
        lock_guard<mutex> lock(_mtx);
        _self_node_id = mediaServerId;
        _self_db_id   = findDbGuid(_self_node_id);
        CHECK(_self_node_id.empty() || _self_db_id.empty());
        _running      = true;
        _peers.clear();
        _bootstrapping.clear();

        // Parse peer list config
        if (!peer_list.empty()) {
            auto peers = split(peer_list, ",");
            for (const auto &p : peers) {
                auto sep = p.find('@');
                if (sep != string::npos) {
                    string peer_id  = p.substr(0, sep);
                    string base_url = p.substr(sep + 1);
                    if (peer_id.empty() || base_url.empty()) {
                        WarnL << "Invalid peer config (empty peer_id or base_url): " << p;
                        continue;
                    }
                    if (peer_id == _self_node_id) {
                        WarnL << "Ignore adding self to peer list";
                        continue;
                    }
                    _peers[peer_id] = base_url;
                    InfoL << "Added peer from config: " << peer_id << " (" << base_url << ")";
                } else {
                    WarnL << "Invalid peer config (missing '@'): " << p;
                }
            }
        }
    }

    weak_ptr<SyncManager> weak_self = shared_from_this();
    float _interval_sec = interval_sec > 0 ? interval_sec *1.0f : 30.0f; // default to 30s if config is invalid
    _timer = make_shared<Timer>(_interval_sec, [weak_self]() {
        auto self = weak_self.lock();
        if (self) self->onTick();
        return true;
    }, nullptr);

    // If transaction_sequence already has data from a previous run, skip bootstrap
    if (!needsBootstrap()) {
        lock_guard<mutex> lock(_mtx);
        _bootstrapped = true;
        InfoL << "Database already populated, skipping bootstrap";
    }

    InfoL << "Sync database started, node=" << mediaServerId << ", bootstrapped=" << _bootstrapped << ", interval=" << interval_sec << "s";
}

void SyncManager::stop() {
    lock_guard<mutex> lock(_mtx);
    _running      = false;
    _bootstrapped = false;
    _timer.reset();
    _peers.clear();
    _bootstrapping.clear();
    _last_bootstrap_peer.clear();
    InfoL << "Sync database stopped";
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
    if (_last_bootstrap_peer == peer_id) {
        _last_bootstrap_peer.clear();
    }
    InfoL << "Removed peer: " << peer_id;
    // Note: Must NOT delete transaction_sequence entries for the removed peer, because:
    // 1) If the peer is re-added later, the old cursor is still valid and can be resumed from.
    // 2) If the peer is permanently removed, the transaction_sequence entries for that peer will 
    // be orphaned but harmless (they won't match any incoming peer_id) and can be ignored or cleaned up manually if desired.
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
        doBootstrap(target_id, target_url, [weak_self, target_id](bool ok) {
            auto self = weak_self.lock();
            if (!self) return;
            {
                lock_guard<mutex> lock(self->_mtx);
                self->_bootstrapping.erase(target_id);
                if (ok) {
                    self->_bootstrapped = true;
                    self->_last_bootstrap_peer.clear(); // reset on success
                }
                // on failure: _last_bootstrap_peer = target_id stays
                // → next tick picks the peer AFTER target_id
            }
            if (ok) {
                InfoL << "Bootstrap done, peer=" << target_id;
                self->markBootstrapDone();
            } else {
                WarnL << "Bootstrap failed, peer=" << target_id << ", will try next peer on next tick";
            }
        });
        return; // don't pull yet; wait for bootstrap to complete
    }

    // ── Pull phase: incremental pull from ALL peers ────────────────────────
    for (const auto &pair : peers_copy) {
        const string &pid = pair.first;
        const string &url = pair.second;
        auto db_guid = findDbGuid(pid);
        if (db_guid.empty()) {
            // Peer has no known db_guid yet (new node joining after initial bootstrap).
            // Trigger a per-peer mini-bootstrap to establish its cursor before pulling.
            bool already;
            {
                lock_guard<mutex> lock(_mtx);
                already = _bootstrapping.count(pid) > 0;
                if (!already) {
                    _bootstrapping.insert(pid);
                }
            }
            if (already) {
                TraceL << "Mini-bootstrap for peer " << pid << " already in progress, skipping";
                continue;
            }
            InfoL << "New peer " << pid << " has no db_guid yet, triggering mini-bootstrap";
            weak_ptr<SyncManager> weak_self = shared_from_this();
            doMiniBootstrap(pid, url, [weak_self, pid](bool ok) {
                auto self = weak_self.lock();
                if (!self) return;
                {
                    lock_guard<mutex> lock(self->_mtx);
                    self->_bootstrapping.erase(pid);
                }
                if (ok) {
                    InfoL << "Mini-bootstrap done for new peer=" << pid;
                } else {
                    WarnL << "Mini-bootstrap failed for peer=" << pid << ", will retry next tick";
                }
            });
        } else {
            pullFromPeer(pid, db_guid, url);
        }
    }
}

// ─── Bootstrap ───────────────────────────────────────────────────────────────

bool SyncManager::needsBootstrap() {
    auto imp = make_shared<MiscDataImp>();
    auto ret = imp->findByKey(MISC_DATA_DB_BOOTSTRAP_DONE);
    return ret.empty() || ret[0].value != "1";
}

void SyncManager::markBootstrapDone() {
    auto imp = make_shared<MiscDataImp>();
    MiscData data;
    data.key = MISC_DATA_DB_BOOTSTRAP_DONE;
    data.value = "1";
    imp->add(data);
}

void SyncManager::doBootstrap(const string &peer_id, const string &base_url, DoneCb cb) {
    InfoL << "Bootstrapping from " << peer_id << " (base_url=" << base_url << ")";

    weak_ptr<SyncManager> weak_self = shared_from_this();
    Broadcast::OnResInvoker onRes = [weak_self, peer_id, cb](const string &err, const int &idx, const Json::Value &data) {
        auto self = weak_self.lock();
        if (!self) return;

        if (!err.empty()) {
            WarnL << "Bootstrap request to " << peer_id << " failed: " << err;
            if (cb) cb(false);
            return;
        }
        try {
            auto snap = SnapshotBuilder::deserialize(data["data"].asString());
            self->applySnapshot(snap);
            InfoL << "Bootstrap from " << peer_id << " done:"
                  << " peer_id=" << snap.peer_id
                  << " db_guid=" << snap.db_guid
                  << " seq=" << snap.sequences.size()
                  << " resource=" << snap.vms_resource.size()
                  << " kv="  << snap.vms_kvpair.size();
            if (cb) cb(true);
        } catch (std::exception &ex) {
            WarnL << "Failed to apply snapshot from " << peer_id << ": " << ex.what();
            if (cb) cb(false);
        }
    };

    auto flag = NOTICE_EMIT(BroadcastSyncSnapshotArgs, Broadcast::kBroadcastSyncSnapshot, base_url, peer_id, onRes);
    if (!flag) {
        WarnL << "No listener for kBroadcastSyncSnapshot, cannot bootstrap from " << peer_id;
        if (cb) cb(false);
    }
}

// Mini-bootstrap: only registers the new peer's cursor in transaction_sequence.
// Does NOT apply vms_resource / vms_kvpair to avoid two data hazards:
//   1. Resurrection of records deleted since our last snapshot (stale data overwrites delete)
//   2. Cursor regression: applying older sequence values from a lagging peer would
//      reset our pull cursor backwards causing duplicate row processing.
// After the cursor is established, the normal incremental pull loop (pullFromPeer)
// will fetch all of the peer's transaction_log rows from sequence 0 onwards.
void SyncManager::doMiniBootstrap(const string &peer_id, const string &base_url, DoneCb cb) {
    InfoL << "Mini-bootstrapping from " << peer_id << " (base_url=" << base_url << ")";

    weak_ptr<SyncManager> weak_self = shared_from_this();
    Broadcast::OnResInvoker onRes = [weak_self, peer_id, cb](const string &err, const int &idx, const Json::Value &data) {
        auto self = weak_self.lock();
        if (!self) return;

        if (!err.empty()) {
            WarnL << "Mini-bootstrap request to " << peer_id << " failed: " << err;
            if (cb) cb(false);
            return;
        }
        try {
            auto snap = SnapshotBuilder::deserialize(data["data"].asString());
            if (snap.peer_id.empty() || snap.db_guid.empty()) {
                WarnL << "Mini-bootstrap: empty peer_id or db_guid in snapshot from " << peer_id;
                if (cb) cb(false);
                return;
            }
            // Register cursor for the new peer only if no entry exists yet.
            // Starting at sequence 0 ensures we pull all of the peer's history.
            insertIfNotExistsCursorSeq(snap.peer_id, snap.db_guid, 0);
            if (cb) cb(true);
        } catch (std::exception &ex) {
            WarnL << "Mini-bootstrap failed from " << peer_id << ": " << ex.what();
            if (cb) cb(false);
        }
    };

    auto flag = NOTICE_EMIT(BroadcastSyncSnapshotArgs, Broadcast::kBroadcastSyncSnapshot, base_url, peer_id, onRes);
    if (!flag) {
        WarnL << "No listener for kBroadcastSyncSnapshot, cannot mini-bootstrap from " << peer_id;
        if (cb) cb(false);
    }
}

void SyncManager::applySnapshot(const SnapshotData &snap) {
    {
        if (!snap.vms_resource.empty() && snap.vms_resource.isArray()) {
            auto imp = std::make_shared<VmsResourceImp>();
            for (const auto &v : snap.vms_resource) {
                auto r = jsonToVmsResource(v);
                imp->add(r, false); // remote data — do not append to local transaction_log
            }
        }
    }
    {
        if (!snap.vms_kvpair.empty() && snap.vms_kvpair.isArray()) {
            auto imp = std::make_shared<VmsKvPairImp>();
            for (const auto &v : snap.vms_kvpair) {
                auto kv = jsonToVmsKvPair(v);
                imp->add(kv, false); // remote data — do not append to local transaction_log
            }
        }
    }
    {
        if (!snap.resource_assignment.empty() && snap.resource_assignment.isArray()) {
            auto imp = std::make_shared<VmsResourceAssignmentImp>();
            for (const auto &v : snap.resource_assignment) {
                auto seq = jsonToVmsResourceAssignment(v);
                imp->add(seq, false); // remote data — do not append to local transaction_log
            }
        }
    }
    {
        if (!snap.sequences.empty() && snap.sequences.isArray()) {
            auto imp = std::make_shared<TransactionSequenceImp>();
            for (const auto &v : snap.sequences) {
                auto seq = jsonToTransactionSequence(v);
                imp->add(seq);
            }
        }
    }
}

// ─── incremental pull ──────────────────────────────────────────────────────

void SyncManager::pullFromPeer(const string &peer_id, const string &db_guid, const string &base_url) {
    CHECK(!db_guid.empty(), StrPrinter << "Cannot find db_guid for peer " << peer_id << ", cannot pull");
    int last_seq = findCursorSeq(peer_id, db_guid);
    
    InfoL << "Pulling from peer " << peer_id << " (db_guid= " << db_guid << ", base_url= " << base_url << ") since sequence: " << last_seq;
    
    weak_ptr<SyncManager> weak_ptr = shared_from_this();
    Broadcast::OnResInvoker onRes = [weak_ptr, peer_id, db_guid, last_seq](const string &err, const int &idx, const Json::Value &data) {
        auto self = weak_ptr.lock();
        if (!self) return;

        if (!err.empty()) {
            WarnL << "Pull sync changes request to peer " << peer_id << " failed: " << err;
            return;
        }

        auto rows = data["data"]; 
        if (!rows.isArray() || rows.empty()) return;

        self->applyBatch(rows);
        InfoL << "Pulled " << rows.size() << " changes from peer " << peer_id << " for db " << db_guid;

        // Advance cursor to max sequence seen in this batch
        int max_seq = last_seq;
        for (const auto &row : rows) {
            int seq = row["sequence"].asInt();
            if (seq > max_seq) {
                max_seq = seq;
            }
        }

        if (max_seq > last_seq) {
            upsertCursorSeq(peer_id, db_guid, max_seq);
            DebugL << "Updated trans seq for peer=" << peer_id << ", db=" << db_guid << ", cursor=" << max_seq;
        }
    };

    GET_CONFIG(int, batch_limit, Database::kBatchLimit);
    auto flag = NOTICE_EMIT(BroadcastSyncChangesArgs, Broadcast::kBroadcastSyncChanges, base_url, peer_id, db_guid, last_seq, batch_limit, onRes);
    if (!flag) {
        WarnL << "No listener for kBroadcastSyncChanges, cannot pull sync changes from " << peer_id;
    }
}

void SyncManager::applyBatch(const Json::Value &rows) {
    auto log_imp = make_shared<TransactionLogImp>();

    for (const auto &it : rows) {
        int tran_type = it["tran_type"].asInt();
        if (tran_type == static_cast<int>(TranType::DataMutation)) {
            // tran_data is a JSON string: {"table":"...","op":"UPSERT|DELETE","payload":{...}}
            string raw = it["tran_data"].asString();
            Json::Value tran_data;
            if (!StrJsonUtils::readJsonString(raw, tran_data)) {
                WarnL << "SyncDB applyBatch: invalid tran_data JSON, skipping row";
                continue;
            }

            string table              = tran_data["table"].asString();
            string op                 = tran_data["op"].asString();
            const Json::Value &payload = tran_data["payload"];

            if (!shouldApply(table, payload)) {
                continue; // skip based on conflict resolution
            }

            if (table == EntityTraits<VmsResource>::tableName()) {
                auto imp = make_shared<VmsResourceImp>();
                if (op == TRAN_DATA_OP_UPSERT) {
                    auto r = jsonToVmsResource(payload);
                    imp->add(r, false); // remote data — do not append to local transaction_log
                } else if (op == TRAN_DATA_OP_DELETE) {
                    string guid = payload["guid"].asString();
                    if (!guid.empty()) imp->remove(guid, false); // remote data — do not re-log
                } else {
                    WarnL << "SyncDB applyBatch: unknown op '" << op << "' for " << table;
                    continue;
                }
            } else if (table == EntityTraits<VmsKvPair>::tableName()) {
                auto imp = make_shared<VmsKvPairImp>();
                if (op == TRAN_DATA_OP_UPSERT) {
                    auto kv = jsonToVmsKvPair(payload);
                    imp->add(kv, false); // remote data — do not append to local transaction_log
                } else if (op == TRAN_DATA_OP_UPSERT_BATCH) {
                    std::vector<VmsKvPair> kvs;
                    for (const auto &item : payload["items"]) {
                        kvs.push_back(jsonToVmsKvPair(item));
                    }
                    imp->addBatch(kvs, false);
                } else if (op == TRAN_DATA_OP_DELETE) {
                    string resource_guid = payload["resource_guid"].asString();
                    if (!resource_guid.empty()) imp->remove(resource_guid, false); // remote data — do not re-log
                } else {
                    WarnL << "SyncDB applyBatch: unknown op '" << op << "' for " << table;
                    continue;
                }
            } else if (table == EntityTraits<VmsResourceAssignment>::tableName()) {
                auto imp = make_shared<VmsResourceAssignmentImp>();
                if (op == TRAN_DATA_OP_UPSERT) {
                    auto assign = jsonToVmsResourceAssignment(payload);
                    imp->add(assign, false); // remote data — do not append to local transaction_log
                } else if (op == TRAN_DATA_OP_DELETE) {
                    string assignment_guid = payload["assignment_guid"].asString();
                    if (!assignment_guid.empty()) imp->remove(assignment_guid, false); // remote data — do not re-log
                } else {
                    WarnL << "SyncDB applyBatch: unknown op '" << op << "' for " << table;
                    continue;
                }
            } else {
                WarnL << "SyncDB applyBatch: unknown table '" << table << "', skipping";
                continue;
            }
        } else {
            WarnL << "SyncDB applyBatch: unknown tran_type " << tran_type << ", skipping row";
            continue;
        }

        // Record the transaction in local log, update sequence of the remote peer
        auto log = jsonToTransactionLog(it);
        log_imp->add(log);
    }
}

bool SyncManager::shouldApply(const string & /*table*/, const Json::Value & /*row*/) {
    // Transaction-log sequence is the authoritative ordering; always apply
    return true;
}

} // namespace managerkit