-- Storage Tiering: pool definitions
CREATE TABLE IF NOT EXISTS "storage_pools" (
    "id"                       TEXT    NOT NULL PRIMARY KEY,
    "name"                     TEXT    NOT NULL,
    "type"                     TEXT    NOT NULL,  -- LOCAL_DISK | NAS | MINIO | S3 | ARCHIVE
    "tier"                     TEXT    NOT NULL,  -- HOT | WARM | COLD
    "endpoint"                 TEXT,
    "bucket"                   TEXT,
    "base_path"                TEXT,
    "access_key"               TEXT,
    "secret_key_enc"           TEXT,
    "mount_path"               TEXT,    -- logical filesystem root; excludes Record::kAppName
    "network_path"             TEXT,    -- logical mounted NAS root; excludes Record::kAppName
    "enabled"                  INTEGER NOT NULL DEFAULT 1,
    "health_check_enabled"     INTEGER NOT NULL DEFAULT 1,
    "high_watermark_percent"   INTEGER NOT NULL DEFAULT 85,
    "critical_watermark_percent" INTEGER NOT NULL DEFAULT 90,
    "created_at"               INTEGER NOT NULL DEFAULT 0,
    "updated_at"               INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_storage_pools_tier_type_enabled"
    ON "storage_pools" ("tier", "type", "enabled");

CREATE INDEX IF NOT EXISTS "idx_storage_pools_type_enabled"
    ON "storage_pools" ("type", "enabled");

-- Storage Tiering: policy definitions (tier configs stored as JSON)
CREATE TABLE IF NOT EXISTS "storage_policies" (
    "id"                    TEXT    NOT NULL PRIMARY KEY,
    "name"                  TEXT    NOT NULL,
    "description"           TEXT,
    "enabled"               INTEGER NOT NULL DEFAULT 1,
    "total_retention_days"  INTEGER NOT NULL DEFAULT 30,
    "allow_camera_override" INTEGER NOT NULL DEFAULT 1,
    "protect_event_video"   INTEGER NOT NULL DEFAULT 0,
    "tiers_json"            TEXT    NOT NULL DEFAULT '[]',
    "delete_policy_json"    TEXT    NOT NULL DEFAULT '{}',
    "advanced_rules_json"   TEXT    NOT NULL DEFAULT '{}',
    "created_at"            INTEGER NOT NULL DEFAULT 0,
    "updated_at"            INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_storage_policies_enabled_created"
    ON "storage_policies" ("enabled", "created_at" DESC);

CREATE INDEX IF NOT EXISTS "idx_storage_policies_created"
    ON "storage_policies" ("created_at" DESC);

-- Storage Tiering: camera-level policy assignments (override)
CREATE TABLE IF NOT EXISTS "camera_policy_assignments" (
    "camera_id"       TEXT    NOT NULL PRIMARY KEY,
    "policy_id"       TEXT    NOT NULL,
    "override_reason" TEXT,
    "assigned_at"     INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_camera_policy_assignments_policy"
    ON "camera_policy_assignments" ("policy_id");

-- Storage Tiering: tiering job history
CREATE TABLE IF NOT EXISTS "tiering_jobs" (
    "job_id"             TEXT    NOT NULL PRIMARY KEY,
    "camera_id"          TEXT    NOT NULL,
    "stream_id"          TEXT    NOT NULL DEFAULT '',
    "range_id"           TEXT    NOT NULL DEFAULT '',
    "source_tier"        TEXT    NOT NULL,
    "target_tier"        TEXT    NOT NULL,
    "source_pool_id"     TEXT    NOT NULL CHECK ("source_pool_id" <> ''),
    "target_pool_id"     TEXT    NOT NULL CHECK ("target_pool_id" <> ''),
    "status"             TEXT    NOT NULL DEFAULT 'PENDING',
    "segment_start_time" INTEGER NOT NULL DEFAULT 0,
    "segment_end_time"   INTEGER NOT NULL DEFAULT 0,
    "bytes_total"        INTEGER NOT NULL DEFAULT 0,
    "bytes_moved"        INTEGER NOT NULL DEFAULT 0,
    "error_message"      TEXT,
    "created_at"         INTEGER NOT NULL DEFAULT 0,
    "updated_at"         INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_tiering_jobs_status_created"
    ON "tiering_jobs" ("status", "created_at");

CREATE INDEX IF NOT EXISTS "idx_tiering_jobs_camera_status_created"
    ON "tiering_jobs" ("camera_id", "status", "created_at" DESC);

CREATE INDEX IF NOT EXISTS "idx_tiering_jobs_status_updated"
    ON "tiering_jobs" ("status", "updated_at");

-- Storage Tiering: compact range tracking for large camera fleets.
CREATE TABLE IF NOT EXISTS "segment_tier_ranges" (
    "range_id"      TEXT    NOT NULL PRIMARY KEY,
    "camera_id"     TEXT    NOT NULL,
    "stream_id"     TEXT    NOT NULL,
    "tier"          TEXT    NOT NULL DEFAULT 'HOT',
    "pool_id"       TEXT    NOT NULL CHECK ("pool_id" <> ''),
    "status"        TEXT    NOT NULL DEFAULT 'AVAILABLE',
    "start_time"    INTEGER NOT NULL DEFAULT 0,
    "end_time"      INTEGER NOT NULL DEFAULT 0,
    "segment_count" INTEGER NOT NULL DEFAULT 0,
    "size_bytes"    INTEGER NOT NULL DEFAULT 0,
    "created_at"    INTEGER NOT NULL DEFAULT 0,
    "updated_at"    INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_camera_time"
    ON "segment_tier_ranges" ("camera_id", "stream_id", "start_time", "end_time");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_tier_age"
    ON "segment_tier_ranges" ("tier", "status", "end_time");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_camera_tier_started"
    ON "segment_tier_ranges" ("camera_id", "tier", "status", "start_time");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_pool_tier_age"
    ON "segment_tier_ranges" ("pool_id", "tier", "status", "end_time");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_camera_pool_tier_age"
    ON "segment_tier_ranges" ("camera_id", "pool_id", "tier", "status", "end_time");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_status_camera"
    ON "segment_tier_ranges" ("status", "camera_id");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_status_updated"
    ON "segment_tier_ranges" ("status", "updated_at");

-- Storage Tiering: periodic pool health metrics for dashboard
CREATE TABLE IF NOT EXISTS "pool_metrics" (
    "pool_id"       TEXT    NOT NULL,
    "timestamp"     INTEGER NOT NULL,
    "used_bytes"    INTEGER NOT NULL DEFAULT 0,
    "total_bytes"   INTEGER NOT NULL DEFAULT 0,
    "usage_pct"     REAL    NOT NULL DEFAULT 0.0,
    "health_status" TEXT    NOT NULL DEFAULT 'OK',
    PRIMARY KEY ("pool_id", "timestamp")
);
