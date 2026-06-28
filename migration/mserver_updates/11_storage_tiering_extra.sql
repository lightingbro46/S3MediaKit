-- Storage Tiering: restore jobs for cold playback workflows
CREATE TABLE IF NOT EXISTS "restore_jobs" (
    "job_id"          TEXT    NOT NULL PRIMARY KEY,
    "camera_id"       TEXT    NOT NULL,
    "source_tier"     TEXT    NOT NULL DEFAULT 'COLD',
    "target_tier"     TEXT    NOT NULL DEFAULT 'HOT',
    "status"          TEXT    NOT NULL DEFAULT 'PENDING',
    "start_time"      INTEGER NOT NULL DEFAULT 0,
    "end_time"        INTEGER NOT NULL DEFAULT 0,
    "total_bytes"     INTEGER NOT NULL DEFAULT 0,
    "processed_bytes" INTEGER NOT NULL DEFAULT 0,
    "reason"          TEXT,
    "error_message"   TEXT,
    "created_at"      INTEGER NOT NULL DEFAULT 0,
    "updated_at"      INTEGER NOT NULL DEFAULT 0
);

-- Storage Tiering: persisted alerts that can be acknowledged by operators
CREATE TABLE IF NOT EXISTS "storage_alerts" (
    "id"           TEXT    NOT NULL PRIMARY KEY,
    "level"        TEXT    NOT NULL,
    "type"         TEXT    NOT NULL,
    "pool_id"      TEXT    NOT NULL,
    "message"      TEXT    NOT NULL,
    "acknowledged" INTEGER NOT NULL DEFAULT 0,
    "created_at"   INTEGER NOT NULL DEFAULT 0
);

-- Storage Tiering: protected/evidence/locked playback ranges
CREATE TABLE IF NOT EXISTS "protected_videos" (
    "protected_id" TEXT    NOT NULL PRIMARY KEY,
    "camera_id"    TEXT    NOT NULL,
    "start_time"   INTEGER NOT NULL DEFAULT 0,
    "end_time"     INTEGER NOT NULL DEFAULT 0,
    "type"         TEXT    NOT NULL,
    "reason"       TEXT,
    "created_at"   INTEGER NOT NULL DEFAULT 0
);
