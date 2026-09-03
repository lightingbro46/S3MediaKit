# Tier-storage integration scenarios

Use a fresh SQLite database and filesystem root for every scenario. Trigger `TierStorageManager::runTieringCycle()` directly and poll durable state with a deadline; do not wait for the five-minute production timer or use arbitrary sleeps.

## Priority 0

1. Registering the same HOT segment twice remains idempotent. **Automated.**
2. HOT to NAS preserves content, updates the range, then removes the source. **Automated.**
3. HOT to MinIO uploads the exact object before removing the source. **Automated; requires MinIO.**
4. Manual MinIO restore creates durable child work and restores identical content. **Automated; requires MinIO.**
5. Duplicate restore requests create at most one active child per physical segment. **Automated.**
6. Protected video and active retention overrides prevent physical deletion. **Automated.**
7. Approved expiry deletes physical data and enqueues exact timefile cleanup. **Automated.**
8. Direct playback resolution accepts readable regular, empty, and symlinked files; rejects an unreadable fallback; and requires restore when object-cache data is missing. **Automated.**
9. SearchEngine exposes HOT/COLD tier metadata and `restore_required` from durable ranges. **Automated.**
10. Saved camera statistics restore `CameraOption::recordRootPath`, then persist the effective tier-policy root. **Automated.**
11. Policy create/update/delete changes the camera HOT root and execution path, guards assigned deletion, then falls back to system default. **Automated.**

## Priority 1

1. Recover tiering and restore children left `RUNNING` across a fresh process. **Automated.**
2. Keep concurrent restore workers within `storage.auto_restore_max_concurrent`. **Automated.**
3. Fail safely when MinIO becomes unavailable during upload or download.
4. Complete backfill only after the stable window cursor reaches its end.
5. Select a deterministic valid custom HOT pool during bootstrap.

Run restart-recovery cases as separate processes because database pools, configuration and `TierStorageManager` are process-lifetime singletons.
