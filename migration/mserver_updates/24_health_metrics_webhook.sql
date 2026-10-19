-- Health monitor history: webhook calls to hook.api_url (cluster.webhook of /media/monitor/summary).
-- -1 = no data (samples written before this migration).
ALTER TABLE "health_metrics"
    ADD COLUMN "webhook_failure_pct" REAL NOT NULL DEFAULT -1;

ALTER TABLE "health_metrics"
    ADD COLUMN "webhook_queued_max" INTEGER NOT NULL DEFAULT -1;

ALTER TABLE "health_metrics"
    ADD COLUMN "webhook_queue_wait_max_ms" INTEGER NOT NULL DEFAULT -1;
