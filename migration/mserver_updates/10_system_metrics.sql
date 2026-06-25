-- System metrics: time-series samples collected by GlobalMonitor
-- Each row holds one sample point aggregating all sub-metrics as JSON blobs
-- for flexibility, plus top-level scalar columns for fast range queries.
CREATE TABLE IF NOT EXISTS "system_metrics" (
    "id"                    INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    "timestamp"             INTEGER NOT NULL,           -- unix epoch seconds
    "cpu_usage_pct"         REAL    NOT NULL DEFAULT 0, -- OS-level CPU %
    "cpu_proc_usage_pct"    REAL    NOT NULL DEFAULT 0, -- process CPU %
    "cpu_cores"             INTEGER NOT NULL DEFAULT 0,
    "ram_used"              INTEGER NOT NULL DEFAULT 0, -- bytes
    "ram_total"             INTEGER NOT NULL DEFAULT 0, -- bytes
    "ram_usage_pct"         REAL    NOT NULL DEFAULT 0,
    "reader_total"          INTEGER NOT NULL DEFAULT 0,
    "reader_live"           INTEGER NOT NULL DEFAULT 0,
    "reader_playback"       INTEGER NOT NULL DEFAULT 0,
    "nets_json"             TEXT    NOT NULL DEFAULT '[]', -- JSON array of NetInterfaceInfo
    "disks_json"            TEXT    NOT NULL DEFAULT '[]'  -- JSON array of DiskPartition
);

CREATE INDEX IF NOT EXISTS "idx_system_metrics_timestamp" ON "system_metrics" ("timestamp");
