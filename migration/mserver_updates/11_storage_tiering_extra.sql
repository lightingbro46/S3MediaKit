-- Preserve the policy context that admitted each durable tiering job.
ALTER TABLE "tiering_jobs"
    ADD COLUMN "job_trigger" TEXT NOT NULL DEFAULT 'LEGACY';

ALTER TABLE "tiering_jobs"
    ADD COLUMN "policy_id" TEXT NOT NULL DEFAULT '';

ALTER TABLE "tiering_jobs"
    ADD COLUMN "policy_updated_at" INTEGER NOT NULL DEFAULT 0;

-- Pressure jobs used a durable ID prefix before job_trigger was persisted.
UPDATE "tiering_jobs"
SET "job_trigger" = 'PRESSURE'
WHERE "job_trigger" = 'LEGACY'
  AND "job_id" LIKE 'ptj-%';

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

CREATE INDEX IF NOT EXISTS "idx_restore_jobs_status_created"
    ON "restore_jobs" ("status", "created_at");

CREATE INDEX IF NOT EXISTS "idx_restore_jobs_camera_window"
    ON "restore_jobs" ("camera_id", "start_time", "end_time", "updated_at" DESC);

-- Durable per-segment restore work. Parent restore_jobs rows are summaries only.
CREATE TABLE IF NOT EXISTS "restore_job_segments" (
    "job_id"          TEXT    NOT NULL,
    "camera_id"       TEXT    NOT NULL,
    "stream_id"       TEXT    NOT NULL,
    "range_id"        TEXT    NOT NULL,
    "segment_path"    TEXT    NOT NULL,
    "source_pool_id"  TEXT    NOT NULL,
    "range_start"     INTEGER NOT NULL DEFAULT 0,
    "range_end"       INTEGER NOT NULL DEFAULT 0,
    "status"          TEXT    NOT NULL DEFAULT 'PENDING',
    "total_bytes"     INTEGER NOT NULL DEFAULT 0,
    "processed_bytes" INTEGER NOT NULL DEFAULT 0,
    "error_message"   TEXT,
    "created_at"      INTEGER NOT NULL DEFAULT 0,
    "updated_at"      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY ("job_id", "stream_id", "segment_path")
);

CREATE INDEX IF NOT EXISTS "idx_restore_job_segments_status_created"
    ON "restore_job_segments" ("status", "created_at");

CREATE INDEX IF NOT EXISTS "idx_restore_job_segments_job"
    ON "restore_job_segments" ("job_id", "status");

-- Physical deletion and playback-metadata deletion are deliberately separated.
CREATE TABLE IF NOT EXISTS "tier_metadata_cleanup_jobs" (
    "range_id"      TEXT    NOT NULL PRIMARY KEY,
    "camera_id"     TEXT    NOT NULL,
    "status"        TEXT    NOT NULL DEFAULT 'PENDING',
    "attempt_count" INTEGER NOT NULL DEFAULT 0,
    "error_message" TEXT,
    "created_at"    INTEGER NOT NULL DEFAULT 0,
    "updated_at"    INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_tier_metadata_cleanup_status"
    ON "tier_metadata_cleanup_jobs" ("status", "updated_at");

-- Operator retention extensions are durable and checked by every delete path.
CREATE TABLE IF NOT EXISTS "segment_retention_overrides" (
    "range_id"     TEXT    NOT NULL PRIMARY KEY,
    "extend_until" INTEGER NOT NULL,
    "reason"       TEXT,
    "updated_at"   INTEGER NOT NULL DEFAULT 0
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

CREATE INDEX IF NOT EXISTS "idx_storage_alerts_filter_created"
    ON "storage_alerts" ("level", "acknowledged", "created_at" DESC);

CREATE INDEX IF NOT EXISTS "idx_storage_alerts_ack_created"
    ON "storage_alerts" ("acknowledged", "created_at" DESC);

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

CREATE INDEX IF NOT EXISTS "idx_protected_videos_camera_window"
    ON "protected_videos" ("camera_id", "start_time", "end_time");

-- Durable, node-local progress for historical TimeFile to HOT tier metadata backfill.
CREATE TABLE IF NOT EXISTS "tier_backfill_tasks" (
    "task_id"           TEXT    NOT NULL PRIMARY KEY,
    "camera_id"         TEXT    NOT NULL UNIQUE,
    "timeline_path"     TEXT    NOT NULL,
    "window_start"      INTEGER NOT NULL DEFAULT 0,
    "cutoff_time"       INTEGER NOT NULL DEFAULT 0,
    "cursor_time"       INTEGER NOT NULL DEFAULT 0,
    "status"            TEXT    NOT NULL DEFAULT 'PENDING',
    "attempt_count"     INTEGER NOT NULL DEFAULT 0,
    "next_retry_at"     INTEGER NOT NULL DEFAULT 0,
    "last_error"        TEXT    NOT NULL DEFAULT '',
    "scanned_blocks"    INTEGER NOT NULL DEFAULT 0,
    "registered_blocks" INTEGER NOT NULL DEFAULT 0,
    "orphan_blocks"     INTEGER NOT NULL DEFAULT 0,
    "indexed_bytes"     INTEGER NOT NULL DEFAULT 0,
    "started_at"        INTEGER NOT NULL DEFAULT 0,
    "completed_at"      INTEGER NOT NULL DEFAULT 0,
    "created_at"        INTEGER NOT NULL DEFAULT 0,
    "updated_at"        INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_tier_backfill_tasks_status_retry"
    ON "tier_backfill_tasks" ("status", "next_retry_at", "updated_at");
