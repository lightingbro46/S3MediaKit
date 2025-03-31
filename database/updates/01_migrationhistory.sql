CREATE TABLE IF NOT EXITS "vms_migrationhistory" (
    "id" SERIAL PRIMARY KEY,
    "app_name" INTEGER NOT NULL,
    "migration" VARCHAR(255) NOT NULL,
    "applied" datetime NOT NULL
);



CREATE vms_kvpair (
    id SERIAL PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    private_key TEXT NOT NULL,
    public_key TEXT NOT NULL
);