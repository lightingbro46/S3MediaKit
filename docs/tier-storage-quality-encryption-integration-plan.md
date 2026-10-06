# Implementation Plan: Tích hợp giảm chất lượng với record encryption

## Objective

Thực hiện phần tích hợp sau khi `feature/merge-record-encryption` đã được merge vào
`develop`, để `TRANSCODE_MOVE` có thể xử lý an toàn segment plaintext và encrypted mà
không làm mất trạng thái mã hóa, không ghi đè bản giảm chất lượng và không để lại biến
thể `.mp4`/`.mp4.enc` orphan ở pool nguồn.

Kết quả cuối cùng:

- Segment encrypted được xử lý theo pipeline `decrypt -> transcode -> re-encrypt -> store`.
- Output giữ đúng mode/key era của source và vẫn có encryption trailer ở tier đích.
- Quality move, immediate encryption và bulk re-encryption serialize theo cùng một
  segment identity ổn định.
- Re-encryption không được phép ghi kết quả vào một range đã chuyển pool hoặc thay đổi
  version kể từ lúc job lấy snapshot.
- Catalog range, TimeFile size, journal và file thực tế nhất quán sau success, retry và
  process restart.
- Deployment không bật encryption tiếp tục đi qua fast path hiện tại.

## Preconditions

- `feature/merge-record-encryption` đã được merge vào `develop` và migration
  `19_record_encryption.sql` đã là một phần của baseline.
- Nhánh quality reduction đã có migration `22_tier_quality_reduction.sql`, durable
  `tiering_job_segments`, TimeFile outbox và `BlockingExecutors::tiering()`.
- Trước khi bắt đầu Phase 1 phải rebase/merge nhánh implementation từ `develop`, giải
  quyết conflict nhưng chưa thay đổi semantics ở các file giao nhau:
  `BlockingExecutors.*`, `StorageTier.h`, `TierStorageBase.*`,
  `TierStorageManager.*`, `TieringJob.h`, `WebApiStorage.cpp` và `config.*`.
- Giữ chuẩn C++11; blocking disk/SQLite/S3/crypto/ffmpeg chỉ chạy trên blocking
  executor, không chạy trên EventPoller.
- Mọi transaction có ghi dùng `beginWriteTxn()`.
- Encryption key/salt/password không được ghi vào tiering job, log, REST response hoặc
  TimeFile.
- Không thay đổi policy JSON/FE contract của quality reduction và record encryption;
  đây là thay đổi orchestration nội bộ.

## Approved Integration Flow

```text
claim TRANSCODE_MOVE
  -> collect range snapshot + segment identities
  -> reject/defer segment awaiting immediate encryption
  -> acquire mutation leases in deterministic identity order
  -> revalidate range version/location and resolve actual .mp4/.mp4.enc variant
  -> for each segment
       plaintext: ffmpeg input = source
       encrypted: decrypt to job-owned staging input
       -> transcode to job-owned plaintext output
       -> if source encrypted, re-encrypt output with captured source key/mode
       -> validate codec/size plus expected encryption trailer
       -> atomic store using key matching the output variant
       -> journal STORED
  -> atomic range/version + journal COMMITTED + TimeFile outbox transaction
  -> delete both stale source variants under the same leases
  -> journal CLEANED
  -> release leases
```

`COPY_MOVE` tiếp tục copy ciphertext nguyên trạng. Không decrypt/re-encrypt một
`COPY_MOVE` chỉ để chuyển tier.

## Change Summary

1. Thêm optimistic mutation version vào `SegmentTierRange`.
2. Thay path-based busy check bằng per-segment mutation lease dùng chung.
3. Thêm API encryption nội bộ để tạo plaintext staging và bảo vệ lại transformed
   output mà không lộ key material ra `TierStorageManager`.
4. Tích hợp pipeline này vào local/NAS và object-storage quality move.
5. Làm cleanup/recovery aware với cả plaintext và encrypted variants.
6. Thêm deterministic race tests cho immediate encryption, bulk re-encryption và
   quality move.

## Files to Modify

### Storage catalog and migration

- `manager/Storage/TieringJob.h`
- `manager/Storage/TierQualityJob.h`
- `tests/unit/storage/test_storage_models.cpp`
- `tests/unit/storage/test_tier_txn_lock.cpp`
- `tests/unit/CMakeLists.txt`

