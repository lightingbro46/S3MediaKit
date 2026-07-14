#ifndef STORAGE_SYNC_MANAGER_H
#define STORAGE_SYNC_MANAGER_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>
#include <json/json.h>
#include "Poller/Timer.h"
#include "Extension/TableSyncHandler.h"

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
    Json::Value  resource_assignment; // Json::arrayValue
    Json::Value  bookmark_index;      // Json::arrayValue — cluster-wide bookmark summary

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
    using DoneCb      = std::function<void(bool ok)>;

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

    Json::Value getCurrentCursors(const Json::Value &cursors_json, int limit);

    void recordRelayAck(const Json::Value &ack_cursors_json);

    void setSingleNodeMode(bool single_node);

    Json::Value getMiscData();

private:
    SyncManager() = default;

    // ── Pull tick ──────────────────────────────
    void onTick();

    // ── Incremental pull (gossip relay) ────────
    // Sends the full local cursor map to relay_peer_id, which returns log entries
    // for ALL peers it has accumulated (not just its own). Peer discovery is
    // implicit: new peers appear as relay entries and their cursors are
    // automatically registered when applyBatch processes the entries.
    void pullFromRelay(const std::string &relay_peer_id, const std::string &base_url);
    void applyBatch(const Json::Value &rows);

    // ── Bootstrap path ─────────────────────────
    // Returns true unless the persisted bootstrap state is "completed".
    bool needsBootstrap();
    // Persist completion metadata, including snapshot source and row counts.
    void markBootstrapDone(const SnapshotData &snap);
    // Full bootstrap from a single peer: fetches all tables, used for initial cluster join.
    void doBootstrap(const std::string &peer_id, const std::string &base_url, DoneCb cb = nullptr);
    void applySnapshot(const SnapshotData &snap);

    // ── Conflict resolution ─────────────────────
    // 3-way decision for incoming log entries:
    //   Skip        — local version is newer, discard incoming
    //   Apply       — no prior entry for this row, apply normally
    //   ApplyWinner — conflict detected, incoming wins; caller sets timestamp_hi=1
    enum class ApplyDecision : int { Skip = 0, Apply = 1, ApplyWinner = 2 };

    // Last-Write-Wins by wall-clock timestamp (TransactionLog::timestamp, ms).
    // Updates _applied_ts on accept (Apply / ApplyWinner).
    ApplyDecision shouldApply(const std::string &table_name, const std::string &op, const Json::Value &payload, int64_t log_ts);

    // Register a sync handler for a table. Must be called before start().
    // Replaces any previously registered handler for the same table name.
    void registerTable(const std::string &table_name, TableSyncHandler handler);

    // GC: compute safe prune watermark and delete old transaction_log entries.
    void maybePruneLog();

private:
    toolkit::Timer::Ptr                          _timer;
    std::mutex                                   _mtx;
    std::unordered_map<std::string, std::string> _peers; // node_id → base_url
    std::unordered_set<std::string>              _bootstrapping; // guard: bootstrap in progress
    std::string                                  _last_bootstrap_peer; // for logging
    // Set to true once the initial full-snapshot bootstrap has completed.
    // After this, onTick() only does incremental pulls from all peers.
    bool                                         _bootstrapped = false;
    std::string                                  _self_node_id; // media server's own node_id
    std::string                                  _self_db_id; // media server's own db_guid
    bool                                         _running      = false;
    uint64_t                                     _last_prune_time = 0;

    // LWW map: key = "table:row_key" → last accepted log_ts (ms).
    // Prevents stale writes when two nodes concurrently modify the same row.
    // Volatile (cleared on restart); safe because the pull cursor prevents
    // re-applying already-processed log entries after a clean restart.
    std::unordered_map<std::string, int64_t>     _applied_ts;

    // Plugin registry: table_name → sync handler.
    std::unordered_map<std::string, TableSyncHandler> _table_handlers;
    bool _single_node = false;
};

} // namespace managerkit

#endif // STORAGE_SYNC_MANAGER_H
