-- Outcome of onvifApplyVideoConfigRule per (logId, deviceId), for lookup by the calling service.
CREATE TABLE IF NOT EXISTS "video_config_rule_result" (
    id TEXT NOT NULL UNIQUE PRIMARY KEY,        -- logId|deviceId
    logId TEXT NOT NULL DEFAULT '',
    deviceId TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'PENDING',     -- PENDING, SUCCESS, FAILED
    errCode TEXT NOT NULL DEFAULT '',
    errMsg TEXT NOT NULL DEFAULT '',
    createdAt INTEGER NOT NULL DEFAULT 0,       -- ms, when the request was accepted
    updatedAt INTEGER NOT NULL DEFAULT 0        -- ms, last status change
);

CREATE INDEX IF NOT EXISTS idx_video_config_rule_result_log_id
    ON video_config_rule_result(logId);

CREATE INDEX IF NOT EXISTS idx_video_config_rule_result_updated_at
    ON video_config_rule_result(updatedAt);
