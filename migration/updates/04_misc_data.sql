CREATE TABLE misc_data (key VARCHAR(64) NOT NULL, data BLOB(128));

CREATE UNIQUE INDEX idx_misc_data_key ON misc_data(key);