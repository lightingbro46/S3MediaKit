-- Storage Tiering: logical capacity for object-storage pools. With
-- capacity_source = 'QUOTA' usage is the byte sum of the pool's stored ranges
-- against capacity_quota_bytes instead of the raw cluster metric.
ALTER TABLE "storage_pools"
    ADD COLUMN "capacity_quota_bytes" INTEGER;

ALTER TABLE "storage_pools"
    ADD COLUMN "capacity_source" TEXT NOT NULL DEFAULT 'METRICS';
