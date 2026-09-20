-- Storage Tiering: indexes for per-range job lookups, pool pressure accounting,
-- the reconciliation sweep and per-camera age retention paging.
CREATE INDEX IF NOT EXISTS "idx_tiering_jobs_range_status"
    ON "tiering_jobs" ("range_id", "status");

CREATE INDEX IF NOT EXISTS "idx_tiering_jobs_source_pool_status"
    ON "tiering_jobs" ("source_pool_id", "status");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_status_start"
    ON "segment_tier_ranges" ("status", "start_time", "range_id");

CREATE INDEX IF NOT EXISTS "idx_segment_tier_ranges_camera_status_start"
    ON "segment_tier_ranges" ("camera_id", "status", "start_time", "range_id");
