CREATE TABLE IF NOT EXISTS transaction_peer_ack_log (
    peer_guid  VARCHAR(64) NOT NULL,
    db_guid    VARCHAR(64) NOT NULL,
    src_peer_guid VARCHAR(64) NOT NULL,
    src_db_guid VARCHAR(64) NOT NULL,
    acked_seq  INTEGER     NOT NULL DEFAULT 0,
    updated_at INTEGER     NOT NULL DEFAULT 0,
    PRIMARY KEY (peer_guid, src_peer_guid)
);
