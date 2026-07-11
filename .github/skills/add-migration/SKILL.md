---
name: add-migration
description: "Add a new SQLite schema migration for the S3MediaKit manager databases. Use when adding a table, column, index, or seed data to the ESC database (migration/updates/) or the MediaServer database (migration/mserver_updates/). Produces the numbered SQL file, reminds about DECLARE_ENTITY and TableSyncHandler registration, and verifies the file ordering. Invoke as: /add-migration <short description> [esc|mserver]"
argument-hint: "<description> [esc|mserver]"
---

# add-migration

Creates a correctly numbered SQL migration file for the S3MediaKit manager layer, then checks whether the new table also requires a C++ `DECLARE_ENTITY` and a `TableSyncHandler` registration.

## When to Use

- Adding a new table to the database
- Adding a column or index to an existing table
- Seeding reference/lookup data (INSERT ... VALUES ...)
- Altering a table structure (use `ALTER TABLE` or recreate with `CREATE TABLE IF NOT EXISTS`)

## Databases

| Tag | Migration folder | C++ constant |
|-----|-----------------|--------------|
| **ESC** (default) | `migration/updates/` | `Database::kEdgeStorageControllerDb` |
| **MediaServer** | `migration/mserver_updates/` | `Database::kMediaServerDb` |

When the target is not specified, ask or infer from context:
- Storage tiering, sync, bookmarks, VMS resources → **ESC**
- Auth, sessions, certifications, audit logs, system metrics, HLS/MP4 bookmarks → **MediaServer**

## Procedure

### Step 1 — Determine the next file number

List the existing files in the target folder to find the highest prefix, then increment:
- ESC: `migration/updates/`
- MediaServer: `migration/mserver_updates/`

File naming convention: `NN_snake_case_description.sql` where `NN` is zero-padded to 2 digits.

Example: if the last file is `11_storage_tiering_extra.sql`, the new file is `12_my_table.sql`.

### Step 2 — Write the SQL file

See [./references/sql-conventions.md](./references/sql-conventions.md) for full conventions.

Key rules:
- Use `CREATE TABLE IF NOT EXISTS` (never bare `CREATE TABLE`) — migrations can be re-run.
- Use `DROP TABLE IF EXISTS` / `DROP TRIGGER IF EXISTS` / `DROP INDEX IF EXISTS` before recreating to allow safe re-run.
- Add indexes after the table definition, one per line.
- Use `INTEGER NOT NULL DEFAULT 0` for timestamps; `TEXT NOT NULL DEFAULT ''` for optional strings.
- Primary keys: `TEXT NOT NULL PRIMARY KEY` for GUID/UUID, `INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT` for sequential IDs.
- Multi-column primary keys: use `PRIMARY KEY (col1, col2)` at the end of the table definition.
- **Never** `DROP TABLE` an existing data table in a migration — add columns with `ALTER TABLE` or recreate with `CREATE TABLE IF NOT EXISTS` and a data copy.

### Step 3 — Check C++ entity wiring

After creating the SQL file, check whether the new table needs a matching C++ struct.

If yes, remind the user to:
1. Add the struct + `DECLARE_ENTITY(...)` in `manager/Storage/MyEntity.h` (see [manager instructions](../../instructions/manager.instructions.md)).
2. Add the repository class `MyEntityRepository : public SqliteRepository<MyEntity>`.

### Step 4 — Check sync handler

Ask: **Should this table be replicated across cluster nodes via gossip sync?**

If yes:
1. Implement `static TableSyncHandler makeSyncHandler()` on the Imp class.
2. Call `SyncManager::Instance().registerTable(EntityTraits<MyEntity>::tableName(), MyEntityImp::makeSyncHandler())` in `manager/Extension/SyncManager.cpp` inside the existing block that registers other tables (around line 221).

Only tables in the **ESC** database can be synced. MediaServer tables are not synced.

### Step 5 — Verify

Read back the directory listing and confirm:
- The new file has the correct numeric prefix (no gaps, no duplicates).
- The filename uses lowercase snake_case.
- `CREATE TABLE IF NOT EXISTS` is present.
- If a new entity was created, `DECLARE_ENTITY` is present in the `.h` file.

## Quick Reference

```
migration/updates/           ← ESC database
  01_migrationhistory.sql
  02_userentity.sql
  ...
  08_bookmark_index.sql
  09_my_new_table.sql        ← new file goes here

migration/mserver_updates/   ← MediaServer database
  01_migrationhistory.sql
  02_bookmarks.sql
  ...
  11_storage_tiering_extra.sql
  12_my_new_table.sql        ← new file goes here
```
