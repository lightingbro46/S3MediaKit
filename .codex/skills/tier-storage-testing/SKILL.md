---
name: tier-storage-testing
description: Run S3MediaKit tier-storage unit and MinIO integration tests against an explicitly configured local test endpoint. Use for validating TierStorageManager storage changes; do not use production buckets or credentials.
---

# Tier storage testing

Validate tier-storage changes with isolated local state and an explicitly authorized MinIO test endpoint.

## Required environment

Require these variables without printing their values:

- `TIER_TEST_S3_ENDPOINT`
- `TIER_TEST_S3_ACCESS_KEY`
- `TIER_TEST_S3_SECRET_KEY`
- `TIER_TEST_S3_BUCKET` is optional and defaults to `s3mediakit-tier-test`.

Never copy credentials into source, config, patches, commands shown in reports, or logs. Stop if the endpoint or bucket may be production.

## Workflow

1. Inspect `git status --short` and preserve existing changes.
2. Check `<endpoint>/minio/health/live` without credentials.
3. Run `scripts/run_tier_storage_tests.sh` from this skill.
4. Report build, unit test, process-isolated `TierStorageManager` integration, and MinIO integration results separately. A skipped MinIO test is not a pass.
5. On failure, inspect only the unique `agent-runs/<pid>` object prefix. Do not delete the bucket or unrelated objects.

The manager integration executable runs every scenario in a fresh child process with a temporary SQLite database and filesystem root. Restart recovery uses separate prepare/verify processes sharing only that temporary database. The MinIO executable creates a unique object prefix, verifies upload/existence/download/content/delete, and removes its local temporary files. Read [references/scenarios.md](references/scenarios.md) for the coverage matrix.
