#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <json/json.h>
#include "Util/SqlitePool.h"

namespace managerkit {

/**
 * TableSyncHandler — plugin interface that connects a storage entity to SyncManager.
 *
 * Each sync-able entity registers exactly one handler via SyncManager::registerTable().
 * SyncManager dispatches applyBatch() and applySnapshot() through these handlers
 * instead of hardcoded if/else chains, so adding a new synced table requires only:
 *   1. Implement makeSyncHandler() on the Imp class.
 *   2. Call SyncManager::registerTable() at startup.
 */
struct TableSyncHandler {
    // Return a stable per-row LWW key derived from an UPSERT/DELETE payload, or from one item of an
    // UPSERT_BATCH payload ({"items":[...]}). Versions are persisted in sync_row_version.
    // nullptr: no LWW tracking for this table (always apply).
    std::function<std::string(const Json::Value &payload)> rowKey;

    // Multi-row groups (e.g. all vms_kvpair rows of one resource). Returns the group of a row/item,
    // or of a DELETE payload that removes the whole group. A group DELETE leaves a group tombstone
    // and only removes rows written before it. nullptr: rows are independent.
    std::function<std::string(const Json::Value &payload)> groupKey;

    // Group DELETE inside the batch transaction: remove the group's rows except those whose
    // rowKey is in `keep` (they were written after the delete). Required when groupKey is set.
    std::function<void(const Json::Value &payload, const std::set<std::string> &keep,
                       toolkit::SqliteTransaction::Ptr txn)> onGroupDeleteWithTxn;

    // Handle UPSERT op (single-row).
    std::function<void(const Json::Value &payload)> onUpsert;

    // Handle UPSERT_BATCH op. May be nullptr when the table does not support batch.
    std::function<void(const Json::Value &payload)> onUpsertBatch;

    // Handle DELETE op.
    std::function<void(const Json::Value &payload)> onDelete;

    // Apply a full snapshot array for this table (used during bootstrap).
    // Receives the entire JSON array for the table — iterate and upsert each row.
    std::function<void(const Json::Value &json_array)> onSnapshot;

    // Transaction-aware variants. Called by applyBatch when a batch transaction
    // is active. If nullptr, applyBatch falls back to the non-txn variant.
    std::function<void(const Json::Value &payload, toolkit::SqliteTransaction::Ptr txn)> onUpsertWithTxn;
    std::function<void(const Json::Value &payload, toolkit::SqliteTransaction::Ptr txn)> onUpsertBatchWithTxn;
    std::function<void(const Json::Value &payload, toolkit::SqliteTransaction::Ptr txn)> onDeleteWithTxn;
};

} // namespace managerkit
