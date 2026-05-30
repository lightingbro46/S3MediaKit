CREATE TABLE IF NOT EXISTS vms_resource_assignment (
    assignment_guid    TEXT    NOT NULL PRIMARY KEY,
    resource_guid      TEXT    NOT NULL,  -- camera guid
    owner_peer_id      TEXT    NOT NULL,  -- node nào record đoạn này
    owner_db_guid      TEXT    NOT NULL,
    assigned_at        INTEGER NOT NULL,  -- Epoch ms: khi nào được giao
    released_at        INTEGER NOT NULL,  -- Epoch ms: khi nào kết thúc; 0 = đang active
    assign_type        INTEGER NOT NULL, -- 0=PRIMARY, 1=FAILOVER, 2=MANUAL
    prev_peer_id       TEXT    NOT NULL
);

CREATE INDEX idx_assignment_resource_time  ON vms_resource_assignment (resource_guid, assigned_at, released_at);
CREATE INDEX idx_assignment_resource_owner ON vms_resource_assignment (owner_peer_id, assigned_at);