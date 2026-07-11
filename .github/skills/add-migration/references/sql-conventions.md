# SQL Migration Conventions

## Table definitions

```sql
-- Always use IF NOT EXISTS to allow safe re-run
CREATE TABLE IF NOT EXISTS "my_table" (
    "id"          TEXT    NOT NULL PRIMARY KEY,      -- GUID/UUID
    "name"        TEXT    NOT NULL,
    "description" TEXT    NOT NULL DEFAULT '',       -- optional string → DEFAULT ''
    "status"      TEXT    NOT NULL DEFAULT 'ACTIVE',
    "enabled"     INTEGER NOT NULL DEFAULT 1,        -- bool as INTEGER
    "created_at"  INTEGER NOT NULL DEFAULT 0,        -- unix timestamp
    "updated_at"  INTEGER NOT NULL DEFAULT 0
);
```

## Sequential integer PK
```sql
"id" INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
```

## Composite PK (no AUTOINCREMENT)
```sql
CREATE TABLE IF NOT EXISTS "my_junction" (
    "resource_id" TEXT NOT NULL,
    "policy_id"   TEXT NOT NULL,
    PRIMARY KEY ("resource_id", "policy_id")
);
```

## Indexes — always after the table
```sql
CREATE INDEX IF NOT EXISTS idx_my_table_name   ON my_table (name);
CREATE UNIQUE INDEX IF NOT EXISTS idx_my_table_id ON my_table (id);
```

## Adding a column to an existing table (ALTER TABLE)
```sql
ALTER TABLE existing_table ADD COLUMN new_col TEXT NOT NULL DEFAULT '';
```

## Seed / lookup data
```sql
INSERT OR IGNORE INTO my_lookup (id, name) VALUES
    (1, 'VALUE_A'),
    (2, 'VALUE_B');
```

## Recreating a table (data-preserving pattern)
```sql
-- 1. Rename old table
ALTER TABLE my_table RENAME TO my_table_old;
-- 2. Create new table with updated schema
CREATE TABLE IF NOT EXISTS "my_table" ( ... );
-- 3. Copy data
INSERT INTO my_table SELECT id, name, ... FROM my_table_old;
-- 4. Drop old table
DROP TABLE IF EXISTS my_table_old;
```

## FTS5 virtual tables (full-text search)
```sql
DROP TABLE IF EXISTS my_table_fts;
CREATE VIRTUAL TABLE my_table_fts USING fts5(
    id UNINDEXED,
    search_text,
    content='my_table',
    tokenize='trigram'
);

-- Backfill
INSERT INTO my_table_fts(rowid, id, search_text)
    SELECT rowid, id, COALESCE(name, '') || ' ' || COALESCE(description, '')
    FROM my_table;
```

## Triggers (maintain FTS in sync)
```sql
DROP TRIGGER IF EXISTS my_table_ai;
DROP TRIGGER IF EXISTS my_table_ad;
DROP TRIGGER IF EXISTS my_table_au;

CREATE TRIGGER my_table_ai AFTER INSERT ON my_table BEGIN
    INSERT INTO my_table_fts(rowid, id, search_text)
    VALUES (new.rowid, new.id, new.name || ' ' || new.description);
END;

CREATE TRIGGER my_table_ad AFTER DELETE ON my_table BEGIN
    INSERT INTO my_table_fts(my_table_fts, rowid, id, search_text)
    VALUES ('delete', old.rowid, old.id, old.name || ' ' || old.description);
END;

CREATE TRIGGER my_table_au AFTER UPDATE ON my_table BEGIN
    INSERT INTO my_table_fts(my_table_fts, rowid, id, search_text)
    VALUES ('delete', old.rowid, old.id, old.name || ' ' || old.description);
    INSERT INTO my_table_fts(rowid, id, search_text)
    VALUES (new.rowid, new.id, new.name || ' ' || new.description);
END;
```

## Things to NEVER do in a migration
- `DROP TABLE existing_data_table` — use ALTER TABLE or rename+copy instead
- `CREATE TABLE` without `IF NOT EXISTS`
- Raw `INSERT` without `OR IGNORE` / `OR REPLACE` when data might already exist
- Hard-coding absolute file paths or server-specific data