### Encryption and tier orchestration

- `manager/Local/TierEncryptionManager.h`
- `manager/Local/TierEncryptionManager.cpp`
- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierMediaTransformer.h`
- `manager/Local/TierMediaTransformer.cpp`

### Integration tests and documentation

- `tests/integration/tier_storage/test_tier_storage_manager.cpp`
- `tests/integration/tier_storage/CMakeLists.txt`
- `docs/storage-tiering-flow.md`
- `docs/threading-blocking-work.md`
- `docs/api-storage-tiering.md`

## Files to Add

- `migration/mserver_updates/20_tier_segment_mutation_version.sql`
- `tests/unit/storage/test_tier_encryption_quality_migration.cpp`

Nếu số migration `20` đã được dùng trên `develop` tại thời điểm triển khai, đổi sang số
trống kế tiếp nhưng vẫn phải chạy sau cả migration 18 và 19. Không sửa migration đã
được deploy.

Không dự kiến xóa file.

## Implementation Steps

## Phase 0 — Establish the post-merge baseline

### Step 0.1 — Resolve merge conflicts without semantic integration

**Objective:** Tạo baseline build được, chứa đầy đủ cả hai feature trước khi thay đổi
concurrency contract.

**Files:** Các file giao nhau liệt kê trong Preconditions.

**Symbols:**

- `BlockingExecutors::{tiering,encryption}` và các thread-count accessors.
- `PolicyTierConfig`, `PolicyAdvancedRules` và encryption fields trong `StorageTier.h`.
- `TieringJob`, `SegmentTierRange` và `DECLARE_ENTITY(...)` tương ứng.
- `TierStorageManager::{collectSegmentFilesForJob,executeLocalTierMove,executeObjectStorageUpload}`.
- `TierStorageManager::registerEncryptionStorageBridge()`.

**Change:**

- Giữ cả hai dedicated executor và config key tương ứng.
- Giữ `origin_pool_id` xuyên suốt range split/merge/move.
- Giữ `operation`, `quality_profile_json` và các quality counters trên `TieringJob`.
- Giữ `dropSegmentsNotReadyToMove()` hoặc logic tương đương gọi
  `TierEncryptionManager::isSegmentPendingEncryption()`.
- Chưa cho `TRANSCODE_MOVE` chạy trên encrypted input; nếu cần để baseline an toàn,
  defer job với error code retryable `TIER_TRANSCODE_ENCRYPTION_INTEGRATION_PENDING`.

**Verification:**

- Build thành công với `ENABLE_TIER_STORAGE=ON`.
- Unit tests riêng của quality và encryption đều pass trước khi sang Phase 1.

**Phase completion criteria:** Không mất field/config/symbol của bên nào khi resolve
conflict; encrypted segment chưa thể bị transcode theo đường dẫn `.enc` trực tiếp.

## Phase 1 — Versioned segment mutation contract

### Step 1.1 — Add `mutation_version` to range persistence

**Objective:** Cung cấp generation/CAS ổn định; không dùng `updated_at` có độ phân giải
giây làm concurrency token.

**Files:**

- `migration/mserver_updates/20_tier_segment_mutation_version.sql`
- `manager/Storage/TieringJob.h`
- `manager/Storage/TierQualityJob.h`
- `tests/unit/storage/test_tier_encryption_quality_migration.cpp`
- `tests/unit/storage/test_storage_models.cpp`
- `tests/unit/storage/test_tier_txn_lock.cpp`
- `tests/unit/CMakeLists.txt`

**Symbols:**

- Add `SegmentTierRange::mutation_version` with default `0`.
- Extend `DECLARE_ENTITY(SegmentTierRange, ...)`.
- Extend `SegmentTierRangeRepository::snapshotWhereClause()` and
  `snapshotWhereParams()`.
- Extend range split/merge/update repositories to carry or increment the version.
- Extend `TierQualityTransferRepository::finalizeTransfer()`.

**Change:**

- Migration adds `mutation_version INTEGER NOT NULL DEFAULT 0`.
- Every mutation that changes file ownership, variant, tier, pool, size or lifecycle
  state increments the version in the same write transaction.
- Quality finalize compares expected version and increments it exactly once.
- A failed CAS leaves source and catalog untouched; already stored target remains
  journal-owned for retry/rollback.
- Range split creates independent rows with an inherited version followed by a single
  increment; normalize/merge must never lower a version.

**Verification:**

- Migration 18 -> 19 -> 20 preserves existing rows and initializes version `0`.
- A replacement and a tier finalize starting from the same version cannot both commit.
- Two mutations inside the same second are still distinguishable.

**Phase completion criteria:** All range mutation paths compile and tests prove stale
snapshots fail by version, not wall-clock timestamp.

### Step 1.2 — Replace path busy checks with stable mutation leases

**Objective:** Serialize every content-changing operation on the same logical segment.

**Files:**

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierEncryptionManager.h`
- `manager/Local/TierEncryptionManager.cpp`

