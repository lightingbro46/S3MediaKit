CREATE TABLE IF NOT EXITS "vms_businessrule" (
    "id" SERIAL PRIMARY KEY,
    "aggregation_period" INTEGER NOT NULL,
    "action_params" VARCHAR(16384) NOT NULL,
    "event_condition" VARCHAR(16384) NOT NULL,
    "schedule" VARCHAR(255),
    "system" BOOLEAN NOT NULL DEFAULT FALSE,
    "comments" VARCHAR(16384),
    "disabled" BOOLEAN NOT NULL DEFAULT FALSE,
    "action_type" SMALLINT NOT NULL,
    'event_state' SMALLINT NOT NULL DEFAULT 0,
    "event_type" SMALLINT NOT NULL,
    "guid" BLOB(16)
);