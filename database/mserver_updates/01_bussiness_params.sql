CREATE vms_migratehistory (
    id SERIAL PRIMARY KEY,
    app_name INTEGER NOT NULL,
    migration VARCHAR(255) NOT NULL,
    applied datetime NOT NULL
);


CREATE runtime_actions (
    id SERIAL PRIMARY KEY,
    app_name INTEGER NOT NULL,
    migration VARCHAR(255) NOT NULL,
    applied datetime NOT NULL
);
