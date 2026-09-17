-- No-op. This migration used to delete from vms_kvpair, which lives in the ESC database, so it
-- failed on the MediaServer database on every start and was never recorded as applied.
-- The cleanup now runs as migration/updates/11_remove_camera_runtime_kv.sql.
-- The file is kept (instead of deleted) so that deployments upgraded by copying files over the
-- old tree replace the failing statement, apply this no-op once, and stop logging the error.
DELETE FROM migrationhistory WHERE 1 = 0;
