CREATE TABLE IF NOT EXISTS "audit_log" (
    id INTEGER NOT NULL PRIMARY KEY autoincrement,
    createdTimeSec INTEGER NOT NULL,
    rangeStartSec INTEGER NOT NULL,
    rangeEndSec INTEGER NOT NULL,
    eventType SMALLINT NOT NULL,
    resources BLOB,
    params TEXT,
    authSession TEXT
);