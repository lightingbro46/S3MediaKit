CREATE TABLE misc_data (key VARCHAR(64) NOT NULL, data TEXT NOT NULL);

CREATE UNIQUE INDEX idx_misc_data_key ON misc_data(key);

INSERT INTO misc_data(key, data)
VALUES ('DB_BOOTSTRAP_DONE', '{"schema_version":1,"status":"pending"}');
