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
    "mount_path"               TEXT,
    "network_path"             TEXT,
    "enabled"                  INTEGER NOT NULL DEFAULT 1,
    "health_check_enabled"     INTEGER NOT NULL DEFAULT 1,
    "high_watermark_percent"   INTEGER NOT NULL DEFAULT 85,
    "critical_watermark_percent" INTEGER NOT NULL DEFAULT 90,
    "created_at"               INTEGER NOT NULL DEFAULT 0,
    "updated_at"               INTEGER NOT NULL DEFAULT 0
);

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

-- Storage Tiering: camera-level policy assignments (override)
CREATE TABLE IF NOT EXISTS "camera_policy_assignments" (
    "camera_id"       TEXT    NOT NULL PRIMARY KEY,
    "policy_id"       TEXT    NOT NULL,
    "override_reason" TEXT,
    "assigned_at"     INTEGER NOT NULL DEFAULT 0
);

-- Storage Tiering: tiering job history
CREATE TABLE IF NOT EXISTS "tiering_jobs" (
    "job_id"             TEXT    NOT NULL PRIMARY KEY,
    "camera_id"          TEXT    NOT NULL,
    "source_tier"        TEXT    NOT NULL,
    "target_tier"        TEXT    NOT NULL,
    "source_pool_id"     TEXT    NOT NULL,
    "target_pool_id"     TEXT    NOT NULL,
    "status"             TEXT    NOT NULL DEFAULT 'PENDING',
    "segment_start_time" INTEGER NOT NULL DEFAULT 0,
    "segment_end_time"   INTEGER NOT NULL DEFAULT 0,
    "bytes_total"        INTEGER NOT NULL DEFAULT 0,
    "bytes_moved"        INTEGER NOT NULL DEFAULT 0,
    "error_message"      TEXT,
    "created_at"         INTEGER NOT NULL DEFAULT 0,
    "updated_at"         INTEGER NOT NULL DEFAULT 0
);

-- Storage Tiering: per-segment tier tracking
CREATE TABLE IF NOT EXISTS "segment_tier_records" (
    "camera_id"    TEXT    NOT NULL,
    "stream_id"    TEXT    NOT NULL,
    "segment_path" TEXT    NOT NULL,
    "tier"         TEXT    NOT NULL DEFAULT 'HOT',
    "pool_id"      TEXT,
    "status"       TEXT    NOT NULL DEFAULT 'AVAILABLE',
    "start_time"   INTEGER NOT NULL DEFAULT 0,
    "end_time"     INTEGER NOT NULL DEFAULT 0,
    "file_size"    INTEGER NOT NULL DEFAULT 0,
    "created_at"   INTEGER NOT NULL DEFAULT 0,
    "updated_at"   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY ("camera_id", "stream_id", "segment_path")
);

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
