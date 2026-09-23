CREATE TABLE IF NOT EXISTS "record_encryption_configs" (
    id TEXT NOT NULL UNIQUE PRIMARY KEY,
    mode INTEGER NOT NULL DEFAULT 0,
    verifySalt TEXT NOT NULL DEFAULT '',
    verifyHash TEXT NOT NULL DEFAULT '',
    saltEnc TEXT NOT NULL DEFAULT '',
    saltHmac TEXT NOT NULL DEFAULT '',
    encryptOldVideo INTEGER NOT NULL DEFAULT 0,
    updatedAt INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS "record_encryption_tier_ranges" (
    tier_id TEXT NOT NULL,
    start_time INTEGER NOT NULL,
    end_time INTEGER NOT NULL,
    salt_enc TEXT NOT NULL DEFAULT '',
    salt_hmac TEXT NOT NULL DEFAULT '',
    key_hash TEXT NOT NULL DEFAULT '',
    mode INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    PRIMARY KEY (tier_id, start_time)
);

CREATE INDEX IF NOT EXISTS idx_record_encryption_tier_ranges_tier_id
ON record_encryption_tier_ranges(tier_id);

CREATE INDEX IF NOT EXISTS idx_record_encryption_tier_ranges_time
ON record_encryption_tier_ranges(start_time, end_time);

ALTER TABLE segment_tier_ranges
ADD COLUMN origin_pool_id TEXT NOT NULL DEFAULT '';

UPDATE segment_tier_ranges
SET origin_pool_id = pool_id
WHERE origin_pool_id = ''
  AND tier = 'HOT'
  AND pool_id IS NOT NULL
  AND pool_id <> '';

CREATE INDEX IF NOT EXISTS idx_segment_tier_ranges_origin_pool
ON segment_tier_ranges(origin_pool_id);