**Symbols:**

- Replace/extend `TierStorageManager::{acquireSegmentLock,releaseSegmentLock,isSegmentFileBusy}`.
- Replace/extend `TierStorageManager::SegmentFileLock` as
  `SegmentMutationLease`.
- Extend `EncryptionStorageBridge::{lockSegmentFile,unlockSegmentFile}` to receive the
  stable segment identity.
- Update `TierEncryptionManager::SegmentGuard`.

**Change:**

- Canonical key is `(camera_id, stream_id, segment_start_time)`; file path is diagnostic
  metadata only.
- Lease acquisition is blocking only on `tiering()`/`encryption()` workers.
- A range job acquires all required leases in lexicographic canonical-key order before
  resolving source variants and holds them through catalog commit and source cleanup.
- Immediate encryption and bulk re-encryption acquire the same lease before resolving
  `.mp4`/`.mp4.enc`.
- Shutdown/cancellation must release every acquired lease via RAII.
- `isSegmentPendingEncryption()` remains a pre-admission optimization, not a locking
  primitive.

**Verification:**

- Unit or process-isolated test proves a second mutator blocks and later resumes.
- Same file with `.mp4` and `.mp4.enc` resolves to the same logical lease.
- Different segments can still mutate concurrently up to configured limits.

**Phase completion criteria:** Không còn check-then-act giữa `isSegmentFileBusy()` và
open/rename/delete; mọi writer dùng chung một lock domain.

## Phase 2 — Encryption-aware transform API

### Step 2.1 — Introduce an opaque encryption snapshot for tier transform

**Objective:** Cho quality pipeline decrypt/re-encrypt đúng key era mà không truy cập
password/key internals.

**Files:**

- `manager/Local/TierEncryptionManager.h`
- `manager/Local/TierEncryptionManager.cpp`
- `manager/Local/TierStorageManager.cpp`

**Symbols:**

- Add `TierTransformEncryptionSnapshot` with non-serializable in-memory state:
  encrypted flag, mode, origin pool, source variant and resolved key-era identity.
- Add `TierEncryptionManager::prepareTierTransformInput(...)`.
- Add `TierEncryptionManager::protectTierTransformOutput(...)`.
- Reuse `resolveOldKeyForFile()`, `decryptFileToPlain()` and `encryptPlainFile()`
  internally; do not make raw keys public.

**Change:**

- `prepareTierTransformInput()` must be called while caller owns the mutation lease.
- Plaintext source returns the original source path and an unencrypted snapshot.
- Encrypted source is decrypted to a caller-supplied, job-owned staging path, not the
  shared playback decrypt cache.
- Snapshot resolution verifies the source trailer/HMAC and records the actual mode/key
  candidate used.
- `protectTierTransformOutput()` re-encrypts the transformed plaintext with that exact
  mode/key and verifies the resulting trailer/HMAC before returning success.
- Temporary paths and error messages must not contain key material.

**Verification:**

- Plaintext prepare/protect is a no-op.
- Light and Strong encrypted fixtures round-trip and transformed output verifies under
  the original key era.
- Wrong/missing key fails before target store and leaves source intact.
- Staging input/output cleanup is idempotent.

**Phase completion criteria:** Transformer luôn nhận một MP4 plaintext hợp lệ; encrypted
source luôn tạo encrypted validated output trước khi storage backend được gọi.

### Step 2.2 — Make in-place re-encryption location-aware

**Objective:** Ngăn output re-encryption được ghi vào pool mới sau khi tier move đã
commit.

**Files:**

