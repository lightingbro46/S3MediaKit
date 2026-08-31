CREATE TABLE IF NOT EXISTS local_resource_status_gap (
    resource_id        TEXT    NOT NULL,
    gap_id             TEXT    NOT NULL,
    disconnect_time    INTEGER NOT NULL,
    reconnect_time     INTEGER NOT NULL DEFAULT 0,
    disconnect_reason  TEXT    NOT NULL DEFAULT '',
    state              INTEGER NOT NULL DEFAULT 1 CHECK (state = 1),
    updated_time       INTEGER NOT NULL,

    PRIMARY KEY (resource_id, gap_id)
);

CREATE INDEX IF NOT EXISTS idx_local_resource_status_gap_resource_disconnect
    ON local_resource_status_gap(resource_id, disconnect_time);

CREATE INDEX IF NOT EXISTS idx_local_resource_status_gap_reconnect
    ON local_resource_status_gap(reconnect_time);

CREATE UNIQUE INDEX IF NOT EXISTS idx_local_resource_status_gap_one_open
    ON local_resource_status_gap(resource_id)
    WHERE state = 1 AND reconnect_time = 0;
