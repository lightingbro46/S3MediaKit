DELETE FROM vms_kvpair
WHERE name IN (
    'mediaStreams',
    'transportStats',
    'storageInfos',
    'streamStatisticInfos',
    'streamStorageInfos',
    'deviceStatistic'
);