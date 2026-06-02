-- bookmark_index: cluster-wide summary index stored in ESC DB (synced across all nodes).
-- Full detail (description, tags, FTS) remains in the per-node kMediaServerDb bookmarks table.
-- owner_peer_id records which node holds the full detail for each bookmark.
CREATE TABLE IF NOT EXISTS bookmark_index (
    bookmark_guid  TEXT NOT NULL PRIMARY KEY,
    camera_guid    TEXT NOT NULL,
    owner_peer_id  TEXT NOT NULL,
    start_time     INTEGER NOT NULL,
    end_time       INTEGER NOT NULL,
    name           TEXT NOT NULL DEFAULT '',
    description    TEXT NOT NULL DEFAULT '',
    tags_csv       TEXT NOT NULL DEFAULT '',
    creator_guid   TEXT NOT NULL DEFAULT '',
    created        INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_bk_idx_camera_time ON bookmark_index (camera_guid, start_time, end_time);
CREATE INDEX IF NOT EXISTS idx_bk_idx_time        ON bookmark_index (start_time, end_time);
CREATE INDEX IF NOT EXISTS idx_bk_idx_peer        ON bookmark_index (owner_peer_id);

CREATE VIRTUAL TABLE bookmark_index_fts USING fts5(
    bookmark_guid UNINDEXED,
    search_text,                     -- name || ' ' || description || ' ' || tags_csv
    content='bookmark_index',
    tokenize='trigram'
);
-- Backfill + triggers (AFTER INSERT/UPDATE/DELETE on bookmark_index)
