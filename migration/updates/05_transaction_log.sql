CREATE TABLE IF NOT EXISTS transaction_log (
    peer_guid   BLOB(16) NOT NULL,
    db_guid     BLOB(16) NOT NULL,
    sequence    INTEGER NOT NULL,
    timestamp   INTEGER NOT NULL,
    tran_guid   BLOB(16) NOT NULL,
    tran_data   BLOB  NOT NULL, 
    tran_type int DEFAULT 0, 
    timestamp_hi BIGINT DEFAULT 0
);

CREATE UNIQUE INDEX idx_transaction_key   ON transaction_log(peer_guid, db_guid, sequence);
CREATE UNIQUE INDEX idx_transaction_hash  ON transaction_log(tran_guid);

CREATE TABLE IF NOT EXISTS "transaction_sequence" (
    peer_guid   BLOB(16) NOT NULL,
    db_guid     BLOB(16) NOT NULL,
    sequence    INTEGER NOT NULL
);

CREATE UNIQUE INDEX idx_tran_sequence_key  ON transaction_sequence(peer_guid, db_guid);