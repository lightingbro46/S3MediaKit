CREATE TABLE IF NOT EXISTS extract_jobs (
    file_id                TEXT    NOT NULL PRIMARY KEY,
    request_hash           TEXT    NOT NULL,
    camera_id              TEXT    NOT NULL,
    stream_id              TEXT    NOT NULL DEFAULT '',
    start_time             INTEGER NOT NULL,
    end_time               INTEGER NOT NULL,
    format                 TEXT    NOT NULL,
    upload_url             TEXT    NOT NULL,
    status                 TEXT    NOT NULL DEFAULT 'PENDING',
    progress_percent       REAL    NOT NULL DEFAULT 0,
    local_path             TEXT    NOT NULL DEFAULT '',
    extract_attempts       INTEGER NOT NULL DEFAULT 0,
    upload_attempts        INTEGER NOT NULL DEFAULT 0,
    next_attempt_at        INTEGER NOT NULL DEFAULT 0,
    size_bytes             INTEGER NOT NULL DEFAULT 0,
    duration_seconds       INTEGER NOT NULL DEFAULT 0,
    content_type           TEXT    NOT NULL DEFAULT '',
    error_code             TEXT    NOT NULL DEFAULT '',
    error_message          TEXT    NOT NULL DEFAULT '',
    callback_status        TEXT    NOT NULL DEFAULT 'NONE',
    callback_attempts      INTEGER NOT NULL DEFAULT 0,
    next_callback_at       INTEGER NOT NULL DEFAULT 0,
    callback_last_error    TEXT    NOT NULL DEFAULT '',
    notified_at            INTEGER NOT NULL DEFAULT 0,
    completed_at           INTEGER NOT NULL DEFAULT 0,
    created_at             INTEGER NOT NULL,
    updated_at             INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_extract_jobs_processing
    ON extract_jobs(status, next_attempt_at, created_at);

CREATE INDEX IF NOT EXISTS idx_extract_jobs_callback
    ON extract_jobs(callback_status, next_callback_at);

CREATE INDEX IF NOT EXISTS idx_extract_jobs_start_time
    ON extract_jobs(start_time DESC);

CREATE INDEX IF NOT EXISTS idx_extract_jobs_camera_start_time
    ON extract_jobs(camera_id, start_time DESC);