- `manager/Local/TierEncryptionManager.h`
- `manager/Local/TierEncryptionManager.cpp`
- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`

**Symbols:**

- Replace `TierStorageManager::replaceSegmentInPlace(...)` with
  `replaceSegmentIfUnchanged(...)`.
- Extend `EncryptionStorageBridge::replaceSegmentInPlace` accordingly.
- Update `TierEncryptionManager::{onRecordMP4Finished,runReencryptionJobInternal}`.

**Change:**

- Replacement receives expected `range_id`, `pool_id`, `tier`, `mutation_version` and
  logical segment identity captured after lease acquisition.
- Revalidate expectation immediately before file replacement.
- Write only to the expected pool/key; never query current range and silently redirect
  old content to its new pool.
- Remove stale counterpart and increment range version only after replacement succeeds.
- On mismatch return typed retryable result; bulk job rescans instead of overwriting.

**Verification:**

- Simulated move between re-encryption prepare and replace returns stale-snapshot and
  does not touch destination.
- Successful replacement updates exactly one variant and increments version once.

**Phase completion criteria:** Re-encryption không thể ghi bản resolution cũ đè lên
quality output của một move đã commit.

## Phase 3 — Integrate quality move with encryption

### Step 3.1 — Integrate local/NAS `TRANSCODE_MOVE`

**Objective:** Áp dụng full encrypted transform pipeline cho file-storage targets.

**Files:**

- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierMediaTransformer.h`
- `manager/Local/TierMediaTransformer.cpp`
- `manager/Storage/TierQualityJob.h`

**Symbols:**

- `TierStorageManager::collectSegmentFilesForJob()`.
- `TierStorageManager::executeLocalTierMove()`.
- `TierStorageManager::commitTierTransfer()` or the quality-specific finalize path.
- `TieringJobSegmentImp::{add,updateResult,markCleaned}`.

**Change:**

- Acquire leases, then recollect/revalidate source variant and range version.
- Use `prepareTierTransformInput()` before `TierMediaTransformer::transform()`.
- Use `protectTierTransformOutput()` before `uploadSegmentAtomic()` when snapshot is
  encrypted.
- Derive target storage key from protected output: encrypted output must use `.enc`;
  plaintext output must not.
- Journal `output_bytes` is final on-disk size including encryption trailer.
- TimeFile size and `SegmentTierRange.size_bytes` use the same final byte count.
- Validate both quality limits and encryption state before marking `STORED`.
- Keep leases until finalize and source cleanup complete.

**Verification:**

- Plain HOT -> reduced plaintext WARM remains unchanged from current behavior.
- Encrypted HOT -> reduced encrypted WARM preserves mode/key and meets resolution,
  bitrate and FPS limits after decrypting for inspection.
- Protected/evidence `COPY_MOVE` copies ciphertext byte-for-byte.

**Phase completion criteria:** Local/NAS quality move never feeds ciphertext to ffmpeg
and never leaves encrypted input as plaintext at the destination.

### Step 3.2 — Integrate object-storage `TRANSCODE_MOVE`

**Objective:** Áp dụng cùng semantics cho MinIO/S3/COLD.

**Files:**

- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierObjectStorage.h`
- `manager/Local/TierObjectStorage.cpp`
- `manager/Storage/TierQualityJob.h`

**Symbols:**

- `TierStorageManager::executeObjectStorageUpload()`.
- Object key selection currently using `keyForVariant()`.
- Restore path in `TierStorageManager::executeRestoreSegment()`.

**Change:**

- Upload protected output under the key matching its actual trailer/variant.
- Retry may reuse a `STORED` target only after size, variant and encryption trailer are
  validated; size equality alone is insufficient.
- Rollback deletes only target artifacts owned by the current job.
- Restore continues to preserve `.enc` spelling and decrypt through the encryption
  manager.

**Verification:**

- Encrypted WARM -> reduced encrypted COLD -> restore -> playback succeeds.
- Plain and encrypted object keys do not coexist after successful finalize/cleanup.
- Retry after upload-before-commit reuses the valid encrypted target without running
  ffmpeg again.

**Phase completion criteria:** Object-storage behavior matches local/NAS behavior and
presigned/plaintext exposure is not introduced.

## Phase 4 — Recovery, cleanup and resource control

### Step 4.1 — Make recovery and cleanup variant-aware

**Objective:** Không để orphan plaintext/ciphertext qua crash hoặc mode transition.

**Files:**

- `manager/Local/TierStorageManager.cpp`
- `manager/Storage/TierQualityJob.h`

**Symbols:**

- `TierStorageManager::{processCommittedSourceCleanup,cleanupOrphanTranscodeStaging}`.
- Post-commit cleanup blocks in `executeLocalTierMove()` and
  `executeObjectStorageUpload()`.
- `TieringJobSegmentImp::findCommittedForCleanup()`.

**Change:**

- Persist enough non-secret variant metadata in the segment journal only if actual
  source/target spelling cannot be derived safely after restart.
- Under mutation lease, delete both source counterpart spellings belonging to the old
  pool; ENOENT remains success.
- Never delete a path/object after its range/version no longer matches the committed
  job ownership record.
- Staging cleanup skips active lease/job paths and recognizes decrypt, transcode and
  re-encrypt suffixes.
- Recovery validates trailer plus transform profile fingerprint before reusing output.

**Verification:**

- Restart after decrypt, transcode, re-encrypt, store, commit and source-delete
  boundaries converges without data loss.
- No plaintext staging file survives completed/failed job beyond configured cleanup
  policy.

**Phase completion criteria:** Mỗi crash point hội tụ về một readable target và không
còn source variants ngoài journal ownership.

### Step 4.2 — Add shared admission control

**Objective:** Tránh tiering FFmpeg và bulk encryption làm nghẽn cùng storage pool.

**Files:**

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierEncryptionManager.cpp`
- `manager/Common/BlockingExecutors.h`
- `manager/Common/BlockingExecutors.cpp`
- `src/Common/config.h`
- `src/Common/config.cpp`
- `conf/config.ini`

**Symbols:**

- Existing tier dispatch state and `BlockingExecutors::{tiering,encryption}`.
- Add per-pool mutation admission counters shared through the storage bridge.

**Change:**

- Keep separate worker pools but cap concurrent disk-heavy mutations per storage pool.
- Immediate encryption has priority over bulk re-encryption; running work is not
  preempted.
- Quality jobs that cannot obtain capacity remain retryable/PENDING rather than FAILED.
- Shutdown stops admission, cancels/waits active child processes and drains leases
  before manager destruction.

**Verification:**

- Concurrency never exceeds configured global/per-pool limits.
- Work on different pools progresses concurrently.
- Stop waits without deadlock and leaves durable jobs recoverable.

**Phase completion criteria:** Encryption và transcode không gây starvation cho REST,
storage maintenance hoặc recording-finalization path.

## Phase 5 — Regression matrix and rollout documentation

### Step 5.1 — Add deterministic integration scenarios

**Objective:** Biến các race đã phát hiện thành regression tests có barrier rõ ràng.

**Files:**

- `tests/integration/tier_storage/test_tier_storage_manager.cpp`
- `tests/integration/tier_storage/CMakeLists.txt`
- `tests/unit/storage/test_tier_txn_lock.cpp`

**Scenarios:**

1. Immediate encryption acquires the lease after quality collect but before source
   resolve; quality must wait/revalidate, không báo missing.
2. Quality holds the lease while ffmpeg runs; re-encryption waits and cannot rename
   source.
3. Re-encryption prepares output, range version changes, replacement fails stale and
   cannot overwrite destination.
4. Encrypted HOT -> quality WARM preserves encryption and reduced media properties.
5. Encrypted WARM -> quality COLD -> restore preserves both properties.
6. Crash after target store but before range commit reuses only a validated output.
7. Crash after commit but before source cleanup removes both stale variants.
8. Encryption disabled follows plaintext fast path and existing quality tests remain
   unchanged.

Không dùng sleep để tạo race. Test hook/barrier chỉ được compile trong test target hoặc
được inject qua interface nội bộ.

**Verification:** Mỗi scenario chạy trong process/database/temp root độc lập như các
tier-manager integration tests hiện tại.

**Phase completion criteria:** Tất cả interleaving quan trọng đều có regression test
deterministic, bao gồm local/NAS và MinIO khi endpoint test được cấu hình.

### Step 5.2 — Document operations and rollout

**Objective:** Giúp vận hành bật hai tính năng theo thứ tự an toàn.

**Files:**

