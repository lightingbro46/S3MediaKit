#!/usr/bin/env bash
set -euo pipefail

readonly CAMERA_COUNT=300
readonly STREAM_COUNT=600
readonly RETENTION_DAYS=180
readonly RANGE_ROWS=$((STREAM_COUNT * RETENTION_DAYS))

benchmark_db="${1:-}"
remove_database=0
if [[ -z "${benchmark_db}" ]]; then
  benchmark_db="$(mktemp /tmp/s3mediakit-tier-metadata-XXXXXX.db)"
  remove_database=1
fi

cleanup() {
  if (( remove_database )); then
    rm -f -- "${benchmark_db}" "${benchmark_db}-wal" "${benchmark_db}-shm"
  fi
}
trap cleanup EXIT

echo "Tier metadata benchmark: ${CAMERA_COUNT} cameras, ${STREAM_COUNT} streams, ${RETENTION_DAYS} days"
echo "Database: ${benchmark_db}"

sqlite3 "${benchmark_db}" <<SQL
.bail on
.timer on
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;

CREATE TABLE segment_tier_ranges (
  range_id TEXT NOT NULL PRIMARY KEY,
  camera_id TEXT NOT NULL,
  stream_id TEXT NOT NULL,
  tier TEXT NOT NULL,
  pool_id TEXT NOT NULL,
  status TEXT NOT NULL,
  start_time INTEGER NOT NULL,
  end_time INTEGER NOT NULL,
  segment_count INTEGER NOT NULL,
  size_bytes INTEGER NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX idx_segment_tier_ranges_camera_tier_started
  ON segment_tier_ranges(camera_id, tier, status, start_time);
CREATE INDEX idx_segment_tier_ranges_tier_age
  ON segment_tier_ranges(tier, status, end_time);
CREATE INDEX idx_segment_tier_ranges_pool_tier_age
  ON segment_tier_ranges(pool_id, tier, status, end_time);
CREATE INDEX idx_segment_tier_ranges_camera_pool_tier_age
  ON segment_tier_ranges(camera_id, pool_id, tier, status, end_time);
CREATE INDEX idx_segment_tier_ranges_status_camera
  ON segment_tier_ranges(status, camera_id);
CREATE INDEX idx_segment_tier_ranges_status_updated
  ON segment_tier_ranges(status, updated_at);

CREATE TABLE tiering_job_segments (
  job_id TEXT NOT NULL, range_id TEXT NOT NULL, camera_id TEXT NOT NULL,
  stream_id TEXT NOT NULL, segment_path TEXT NOT NULL, storage_key TEXT NOT NULL,
  status TEXT NOT NULL, updated_at INTEGER NOT NULL,
  PRIMARY KEY(job_id, stream_id, segment_path)
);
CREATE INDEX idx_tiering_job_segments_job_status
  ON tiering_job_segments(job_id, status);
CREATE INDEX idx_tiering_job_segments_status_updated
  ON tiering_job_segments(status, updated_at);
CREATE TABLE tier_timefile_size_updates (
  job_id TEXT NOT NULL, range_id TEXT NOT NULL, camera_id TEXT NOT NULL,
  stream_id TEXT NOT NULL, segment_path TEXT NOT NULL, status TEXT NOT NULL,
  updated_at INTEGER NOT NULL,
  PRIMARY KEY(camera_id, stream_id, segment_path)
);
CREATE INDEX idx_tier_timefile_size_updates_status_updated
  ON tier_timefile_size_updates(status, updated_at);

BEGIN;
WITH RECURSIVE ranges(value) AS (
  VALUES(0)
  UNION ALL SELECT value + 1 FROM ranges WHERE value + 1 < ${RANGE_ROWS}
)
INSERT INTO segment_tier_ranges
SELECT printf('range-%08d', value),
       printf('camera-%03d', (value % ${STREAM_COUNT}) / 2),
       CASE WHEN value % 2 = 0 THEN 'main' ELSE 'sub' END,
       'HOT', 'hot-pool', 'AVAILABLE',
       1700000000 + (value / ${STREAM_COUNT}) * 86400,
       1700086400 + (value / ${STREAM_COUNT}) * 86400,
       1440, 1509949440, 1700000000, 1700000000
  FROM ranges;
COMMIT;

BEGIN;
WITH RECURSIVE children(value) AS (
  VALUES(0) UNION ALL SELECT value + 1 FROM children WHERE value + 1 < 10000
)
INSERT INTO tiering_job_segments
SELECT printf('job-%04d', value / 100), printf('range-%08d', value),
       printf('camera-%03d', value % ${CAMERA_COUNT}), 'main',
       printf('segment-%08d', value), printf('key-%08d', value),
       CASE WHEN value % 10 = 0 THEN 'PENDING' ELSE 'STORED' END, 1700000000
  FROM children;
INSERT INTO tier_timefile_size_updates
SELECT job_id, range_id, camera_id, stream_id, segment_path,
       CASE WHEN rowid % 10 = 0 THEN 'PENDING' ELSE 'DONE' END, updated_at
  FROM tiering_job_segments;
COMMIT;
SQL

actual_ranges="$(sqlite3 "${benchmark_db}" 'SELECT COUNT(*) FROM segment_tier_ranges;')"
if (( actual_ranges != RANGE_ROWS )); then
  echo "Expected ${RANGE_ROWS} compact ranges, found ${actual_ranges}" >&2
  exit 1
fi

sqlite3 "${benchmark_db}" <<SQL
.bail on
.timer on
SELECT 'compact_ranges', count(*) FROM segment_tier_ranges;
SELECT 'available_cameras', count(DISTINCT camera_id)
  FROM segment_tier_ranges WHERE status = 'AVAILABLE';
SELECT 'eligible_page', count(*) FROM (
  SELECT range_id FROM segment_tier_ranges
  WHERE camera_id = 'camera-000' AND tier = 'HOT'
    AND status = 'AVAILABLE' AND start_time < 1800000000
  ORDER BY start_time ASC LIMIT 64
);
SELECT 'pending_job_segments', count(*) FROM (
  SELECT segment_path FROM tiering_job_segments
   WHERE job_id = 'job-0000' AND status = 'PENDING' LIMIT 100
);
SELECT 'pending_timefile_patches', count(*) FROM (
  SELECT segment_path FROM tier_timefile_size_updates
   WHERE status = 'PENDING' ORDER BY updated_at LIMIT 100
);
BEGIN;
UPDATE segment_tier_ranges SET tier = 'WARM', pool_id = 'warm-pool'
 WHERE range_id IN (
   SELECT range_id FROM segment_tier_ranges
    WHERE camera_id = 'camera-000' AND tier = 'HOT'
      AND status = 'AVAILABLE'
    ORDER BY start_time ASC LIMIT 64
 );
SELECT 'tier_ranges_updated', changes();
ROLLBACK;
BEGIN;
UPDATE segment_tier_ranges SET status = 'DELETED', updated_at = 0
 WHERE range_id IN (
   SELECT range_id FROM segment_tier_ranges ORDER BY start_time ASC LIMIT 600
 );
DELETE FROM segment_tier_ranges
 WHERE status IN ('DELETED', 'EXPIRED') AND updated_at < 1;
SELECT 'terminal_ranges_pruned', changes();
ROLLBACK;
PRAGMA page_count;
PRAGMA page_size;
SQL
