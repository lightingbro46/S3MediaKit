CREATE TABLE IF NOT EXISTS "vms_resourcetype" (
    "id" integer NOT NULL PRIMARY KEY,
    "name" varchar(200) NOT NULL,
    "description" varchar(200) NULL,
    "manufacture_id" integer NULL, guid BLOB(16));
INSERT INTO vms_resourcetype VALUES(1,'User',NULL,NULL,X'774e6ecdffc6ae8801658f4a6d0eafa7');
INSERT INTO vms_resourcetype VALUES(2,'Server',NULL,NULL,X'be5d1ee0b92c3b3486d9bca2dab7826f');
INSERT INTO vms_resourcetype VALUES(3,'Storage',NULL,NULL,X'f8544a40880e9442b78a9da6db6862b4');
INSERT INTO vms_resourcetype VALUES(4,'Local',NULL,NULL,X'62aae8c0d28ed28030319bcc8153a749');
INSERT INTO vms_resourcetype VALUES(5,'Camera',NULL,NULL,X'1b7181ce0227d3f79443c86aab922d96');

CREATE UNIQUE INDEX idx_resourcetype_guid ON vms_resourcetype(guid);

CREATE TABLE IF NOT EXISTS "vms_resource" (
    "id" INTEGER PRIMARY KEY AUTOINCREMENT,
    "guid" BLOB(16) NULL UNIQUE,
    "parent_guid" BLOB(16),
    "name" VARCHAR(200) NOT NULL,
    "url" VARCHAR(200),
    "xtype_guid" BLOB(16)
);

CREATE UNIQUE INDEX idx_resource_guid     ON vms_resource(guid);
CREATE INDEX idx_resource_parent          ON vms_resource(parent_guid);

CREATE TABLE IF NOT EXISTS "vms_resource_status" (
	"guid" BLOB(16) NOT NULL UNIQUE,
    "status" SMALLINT NOT NULL
);

CREATE TABLE IF NOT EXISTS "vms_kvpair" (
    "id" integer PRIMARY KEY AUTOINCREMENT,
    "resource_guid" BLOB(16) NOT NULL,
    "name" varchar(200) NOT NULL,
    "value" varchar(200) NOT NULL
);

CREATE UNIQUE INDEX idx_kvpair_name ON vms_kvpair (resource_guid, name);