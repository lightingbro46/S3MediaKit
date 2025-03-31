CREATE TABLE IF NOT EXITS "vms_manufacture" (
    "id" SERIAL PRIMARY KEY,
    "name" VARCHAR(200) NOT NULL UNIQUE
);

INSERT INTO "vms_manufacture" VALUES (1, 'DESKTOP_CAMERA');
INSERT INTO "vms_manufacture" VALUES (2, 'S3PRO');
INSERT INTO "vms_manufacture" VALUES (3, 'OnvifDevice');
INSERT INTO "vms_manufacture" VALUES (4, 'THIRD_PARTY');

CREATE TABLE IF NOT EXITS "vms_resourcetype" (
    "id" SERIAL PRIMARY KEY,
    "name" VARCHAR(200) NOT NULL,
    "description" VARCHAR(200) NOT NULL,
    "manufacture_id" INTEGER NULL,
    guid BLOB(16)
);

INSERT INTO "vms_resourcetype" VALUES (1, 'User', NULL, NULL, X'87d3316e611e41af88cbfac4fab4a4d8');
INSERT INTO "vms_resourcetype" VALUES (2, 'Server', NULL, NULL, X'939e8557c25347daa225ba1307813009');
INSERT INTO "vms_resourcetype" VALUES (3, 'Storage', NULL, NULL, X'08f9de9f11654cd1a6562d8730c83a41');
INSERT INTO "vms_resourcetype" VALUES (4, 'Local', NULL, NULL, X'45784f3706fe46e18d09d1c25a88c284');
INSERT INTO "vms_resourcetype" VALUES (5, 'Camera', NULL, NULL, X'8802c806d137463491f710ef31eee07b');
INSERT INTO "vms_resourcetype" VALUES (6, 'WebPage', NULL, NULL, X'bd5da3468d914ae1aa013164d6206f6e');
INSERT INTO "vms_resourcetype" VALUES (7, 'AnalyticsPlugin', NULL, NULL, X'd05305160dbd4d259a7cfe8e85959f26');
INSERT INTO "vms_resourcetype" VALUES (8, 'AnalyticsEngine', NULL, NULL, X'23b20f3a19e1422f9b9a087bb28106ba');
INSERT INTO "vms_resourcetype" VALUES (9, 'Videowall', NULL, NULL, X'd82a5244-def1-444f-8153-776b4ea0f78d');
INSERT INTO "vms_resourcetype" VALUES (10, 'ONVIF', NULL, 3, X'48af1ccb-3bc6-4596-be05-25c42f4014c4');
INSERT INTO "vms_resourcetype" VALUES (11, 'OnvifDevice Camera', NULL, 3, X'8a24339a-bba8-428c-8c4a-8889f3ad0d33');

CREATE TABLE IF NOT EXITS "vms_resourcetype_parent" (
    "id" SERIAL PRIMARY KEY,
    "name" VARCHAR(200) NOT NULL,
    "description" VARCHAR(200) NOT NULL,
    "manufacture_id" INTEGER NULL,
    guid BLOB(16)
);