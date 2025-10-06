CREATE TABLE local_resource_properties (
	id INTEGER NOT NULL PRIMARY KEY,
    resource_id BLOB NOT NULL,
	property_name TEXT NOT NULL,
	property_value TEXT
);