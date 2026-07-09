CREATE TABLE IF NOT EXISTS sd_sync_stream (
    device_id             TEXT    NOT NULL,
    stream_id             TEXT    NOT NULL,
    replay_uri            TEXT    NOT NULL,
    earliest_record_time  INTEGER,
    latest_record_time    INTEGER,

    PRIMARY KEY (device_id, stream_id)
);

CREATE TABLE IF NOT EXISTS sd_sync_session (
    device_id              TEXT    NOT NULL,
    session_id             TEXT    NOT NULL,
    disconnect_time        INTEGER NOT NULL,
    reconnect_time         INTEGER,
    processing_start_time  INTEGER,
    processing_end_time    INTEGER,
    progress_percent       REAL,
    session_state          TEXT,

    PRIMARY KEY (device_id, session_id)
);

CREATE TABLE IF NOT EXISTS sd_sync_stream_progress (
    device_id              TEXT    NOT NULL,
    session_id             TEXT    NOT NULL,
    stream_id              TEXT    NOT NULL,
    handle_state           TEXT,
    current_segment_index  INTEGER,

    PRIMARY KEY (device_id, session_id, stream_id)
);

CREATE TABLE IF NOT EXISTS sd_sync_segment (
    device_id       TEXT    NOT NULL,
    session_id      TEXT    NOT NULL,
    stream_id       TEXT    NOT NULL,
    segment_index   INTEGER NOT NULL,
    start_time      INTEGER NOT NULL,
    end_time        INTEGER NOT NULL,
    processed_up_to INTEGER,
    state           TEXT,

    PRIMARY KEY (device_id, session_id, stream_id, segment_index)
);
