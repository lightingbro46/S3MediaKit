---
name: s3mediakit-database
description: "Use when changing S3MediaKit SQLite schemas, numbered migrations, manager storage entities, repositories, transaction behavior, FTS tables, or multi-node table synchronization."
---

# S3MediaKit database and manager storage

Inspect `manager/Storage`, `manager/Extension/SyncManager.cpp`, and both migration directories before changing persistence.

## Migration workflow

1. Choose the database: ESC uses `migration/updates/` and `Database::kEdgeStorageControllerDb`; MediaServer uses `migration/mserver_updates/` and `Database::kMediaServerDb`.
2. Find the highest existing numeric prefix and add one new zero-padded `NN_snake_case_description.sql` file. Never edit an applied migration.
3. Make the migration rerunnable where possible: `CREATE TABLE IF NOT EXISTS`, idempotent indexes/seeds, and explicit trigger/FTS recreation when needed.
4. Preserve existing data. Do not drop a live data table; use `ALTER TABLE` or rename/copy/drop with a complete data-preserving plan.
5. If a table maps to C++, update the entity macro, repository, JSON conversion, and transaction paths consistently.
6. If the table is replicated, implement/register `TableSyncHandler`; only ESC tables participate in the existing cluster sync design.
7. Validate ordering, SQL syntax, rerun behavior, old-data preservation, indexes/triggers, and the owning C++ target.

See [storage-conventions.md](references/storage-conventions.md) for schema and sync details.
