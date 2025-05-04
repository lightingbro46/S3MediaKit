CREATE TABLE IF NOT EXITS "vms_migrationhistory" (
    "id" SERIAL PRIMARY KEY,
    "app_name" INTEGER NOT NULL,
    "migration" VARCHAR(255) NOT NULL,
    "applied" datetime NOT NULL
);