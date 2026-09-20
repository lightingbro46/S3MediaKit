#ifndef STORAGE_SYNC_MANAGER_H
#define STORAGE_SYNC_MANAGER_H

#ifdef ENABLE_SYNC_CHANGES

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <mutex>
#include <functional>
#include <json/json.h>
#include "Poller/Timer.h"
#include "Poller/EventPoller.h"
#include "Extension/TableSyncHandler.h"
#include "Storage/SyncRowVersion.h"
#include "Storage/TransactionLog.h"

namespace managerkit {

/**
 * SnapshotData is the data structure used to store the snapshot of database. 
 * It contains the peer_id and db_guid of the media server. 
 * The SnapshotData structure is used to store the snapshot of a ESC database
 */
struct SnapshotData {
    std::string peer_id;
    std::string db_guid;
    // Transaction_sequence at time of snapshot
    Json::Value  sequences;           // Json::arrayValue
    Json::Value  vms_resource;        // Json::arrayValue
    Json::Value  vms_kvpair;          // Json::arrayValue
    Json::Value  vms_resource_status; // Json::arrayValue
    Json::Value  resource_assignment; // Json::arrayValue
    Json::Value  bookmark_index;      // Json::arrayValue — cluster-wide bookmark summary
    // sync_row_version (LWW state). Optional: snapshots from older nodes do not carry it.
    Json::Value  row_versions;        // Json::arrayValue

    bool empty() const { return sequences.empty(); }
};

/**
 * SnapshotBuilder is the class used to build the snapshot of database.
 * Read database in one transaction, used for Web Api and SyncManager. 
 */
class SnapshotBuilder {
public:
    SnapshotBuilder() = delete;
    ~SnapshotBuilder() = delete;

    // Build the snapshot of database and return the SnapshotData structure
    static SnapshotData build(const std::string &node_id, const std::string &db_guid, const std::string &db_tag);

    // Serialize the SnapshotData structure to a string, which can be used to transmit the snapshot data to other media servers
    static std::string serialize(const SnapshotData &snap);

    // Deserialize the string to SnapshotData structure, which can be used to update the database with the snapshot data
    static SnapshotData deserialize(const std::string &json_body);
};

/**
 * SyncManager: orchestrate pull loop + bootstrap
 */
class SyncManager : public std::enable_shared_from_this<SyncManager> {
public:
    using Ptr         = std::shared_ptr<SyncManager>;
    // Outcome of one bootstrap attempt: done, the peer has not completed its own bootstrap yet,
    // or the request/apply failed.
    enum class BootstrapResult : int { Done = 0, NotReady = 1, Failed = 2 };
    using DoneCb      = std::function<void(BootstrapResult result)>;

    // Answers of each peer to bootstrap attempts since the last successful bootstrap.
    struct BootstrapProbe {
        int not_ready = 0;
        int failed = 0;
    };

    // A relay returned entries of a source that do not follow the local cursor.
    struct SyncGap {
        std::string peer_guid;
        std::string db_guid;
        int cursor = 0;     // last sequence applied from this source
        int first_seq = 0;  // first sequence the relay offered
    };

    static SyncManager &Instance();
    ~SyncManager() = default;

    // Start the sync process.
    void start();
    void stop();

    // Manager peer nodes (when cluster membership changes)
    void addPeer(const std::string &peer_id, const std::string &base_url);
    void removePeer(const std::string &peer_id);

    // Expose for HTTP handler: build snapshot of local DB
    SnapshotData buildLocalSnapshot();

    // Whether this node's ESC database is complete (bootstrap done or seeded). A node that is not
    // must not serve its snapshot for another node's bootstrap.
    bool isBootstrapped();

    // Seed election when no peer has completed bootstrap: true once every peer was tried at least
    // `rounds` times, at least one answered "not bootstrapped yet", and `self_id` is lower than the
    // id of every peer that answered so. Peers that only failed to connect do not take part.
    static bool shouldSeedBootstrap(const std::string &self_id, const std::vector<std::string> &peers,
                                    const std::unordered_map<std::string, BootstrapProbe> &probes, int rounds);

    Json::Value getCurrentCursors(const Json::Value &cursors_json, int limit);

    void recordRelayAck(const Json::Value &ack_cursors_json);

    void setSingleNodeMode(bool single_node);

    Json::Value getMiscData();

    // Called for every local mutation before it is appended to transaction_log (ENABLE_SYNC_CHANGES).
    // Records the LWW versions of the affected rows and returns the log timestamp to use:
    // >= now_ms and newer than any version already known for those rows, so a lagging clock
    // cannot make a local write lose against data this node has already accepted.
    int64_t stampLocalMutation(const std::string &table, const std::string &op, const Json::Value &payload,
                               int64_t now_ms, const std::string &self_peer);

private:
    friend class SyncManagerTestPeer; // unit tests drive applyBatch/applySnapshot directly

    SyncManager();

    // ── Pull tick ──────────────────────────────
    void onTick();

    // ── Incremental pull (gossip relay) ────────
    // Sends the full local cursor map to relay_peer_id, which returns log entries
    // for ALL peers it has accumulated (not just its own). Peer discovery is
    // implicit: new peers appear as relay entries and their cursors are
    // automatically registered when applyBatch processes the entries.
    void pullFromRelay(const std::string &relay_peer_id, const std::string &base_url);

