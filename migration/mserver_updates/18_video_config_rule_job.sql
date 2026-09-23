CREATE TABLE IF NOT EXISTS "video_config_rule_job" (
    id TEXT NOT NULL UNIQUE PRIMARY KEY,        -- deviceId|profileToken|active  or  deviceId|profileToken|queued
    logId TEXT NOT NULL DEFAULT '',
    deviceId TEXT NOT NULL DEFAULT '',
    profileToken TEXT NOT NULL DEFAULT '',
    status INTEGER NOT NULL DEFAULT 0,          -- 0: ACTIVE, 1: QUEUED
    vcodec TEXT NOT NULL DEFAULT '',
    width INTEGER NOT NULL DEFAULT 0,
    height INTEGER NOT NULL DEFAULT 0,
    fps REAL NOT NULL DEFAULT 0,
    bitrate INTEGER NOT NULL DEFAULT 0,
    receivedAt INTEGER NOT NULL DEFAULT 0,
    appliedAt INTEGER NOT NULL DEFAULT 0,
    deadlineAt INTEGER NOT NULL DEFAULT 0,
    updatedAt INTEGER NOT NULL DEFAULT 0
);