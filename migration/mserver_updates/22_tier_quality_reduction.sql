-- Durable quality-reduction state for tier transfers. Existing jobs remain copy moves.
ALTER TABLE "tiering_jobs"
    ADD COLUMN "operation" TEXT NOT NULL DEFAULT 'COPY_MOVE';

ALTER TABLE "tiering_jobs"
    ADD COLUMN "quality_profile_json" TEXT NOT NULL DEFAULT '{}';

ALTER TABLE "tiering_jobs"
    ADD COLUMN "output_bytes" INTEGER NOT NULL DEFAULT 0;

ALTER TABLE "tiering_jobs"
    ADD COLUMN "transcoded_segment_count" INTEGER NOT NULL DEFAULT 0;

ALTER TABLE "tiering_jobs"
    ADD COLUMN "preserved_segment_count" INTEGER NOT NULL DEFAULT 0;

ALTER TABLE "tiering_jobs"
    ADD COLUMN "encoder_summary_json" TEXT NOT NULL DEFAULT '{}';

CREATE TABLE IF NOT EXISTS "tiering_job_segments" (
    "job_id"              TEXT    NOT NULL,
    "range_id"            TEXT    NOT NULL,
    "camera_id"           TEXT    NOT NULL,
    "stream_id"           TEXT    NOT NULL,
    "segment_path"        TEXT    NOT NULL,
    "storage_key"         TEXT    NOT NULL,
    "segment_start_time"  INTEGER NOT NULL DEFAULT 0,
    "segment_end_time"    INTEGER NOT NULL DEFAULT 0,
    "operation"           TEXT    NOT NULL DEFAULT 'COPY_MOVE',
    "profile_fingerprint" TEXT    NOT NULL DEFAULT '',
    "encoder_backend"     TEXT    NOT NULL DEFAULT '',
    "status"              TEXT    NOT NULL DEFAULT 'PENDING',
    "attempt_count"       INTEGER NOT NULL DEFAULT 0,
    "source_bytes"        INTEGER NOT NULL DEFAULT 0,
    "output_bytes"        INTEGER NOT NULL DEFAULT 0,
    "error_code"          TEXT    NOT NULL DEFAULT '',
    "error_message"       TEXT    NOT NULL DEFAULT '',
    "created_at"          INTEGER NOT NULL DEFAULT 0,
    "updated_at"          INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY ("job_id", "stream_id", "segment_path")
);

CREATE INDEX IF NOT EXISTS "idx_tiering_job_segments_job_status"
    ON "tiering_job_segments" ("job_id", "status");
CREATE INDEX IF NOT EXISTS "idx_tiering_job_segments_status_updated"
    ON "tiering_job_segments" ("status", "updated_at");
CREATE INDEX IF NOT EXISTS "idx_tiering_job_segments_range_status"
    ON "tiering_job_segments" ("range_id", "status");

CREATE TABLE IF NOT EXISTS "tier_timefile_size_updates" (
    "job_id"         TEXT    NOT NULL,
    "range_id"       TEXT    NOT NULL,
    "camera_id"      TEXT    NOT NULL,
    "stream_id"      TEXT    NOT NULL,
    "segment_path"   TEXT    NOT NULL,
    "start_time"     INTEGER NOT NULL DEFAULT 0,
    "old_size"       INTEGER NOT NULL DEFAULT 0,
    "new_size"       INTEGER NOT NULL DEFAULT 0,
    "status"         TEXT    NOT NULL DEFAULT 'PENDING',
    "attempt_count"  INTEGER NOT NULL DEFAULT 0,
    "error_message"  TEXT    NOT NULL DEFAULT '',
    "created_at"     INTEGER NOT NULL DEFAULT 0,
    "updated_at"     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY ("camera_id", "stream_id", "segment_path")
);

CREATE INDEX IF NOT EXISTS "idx_tier_timefile_size_updates_status_updated"
    ON "tier_timefile_size_updates" ("status", "updated_at");
CREATE INDEX IF NOT EXISTS "idx_tier_timefile_size_updates_range_status"
    ON "tier_timefile_size_updates" ("range_id", "status");