- `docs/storage-tiering-flow.md`
- `docs/threading-blocking-work.md`
- `docs/api-storage-tiering.md`

**Change:**

- Mô tả encrypted transcode pipeline, staging footprint và worker-pool interaction.
- Ghi rõ quality output kế thừa encryption state; FE không cần thêm toggle kết hợp.
- Rollout: migrate -> deploy disabled -> smoke plaintext/encrypted -> bật một policy ->
  quan sát -> tăng concurrency.
- Metrics/logs phải phân biệt decrypt, transcode, re-encrypt, store, CAS conflict và
  cleanup; không log secrets.

**Phase completion criteria:** Runbook có rollback bằng kill switch quality reduction;
encryption policy và encrypted data vẫn được giữ nguyên khi rollback.

## Interface Changes

### Public REST / FE

- Không thêm endpoint hoặc field bắt buộc.
- Các toggle quality và encryption hiện có vẫn độc lập ở FE.
- Job response có thể bổ sung trạng thái/error code nội bộ nhưng field cũ giữ nguyên.

### Internal C++

- `EncryptionStorageBridge` chuyển từ path-based lock/replace sang stable identity và
  expected snapshot.
- `TierEncryptionManager` thêm prepare/protect API dành cho tier transform.
- `SegmentTierRange` thêm `mutation_version`.
- Không public raw encryption key qua bridge.

### Configuration

- Giữ nguyên `blocking.tieringThreads`, `blocking.encryptionThreads` và các quality
  kill switch hiện có.
- Chỉ thêm per-pool mutation limit nếu không thể dùng chung limit tiering hiện tại;
  default phải bảo thủ và backward-compatible.

## Data / Schema Changes

- Migration 18: durable quality state, chạy trước.
- Migration 19: encryption config/key ranges và `origin_pool_id`, chạy tiếp theo.
- Migration 20: `segment_tier_ranges.mutation_version`, chạy cuối.
- Existing rows dùng version `0`; không cần media backfill.
- `origin_pool_id` không đổi sau transcode/move vì vẫn là namespace chọn decryption key.
- `size_bytes` và TimeFile `file_size` phản ánh encrypted output thực tế, gồm trailer.
- Không lưu key, password hoặc decrypted staging path lâu dài trong database.

## Concurrency and Ownership

| Resource | Owner | Synchronization |
|---|---|---|
| Source/target content của một segment | Segment mutator hiện tại | `SegmentMutationLease` theo logical identity |
| Range location/version | SQLite repository | `BEGIN IMMEDIATE` + expected `mutation_version` CAS |
| Transcode staging | `tiering_job_segments` job | Job-specific path + journal state |
| Playback decrypt cache | `TierEncryptionManager` | Giữ riêng; quality không dùng cache này |
| Tier worker capacity | `TierStorageManager` dispatch state | Global/per-pool counters |
| Encryption worker capacity | `TierEncryptionManager` + storage bridge | Shared per-pool admission, dedicated executor |

Lock order bắt buộc:

1. Acquire segment mutation leases theo canonical-key order.
2. Không giữ `_pool_cache_mtx`, `_mutex` của encryption hoặc progress mutex trong lúc
   chờ lease/ffmpeg/S3.
3. Mở SQLite write transaction chỉ cho finalize ngắn; không giữ transaction trong lúc
   chạy crypto, ffmpeg hoặc network I/O.
4. Release database transaction trước khi gọi TimeFile patch hoặc filesystem cleanup.

## Compatibility

- Policy cũ, job `COPY_MOVE` cũ và deployment không encryption giữ nguyên hành vi.
- Job `TRANSCODE_MOVE` đã tồn tại trước upgrade được retry theo pipeline mới; không reuse
  target `.enc` nếu chưa xác minh trailer/profile.
- Playback URL/storage key logic giữ nguyên; encrypted output tiếp tục dùng `.enc`.
- Không yêu cầu backfill WARM/COLD hiện có.
- Nếu migration 20 chưa áp dụng, binary mới phải fail startup migration rõ ràng thay vì
  chạy mà bỏ qua version protection.

## Test Plan

### Unit

- Entity/migration round-trip của `mutation_version`.
- CAS conflict giữa replace và finalize.
- Logical identity normalization cho `.mp4`/`.mp4.enc`.
- Prepare/protect plaintext, Light và Strong encryption.
- Cancellation/RAII release mutation lease.
- Transformer validation đọc plaintext staging, không đọc ciphertext.

