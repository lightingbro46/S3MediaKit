-- Health monitor history (/media/monitor/history): one sample of the evaluated health every
-- monitor.historyIntervalSec, kept globalmonitor.metrics_retention_days like system_metrics.
CREATE TABLE IF NOT EXISTS "health_metrics" (
    "id"                    INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    "timestamp"             INTEGER NOT NULL,                  -- unix epoch seconds
    "overall"               TEXT    NOT NULL DEFAULT 'unknown', -- ok | warning | critical | unknown
    "score"                 INTEGER NOT NULL DEFAULT 0,
    "cameras_total"         INTEGER NOT NULL DEFAULT 0,
    "cameras_online"        INTEGER NOT NULL DEFAULT 0,
    "rec_expected"          INTEGER NOT NULL DEFAULT 0,
    "rec_active"            INTEGER NOT NULL DEFAULT 0,
    "rec_stalled"           INTEGER NOT NULL DEFAULT 0,
    "viewers_total"         INTEGER NOT NULL DEFAULT 0,
    "poller_delay_max_ms"   REAL    NOT NULL DEFAULT 0,
    "alerts_active"         INTEGER NOT NULL DEFAULT 0,
    "proc_rss"              INTEGER NOT NULL DEFAULT 0,        -- bytes, for the RSS growth check
    "components_json"       TEXT    NOT NULL DEFAULT '{}'      -- component -> level
);

CREATE INDEX IF NOT EXISTS "idx_health_metrics_timestamp" ON "health_metrics" ("timestamp");

-- One row per MediaServer start (process.restarts_24h). stopped_at is written on a clean exit;
-- prev_clean tells whether the run before this one ended cleanly (1), not (0, crash or kill), or
-- there was none (-1).
CREATE TABLE IF NOT EXISTS "process_starts" (
    "id"                    INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    "started_at"            INTEGER NOT NULL,
    "stopped_at"            INTEGER NOT NULL DEFAULT 0,
    "prev_clean"            INTEGER NOT NULL DEFAULT -1,
    "version"               TEXT    NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS "idx_process_starts_started_at" ON "process_starts" ("started_at");

-- fps/bitrate baseline learnt per camera stream, so a restart does not learn it again.
CREATE TABLE IF NOT EXISTS "stream_baselines" (
    "camera_id"             TEXT    NOT NULL,
    "stream_type"           INTEGER NOT NULL,                  -- 0 = main, 1 = sub
    "format"                TEXT    NOT NULL DEFAULT '',       -- codec + resolution it was learnt for
    "fps"                   REAL    NOT NULL DEFAULT 0,
    "bitrate_kbps"          REAL    NOT NULL DEFAULT 0,
    "learnt_at"             INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY ("camera_id", "stream_type")
);
