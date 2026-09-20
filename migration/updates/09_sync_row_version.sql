-- sync_row_version: persistent last-writer-wins state for replicated ESC rows.
-- One row per (table_name, row_key): the version (ts_ms, peer_guid) of the last accepted write,
-- local or remote. Versions are ordered by ts_ms, then peer_guid (deterministic tie-break).
-- deleted = 1 marks a tombstone: an older UPSERT for the same key must not resurrect the row.
-- group_key links rows removed together by one DELETE (e.g. all vms_kvpair rows of a resource);
-- the group tombstone itself is stored with row_key = '#group:' || group_key.
CREATE TABLE IF NOT EXISTS sync_row_version (
    table_name  TEXT    NOT NULL,
    row_key     TEXT    NOT NULL,
    group_key   TEXT    NOT NULL DEFAULT '',
    ts_ms       INTEGER NOT NULL,
    peer_guid   TEXT    NOT NULL,
    deleted     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (table_name, row_key)
);

CREATE INDEX IF NOT EXISTS idx_row_version_group   ON sync_row_version (table_name, group_key);
CREATE INDEX IF NOT EXISTS idx_row_version_deleted ON sync_row_version (deleted, ts_ms);
