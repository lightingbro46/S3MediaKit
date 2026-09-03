CREATE TABLE IF NOT EXISTS media_gap_fill_task (
    connection_id         TEXT    NOT NULL PRIMARY KEY,
    gap_id                TEXT    NOT NULL,
    session_id            TEXT    NOT NULL,
    device_id             TEXT    NOT NULL,
    stream_id             TEXT    NOT NULL,
    source_type           TEXT    NOT NULL,
    origin_url            TEXT    NOT NULL,
    source_begin_time     INTEGER NOT NULL,
    segment_index         INTEGER NOT NULL,
    disconnect_time       INTEGER NOT NULL,
    reconnect_time        INTEGER NOT NULL,
    begin_time            INTEGER NOT NULL,
    end_time              INTEGER NOT NULL,
    processed_up_to       INTEGER NOT NULL,
    processing_start_time INTEGER NOT NULL DEFAULT 0,
    processing_end_time   INTEGER NOT NULL DEFAULT 0,
    retry_count           INTEGER NOT NULL DEFAULT 0,
    retry_limit           INTEGER NOT NULL DEFAULT 0,
    next_retry_time       INTEGER NOT NULL DEFAULT 0,
    last_attempt_time     INTEGER NOT NULL DEFAULT 0,
    state                 TEXT    NOT NULL,
    last_error            TEXT    NOT NULL DEFAULT '',
    updated_time          INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_media_gap_fill_task_device_gap_state
    ON media_gap_fill_task(device_id, gap_id, state);

CREATE INDEX IF NOT EXISTS idx_media_gap_fill_task_retry_schedule
    ON media_gap_fill_task(state, next_retry_time);

CREATE INDEX IF NOT EXISTS idx_media_gap_fill_task_device_session_segment
    ON media_gap_fill_task(device_id, session_id, stream_id, segment_index);

CREATE TABLE IF NOT EXISTS media_gap_fill_session (
    device_id          TEXT    NOT NULL,
    gap_id             TEXT    NOT NULL,
    session_id         TEXT    NOT NULL,
    source_type        TEXT    NOT NULL,
    disconnect_time    INTEGER NOT NULL,
    reconnect_time     INTEGER NOT NULL,
    discovery_state    TEXT    NOT NULL,
    control_state      TEXT    NOT NULL,
    discovery_attempts INTEGER NOT NULL DEFAULT 0,
    last_error         TEXT    NOT NULL DEFAULT '',
    updated_time       INTEGER NOT NULL,
    PRIMARY KEY (device_id, gap_id)
);

CREATE INDEX IF NOT EXISTS idx_media_gap_fill_session_device_state
    ON media_gap_fill_session(device_id, discovery_state, control_state, disconnect_time);
