-- transaction_peer_ack_log: one ack per (acker peer, source peer, source db).
-- The old key (peer_guid, src_peer_guid) could hold only one ack per source peer, so after a
-- source node recreated its database (new src_db_guid) the ack for the new database could never
-- be stored and its transaction_log was never pruned.
-- Rebuild keeps every existing row.
ALTER TABLE transaction_peer_ack_log RENAME TO transaction_peer_ack_log_v1;

CREATE TABLE IF NOT EXISTS transaction_peer_ack_log (
    peer_guid     VARCHAR(64) NOT NULL,
    db_guid       VARCHAR(64) NOT NULL,
    src_peer_guid VARCHAR(64) NOT NULL,
    src_db_guid   VARCHAR(64) NOT NULL,
    acked_seq     INTEGER     NOT NULL DEFAULT 0,
    updated_at    INTEGER     NOT NULL DEFAULT 0,
    PRIMARY KEY (peer_guid, src_peer_guid, src_db_guid)
);

INSERT OR IGNORE INTO transaction_peer_ack_log (peer_guid, db_guid, src_peer_guid, src_db_guid, acked_seq, updated_at)
    SELECT peer_guid, db_guid, src_peer_guid, src_db_guid, acked_seq, updated_at FROM transaction_peer_ack_log_v1;

DROP TABLE transaction_peer_ack_log_v1;