    // Applies a relay batch. Entries after a gap are not applied (the cursor would jump past the
    // missing ones); the detected gaps are returned.
    std::vector<SyncGap> applyBatch(const Json::Value &rows);
    // Applies one log entry inside `txn`; returns its TranAuditFlag.
    int applyEntryWithTxn(const TransactionLog &log, SyncRowVersionImp &versions, toolkit::SqliteTransaction::Ptr txn);
    // Re-applies entries stored as TRAN_AUDIT_DEFERRED that this build understands. Returns the count.
    size_t reapplyDeferredEntries();
    // A gap that persists (same cursor) for 3 pull intervals means no peer can provide the missing
    // entries any more: request a full resync from a snapshot.
    void handleGaps(const std::vector<SyncGap> &gaps, uint64_t now_ms);
    // Marks the persisted bootstrap state pending; the next tick bootstraps from a peer.
    void requestResync(const std::string &reason);
    // Peers whose acks bound log pruning: healthy peers, plus known peers that acked within
    // `offline_ttl_sec` (they can still resume incrementally).
    static std::unordered_set<std::string> selectPruneWatermarkPeers(
        const std::unordered_set<std::string> &healthy, const std::vector<std::string> &known,
        const std::unordered_map<std::string, int64_t> &last_ack_sec, int64_t now_sec, int64_t offline_ttl_sec);

    // ── Bootstrap path ─────────────────────────
    // Returns true unless the persisted bootstrap state is "completed".
    bool needsBootstrap();
    // Persist completion metadata, including snapshot source and row counts.
    void markBootstrapDone(const SnapshotData &snap);
    // Persist completion as the seed of a new cluster (no peer had completed bootstrap).
    void markBootstrapSeeded(const std::string &self_id);
    // Full bootstrap from a single peer: fetches all tables, used for initial cluster join.
    void doBootstrap(const std::string &peer_id, const std::string &base_url, DoneCb cb = nullptr);
    void applySnapshot(const SnapshotData &snap);
    // Runs on the sync worker: validates and applies a bootstrap snapshot response.
    void onBootstrapResponse(const std::string &peer_id, const std::string &err, const Json::Value &data, const DoneCb &cb);

    // Single WorkThreadPool poller that owns all SyncManager DB work (tick, apply, prune),
    // so blocking SQLite calls never run on network pollers and batches are applied serially.
    toolkit::EventPoller::Ptr getWorker();

    // ── Conflict resolution ─────────────────────
    // 3-way decision for incoming log entries:
    //   Skip        — local version is newer, discard incoming
    //   Apply       — no prior entry for this row, apply normally
    //   ApplyWinner — conflict detected, incoming wins; caller sets timestamp_hi=1
    enum class ApplyDecision : int { Skip = 0, Apply = 1, ApplyWinner = 2 };

    // Last-writer-wins on the persistent row version (log timestamp, origin peer_guid), read and
    // written inside the batch transaction `txn`. Produces the payload to apply (stale items of an
    // UPSERT_BATCH removed) and, for a group DELETE, the row keys that must survive it.
    ApplyDecision resolveConflict(const std::string &table, const TableSyncHandler &h, const std::string &op,
                                  const Json::Value &payload, int64_t ts, const std::string &peer,
                                  SyncRowVersionImp &versions, toolkit::SqliteTransaction::Ptr txn,
                                  Json::Value &apply_payload, std::set<std::string> &keep_keys);

    static SyncRowVersion makeVersion(const std::string &table, const std::string &key, const std::string &group,
                                      int64_t ts, const std::string &peer, bool deleted);

    // Register a sync handler for a table. Called from the constructor, so handlers exist
    // before any local mutation or remote batch is processed; read-only afterwards.
    void registerTable(const std::string &table_name, TableSyncHandler handler);

    // GC: compute safe prune watermark and delete old transaction_log entries.
    void maybePruneLog();

private:
    toolkit::Timer::Ptr                          _timer;
    toolkit::EventPoller::Ptr                    _worker;
    std::mutex                                   _mtx;
    // Serializes applyBatch/applySnapshot; normally they already run on _worker, the lock also
    // covers direct callers such as unit tests.
    std::mutex                                   _apply_mtx;
    std::unordered_map<std::string, std::string> _peers; // node_id → base_url
    std::unordered_set<std::string>              _bootstrapping; // guard: bootstrap in progress
    std::string                                  _last_bootstrap_peer; // for logging
    // Answers per peer while bootstrapping, for the seed election (cleared once bootstrapped).
    std::unordered_map<std::string, BootstrapProbe> _bootstrap_probes;
    // Set to true once the initial full-snapshot bootstrap has completed.
    // After this, onTick() only does incremental pulls from all peers.
    bool                                         _bootstrapped = false;
    std::string                                  _self_node_id; // media server's own node_id
    std::string                                  _self_db_id; // media server's own db_guid
    bool                                         _running      = false;
    uint64_t                                     _last_prune_time = 0;
    // Gap tracking (sync worker only): source "peer|db" → (cursor when first seen, first seen ms).
    std::unordered_map<std::string, std::pair<int, uint64_t>> _gap_state;

    // Plugin registry: table_name → sync handler.
    std::unordered_map<std::string, TableSyncHandler> _table_handlers;
    bool _single_node = false;
};

} // namespace managerkit

#endif // ENABLE_SYNC_CHANGES

#endif // STORAGE_SYNC_MANAGER_H
