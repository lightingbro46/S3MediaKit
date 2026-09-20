-- Remove camera runtime statistics that older builds stored as ESC resource kv pairs.
-- Only camera configuration is replicated in vms_kvpair now (see docs/api-sync.md, "Dữ liệu ESC").
-- Moved here from mserver_updates/15_remove_camera_runtime_kv.sql: vms_kvpair lives in the ESC
-- database, so that file always failed on the MediaServer database and never ran.
DELETE FROM vms_kvpair
WHERE name IN (
    'mediaStreams',
    'transportStats',
    'storageInfos',
    'streamStatisticInfos',
    'streamStorageInfos',
    'deviceStatistic'
);
