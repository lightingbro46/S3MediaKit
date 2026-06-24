#pragma once

#include <functional>
#include <memory>
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
    // Return a stable per-row LWW key derived from the UPSERT payload.
    // Return empty string to skip LWW tracking for this table (always apply).
    std::function<std::string(const Json::Value &payload)> rowKey;

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
