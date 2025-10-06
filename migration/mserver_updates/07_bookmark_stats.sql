CREATE TABLE bookmark_stats (
    device_id BLOB NOT NULL UNIQUE PRIMARY KEY,
    count INTEGER,
    size INTEGER
);

INSERT INTO bookmark_stats (device_id, count, size)
SELECT 
    camera_guid AS device_id,
    COUNT(*) AS count,
    0 AS size
FROM bookmarks
WHERE camera_guid IS NOT NULL
GROUP BY camera_guid;