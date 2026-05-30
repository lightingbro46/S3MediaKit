#ifndef STORAGE_SYNC_MANAGER_H
#define STORAGE_SYNC_MANAGER_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>
#include <json/json.h>
#include "Poller/Timer.h"
#include "Storage/DbStorage.h"

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
    Json::Value  sequences;       // Json::arrayValue
    Json::Value  vms_resource;    // Json::arrayValue
    Json::Value  vms_kvpair;      // Json::arrayValue
    Json::Value  resource_assignment; // Json::arrayValue

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

private:
    SyncManager() = default;

    // ── Pull tick ──────────────────────────────
    void onTick();

    // ── Incremental pull (normal path) ─────────
    void pullFromPeer(const std::string &peer_id, const std::string &db_guid, const std::string &base_url);
    void applyBatch(const Json::Value &rows);

    // ── Bootstrap path ─────────────────────────
    // Returns true if transaction_sequence table is empty (never synced)
    bool needsBootstrap();
    // Mark bootstrap as done by setting a flag in the database, so that next time onTick() knows to start incremental pull instead of bootstrapping again.
    void markBootstrapDone();
    // Full bootstrap from a single peer: fetches all tables, used for initial cluster join.
    void doBootstrap(const std::string &peer_id, const std::string &base_url, DoneCb cb = nullptr);
    // Mini-bootstrap for a new peer joining an existing cluster: registers cursor only,
    // does NOT apply vms_resource/vms_kvpair to avoid overwriting newer local data.
    void doMiniBootstrap(const std::string &peer_id, const std::string &base_url, DoneCb cb = nullptr);
    void applySnapshot(const SnapshotData &snap);

    // ── Conflict resolution ─────────────────────
    // Last-Write-Wins: compare sequence between local and incoming
    bool shouldApply(const std::string &table_name, const Json::Value &incoming_row);

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
};

} // namespace managerkit

#endif // STORAGE_SYNC_MANAGER_H