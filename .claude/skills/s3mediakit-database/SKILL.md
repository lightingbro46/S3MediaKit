---
name: s3mediakit-database
description: "Use when changing S3MediaKit persistence — SQLite schema, numbered migrations under migration/, manager/Storage entities, Repository/Imp classes, transactions, FTS tables, or multi-node table synchronization via SyncManager and TableSyncHandler."
---

# S3MediaKit database and manager storage

Inspect `manager/Storage/`, `manager/Storage/DbStorage.{h,cpp}`,
`manager/Extension/SyncManager.cpp` and both migration directories before changing anything.

## Two databases

| Database | Handle | Migrations |
|---|---|---|
| Edge Storage Controller | `kEdgeStorageControllerDb` | `migration/updates/` |
| MediaServer | `kMediaServerDb` | `migration/mserver_updates/` |

Pick the wrong one and the table silently lands on the wrong node. `MigrationHistory`
records applied files.

## Layering — enforced, not advisory

| Layer | Owns | Never does |
|---|---|---|
| `<Entity>Repository` | parameterized SQL, row↔struct mapping, CRUD primitives, `SqliteTransaction` | validation, defaults, business rules |
| `<Entity>Imp` | input validation, normalization/defaults, generated IDs, timestamps, JSON↔domain parsing, computed fields, state transitions | raw SQL |
| Manager/service | cross-component orchestration, caches, external I/O, policy | direct Repository use |

- Production callers use `*Imp`. Constructing a `*Repository` directly is for
  Repository-level tests and persistence infrastructure only.
- Never write `using FooImp = FooRepository;`. Define a real `Imp` even if its first version
  is a thin facade — domain behavior needs one stable home.
- Entities live one-per-header in `manager/Storage/` (row struct + Repository + Imp together);
  `DbSchema.h` holds `Optional<T>` and shared schema types.

## Migration workflow

1. Choose the database (table above).
2. Find the highest numeric prefix in that directory and add exactly one
   `NN_snake_case_description.sql`, zero-padded. **Never edit an applied migration.**
3. Make it rerunnable: `CREATE TABLE IF NOT EXISTS`, `CREATE INDEX IF NOT EXISTS`,
   idempotent seeds, explicit drop+recreate for triggers and FTS tables.
4. Preserve data. Do not drop a live table; use `ALTER TABLE ADD COLUMN`, or a complete
   rename → copy → drop plan that keeps every existing row.
5. Update the C++ side consistently: row struct, Repository queries and mapping, JSON
   conversion, and any transaction path that touches the table.
6. If the table replicates, implement `makeSyncHandler()` on the `Imp` and call
   `SyncManager::registerTable()` at startup (see below).
7. Verify: ordering, SQL syntax, rerun behavior, old-row preservation, indexes/triggers,
   Repository/Imp boundary, and that the file is picked up by the right target.

## Sync (`ENABLE_SYNC_CHANGES`)

`TableSyncHandler` (`manager/Extension/TableSyncHandler.h`) is the plugin point:

```cpp
TableSyncHandler h;
h.rowKey            = [](const Json::Value &p) { return p["id"].asString(); }; // "" = always apply
h.onUpsert          = ...;
h.onUpsertBatch     = ...;   // may be nullptr
h.onDelete          = ...;
h.onSnapshot        = ...;   // receives the whole JSON array during bootstrap
h.onUpsertWithTxn   = ...;   // used when a batch runs inside one SqliteTransaction
SyncManager::Instance().registerTable("my_table", h);
```

- Conflict resolution is last-writer-wins on `rowKey`, so payloads must carry their own
  timestamps and a stale row must be rejected before it is written.
- `*WithTxn` variants fall back to the non-txn variant when null — supply them for any table
  that can appear in a large batch.
- Only ESC tables participate in the current design. Node-local statistics are deliberately
  **not** replicated; do not add them.
- Transaction bookkeeping lives in `TransactionLog`, `TransactionSequence` and
  `TransactionPeerAckLog`; `maybePruneLog()` trims acknowledged history.
- Protocol: `docs/api-sync.md`; endpoints in `server/WebApiSyncDb.cpp`.

## Threading

All SQLite access is blocking — it belongs on `WorkThreadPool`, never on an `EventPoller`.
Keep transactions short; a long transaction blocks every other writer on that database.

Open transactions through the executor (`getExecutor()`):

- `beginWriteTxn()` — `BEGIN IMMEDIATE`. Use it for every transaction that may write, including
  ones that SELECT first (claim, find-or-create, merge). A deferred transaction that has read
  cannot be upgraded after another connection commits and fails with "database is locked"
  without honoring `busy_timeout`.
- `beginReadTxn()` — deferred, read-only (snapshot build, filtering). Never write through it; it
  must not hold the write lock during a long read.

Regression pattern: hold the write lock from another connection for ~300ms while calling the
function under test (`WriteLockHolder` in `tests/unit/storage/test_tier_txn_lock.cpp`).

## Tests

Storage tests live in `tests/unit/storage/` (`test_db_schema.cpp`, `test_storage_entities.cpp`,
`test_storage_models.cpp`, `test_extract_job.cpp`). Add a case there for every new entity or
migration-visible behavior. See `s3mediakit-testing` for how to build and run them.

Schema conventions, Repository/Imp classification and integrity-risk review:
`.codex/skills/s3mediakit-database/references/storage-conventions.md`.