### Integration

- Các eight race/recovery scenarios ở Phase 5.1.
- Local disk và NAS path.
- MinIO upload/restore với encrypted reduced output.
- H.264 và H.265 software encoder; hardware encoder vẫn theo capability/fallback tests
  hiện có.

### Performance / soak

- Đo throughput khi immediate encryption, one bulk re-encryption và quality move cùng
  hoạt động trên một pool.
- Xác nhận recording-finalization latency không tăng không giới hạn.
- Theo dõi staging high-water mark và số CAS retries.

## Verification Commands

Build tối thiểu:

```bash
cmake --build build -j2
```

Focused unit tests:

```bash
ctest --test-dir build --output-on-failure \
  -R 'unit\.(storage_all|tier_transcode_all|tier_quality_migration_all|tier_txn_lock_all|blocking_executors_all)'
```

FFmpeg integration:

```bash
ctest --test-dir build --output-on-failure \
  -R 'integration\.tier_transcode_ffmpeg_all'
```

Tier manager process-isolated scenarios:

```bash
ctest --test-dir build --output-on-failure \
  -R 'integration\.tier-manager\.'
```

MinIO validation phải dùng endpoint/bucket test được cấu hình rõ ràng và script:

```bash
.codex/skills/tier-storage-testing/scripts/run_tier_storage_tests.sh
```

Không coi MinIO test bị skip là pass.

## Rollout Order

1. Merge encryption vào `develop`; chạy baseline encryption tests.
2. Merge conflict-only baseline của quality; giữ encrypted `TRANSCODE_MOVE` deferred.
3. Deploy migration 20 cùng binary có version-aware readers/writers.
4. Bật mutation lease và version CAS nhưng vẫn giữ quality kill switch off.
5. Bật encrypted transform pipeline trên test/staging.
6. Canary một HOT -> WARM policy với concurrency `1`.
7. Xác minh output quality, trailer/HMAC, playback, restore, size accounting và orphan
   count.
8. Mở rộng policy/concurrency theo pool; chỉ sau đó cho phép bulk re-encryption chạy
   đồng thời.

## Risks

- Giữ lease cả range trong thời gian encode dài có thể làm immediate encryption chờ;
  giảm bằng range chunk nhỏ và admission priority, không bằng cách nhả lock sớm.
- Encryption trailer làm final size khác plaintext transcode size; mọi accounting phải
  lấy sau bước protect.
- Hardware encoder retry có thể kéo dài lease; timeout/cancellation phải kill child và
  release RAII sạch.
- NAS/shared filesystem giữa nhiều process không được bảo vệ bởi mutex in-process. Nếu
  deployment cho phép nhiều MediaServer cùng mutate một pool, cần distributed lease hoặc
  single-writer ownership trước khi rollout; việc này nằm ngoài implementation mặc định.
- Migration number có thể bị chiếm trên `develop`; phải kiểm tra lại ngay trước khi tạo
  migration, không sửa file migration đã phát hành.

## Out of Scope

- Thay đổi thuật toán/mode mã hóa hoặc password derivation.
- Distributed transcoding service.
- Re-transcode toàn bộ dữ liệu WARM/COLD đã tồn tại trước khi bật feature.
- Thay đổi live-stream codec/profile.
- Cho phép FE chọn “decrypt khi archive”; output kế thừa encryption state bắt buộc.
- Distributed lock đa node nếu hiện tại mỗi writable pool đã có single-writer owner.

## Exit Criteria

- Encrypted input không bao giờ được đưa trực tiếp cho ffprobe/ffmpeg.
- Encrypted quality output vẫn xác minh được trailer/HMAC với source key era.
- Không interleaving nào cho phép re-encryption ghi đè output đã giảm chất lượng.
- Range/version, job journal, TimeFile size và physical bytes khớp sau success/retry.
- Không còn plaintext/encrypted orphan qua toàn bộ crash matrix.
- Existing plaintext, protected-video và `COPY_MOVE` regression tests pass.
- Focused unit, FFmpeg integration, tier-manager integration và configured MinIO suite
  đều pass; skipped MinIO được báo riêng.
- Tài liệu rollout/rollback hoàn chỉnh và quality kill switch mặc định vẫn tắt.
