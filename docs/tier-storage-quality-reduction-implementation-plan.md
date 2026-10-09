# Implementation Plan: Giảm chất lượng khi chuyển dữ liệu giữa các tier

## Objective

Triển khai thiết kế trong [tier-storage-quality-reduction-architecture.md](tier-storage-quality-reduction-architecture.md) theo các phase repository-level có thể review và kiểm thử độc lập.

Kết quả cuối cùng:

- FE cấu hình master switch và profile chất lượng theo WARM/COLD.
- Mọi move retention, pressure và external có profile phù hợp đều dùng durable `TRANSCODE_MOVE`.
- Protected/evidence range được tách và move bằng `COPY_MOVE` nguyên bản.
- ffmpeg chạy ngoài process media pipeline, tự chọn NVENC → QSV → software.
- Segment state, target output, aggregate range và TimeFile size phục hồi đúng sau crash.
- Source không bị xóa khi transform/store/finalize thất bại.
- Tính năng mặc định tắt; job/policy cũ tiếp tục hoạt động như trước.

## Preconditions

- Architecture design và requirement plan đã được duyệt.
- Implementation giữ chuẩn C++11.
- Không đưa blocking I/O hoặc chờ child process lên EventPoller/`WorkThreadPool`.
- Mọi transaction có ghi dùng `beginWriteTxn()`.
- AWS SDK vẫn chỉ cần cho MinIO; transcoding local/NAS không phụ thuộc `ENABLE_FFMPEG` library flag vì dùng executable bên ngoài.
- `22_tier_quality_reduction.sql` là migration MediaServer DB tiếp theo; không sửa migration đã áp dụng.
- Các table mới là node-local operational state trong `Database::kMediaServerDb`, không đăng ký `SyncManager`.
- Không backfill video đã nằm ở WARM/COLD.

## Change Summary

1. Thêm policy/API/config contract và schema backward-compatible.
2. Thêm process runner an toàn, transformer interface/concrete implementation và capability registry.
3. Thêm durable per-segment journal, TimeFile size outbox và atomic finalize repository.
4. Tách range protected, snapshot profile và chuyển tier dispatcher sang dedicated blocking executor.
5. Tích hợp transform/store/recovery/cleanup cho NAS/local/MinIO.
6. Hoàn thiện REST response, observability và tài liệu vận hành.
7. Chốt regression, ffmpeg integration, MinIO integration và performance gates.

## Files to Modify

### Configuration and threading

- `src/Common/config.h`
- `src/Common/config.cpp`
- `manager/Common/BlockingExecutors.h`
- `manager/Common/BlockingExecutors.cpp`
- `conf/config.ini`

### Policy, persistence and API mapping

- `manager/Local/StorageTier.h`
- `manager/Storage/TieringJob.h`
- `manager/Local/TierStorageBase.h`
- `manager/Local/TierStorageBase.cpp`
- `server/WebApiStorage.cpp`

### Tier execution and storage

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierFileStorageBase.h`
- `manager/Local/TierFileStorageBase.cpp`
- `manager/Local/TierObjectStorage.h`
- `manager/Local/TierObjectStorage.cpp`

### TimeFile metadata

- `manager/Local/TimeRebuilder.h`
- `manager/Local/TimeRebuilder.cpp`
- `manager/Local/StorageManager.h`
- `manager/Local/StorageManager.cpp`

### Build, tests and docs

- `tests/unit/CMakeLists.txt`
- `tests/unit/storage/test_storage_entities.cpp`
- `tests/unit/storage/test_storage_models.cpp`
- `tests/unit/storage/test_tier_txn_lock.cpp`
- `tests/unit/manager/test_blocking_executors.cpp`
- `tests/unit/manager/test_time_file.cpp`
- `tests/unit/server/test_storage_pool_api.cpp`
- `tests/integration/tier_storage/CMakeLists.txt`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`
- `tests/integration/tier_storage/test_tier_object_storage.cpp`
- `tests/performance/run_tier_storage_metadata_benchmark.sh`
- `docs/api-storage-tiering.md`
- `docs/storage-tiering-flow.md`
- `docs/threading-blocking-work.md`
- `docs/README.md`

## Files to Add

- `migration/mserver_updates/22_tier_quality_reduction.sql`
- `manager/Common/BlockingProcessRunner.h`
- `manager/Common/BlockingProcessRunner.cpp`
- `manager/Local/TierMediaTransformer.h`
- `manager/Local/TierMediaTransformer.cpp`
- `manager/Storage/TierQualityJob.h`
- `tests/unit/manager/test_blocking_process_runner.cpp`
- `tests/unit/manager/test_tier_media_transformer.cpp`
- `tests/unit/storage/test_tier_quality_migration.cpp`
- `tests/integration/tier_storage/test_tier_media_transformer.cpp`

Không xóa file nào. `manager/CMakeLists.txt` dùng glob cho `manager/*/*.cpp`, nên không cần sửa để nhận các source mới.

## Implementation Steps

## Phase 1 — Contract, configuration and migration

### Step 1.1 — Add policy models and defaults

**Objective:** Hoàn thiện contract C++/JSON mà chưa thay đổi luồng move.

**Files:**

- `manager/Local/StorageTier.h`
- `tests/unit/storage/test_storage_entities.cpp`
- `tests/unit/storage/test_storage_models.cpp`

**Symbols:**

- Add `TierQualityProfile`.
- Extend `PolicyTierConfig::toJson()` and `PolicyTierConfig::fromJson()`.
- Extend `PolicyAdvancedRules::{toJson,fromJson}` with `enable_tier_quality_reduction`.
- Extend `getSystemDefaultPolicyTierConfig()` and `getSystemDefaultPolicyAdvancedRules()` defaults.

**Change:**

- Add fields `enabled`, `video_codec`, `max_width`, `max_height`, `max_fps`, `max_video_bitrate_kbps`.
- Default profile and master switch to disabled.
- Keep absent JSON fields backward-compatible.
- Add helpers `tierQualityCodecToString/fromString` only if an enum is used; invalid text must remain distinguishable for validation instead of silently becoming H.264.

**Verification:**

- `StoragePolicyTest.RoundTripsNestedConfiguration` covers full profile round-trip and old JSON defaults.
- `StorageModelsTest.RoundTripsEveryPolicyAdvancedRule` covers the new master switch.

**Phase completion criteria:** Policy JSON old/new round-trips without changing effective behavior; feature remains disabled everywhere.

### Step 1.2 — Add config keys and dedicated executor

**Objective:** Chuẩn bị operational controls và thread boundary trước khi có transcoding.

**Files:**

- `src/Common/config.h`
- `src/Common/config.cpp`
- `manager/Common/BlockingExecutors.h`
- `manager/Common/BlockingExecutors.cpp`
- `conf/config.ini`
- `tests/unit/manager/test_blocking_executors.cpp`

**Symbols:**

- Add `Storage::kTierTranscodeEnabled`, `kTierTranscodeFfmpegBin`, `kTierTranscodeFfprobeBin`, `kTierTranscodeStagingPath`, `kTierTranscodeTimeoutSeconds`, `kTierTranscodeMaxConcurrent`, `kTierTranscodeH264Encoders`, `kTierTranscodeH265Encoders`.
- Add `Blocking::kTieringThreads`.
- Add `BlockingExecutors::tiering()` and `BlockingExecutors::tieringThreadCount()`.

**Change:**

- `tier_transcode_enabled=0`, `tier_transcode_staging_path=./tier-transcode`, `tier_transcode_timeout_seconds=600` và `tier_transcode_max_concurrent=1`.
- Binary defaults are `ffmpeg`/`ffprobe`; staging path is server-managed and outside playback roots.
- Encoder defaults are `h264_nvenc,h264_qsv,libx264` and `hevc_nvenc,hevc_qsv,libx265`.
- `blocking.tieringThreads <= 0` resolves to current `storage.tiering_transfer_max_concurrent`; it is a plain `ThreadPool`, not an EventPoller.
- Unit test asserts pool identity is stable, distinct from api/storage/ffmpeg and thread name is `tiering worker`.

**Verification:**

- Target `unit_blocking_executors` passes.
- Configure/build with `ENABLE_TIER_STORAGE=OFF` still succeeds.

**Phase completion criteria:** Dedicated executor exists and no existing workload has moved to it yet; default config cannot start transcoding.

### Step 1.3 — Apply schema migration and C++ entities

**Objective:** Có schema bền vững trước khi code runtime sử dụng field/table mới.

**Files:**

- `migration/mserver_updates/22_tier_quality_reduction.sql`
- `manager/Storage/TieringJob.h`
- `manager/Storage/TierQualityJob.h`
- `tests/unit/CMakeLists.txt`
- `tests/unit/storage/test_storage_models.cpp`
- `tests/unit/storage/test_tier_quality_migration.cpp`

**Symbols:**

- Extend `TieringJob` and `DECLARE_ENTITY(TieringJob, ...)`.
- Add `TieringJobSegment`, `TierTimefileSizeUpdate` entities.
- Add real `TieringJobSegmentRepository`/`TieringJobSegmentImp` and `TierTimefileSizeUpdateRepository`/`TierTimefileSizeUpdateImp`; do not alias Repository as Imp.

**Migration order inside file:**

1. `ALTER TABLE tiering_jobs` add `operation TEXT NOT NULL DEFAULT 'COPY_MOVE'`.
2. Add `quality_profile_json TEXT NOT NULL DEFAULT '{}'`.
3. Add `output_bytes`, `transcoded_segment_count`, `preserved_segment_count` as non-null integer default `0`.
4. Add `encoder_summary_json TEXT NOT NULL DEFAULT '{}'` để job list không cần aggregate child rows theo từng item.
5. Create `tiering_job_segments` with composite primary key `(job_id, stream_id, segment_path)`.
6. Add indexes `(job_id,status)`, `(status,updated_at)` and `(range_id,status)`.
7. Create `tier_timefile_size_updates` with primary key `(camera_id, stream_id, segment_path)`; keep `job_id`/`range_id` as metadata of the latest move.
8. Add indexes `(status,updated_at)` and `(range_id,status)`.

Không thêm foreign key vì MediaServer schema hiện không bật/không dùng SQLite foreign keys; lifecycle child được cleanup atomically/explicitly trước khi prune parent.

**Verification:**

- Migration test tạo legacy `tiering_jobs` row, áp dụng migration 18, xác nhận defaults và data cũ còn nguyên.
- Chạy migration trên database trống và xác nhận toàn bộ entity columns/primary keys bằng `EntityTraits`.
- Test rerun chỉ cho các `CREATE ... IF NOT EXISTS`; ghi rõ `ALTER TABLE` được bảo vệ bởi migration history và không chạy trực tiếp lần hai.
- Add CMake target/aggregate test `unit_tier_quality_migration` / `unit.tier_quality_migration_all`.

**Phase completion criteria:** Database cũ nâng cấp không mất dữ liệu; binary mới đọc job cũ thành `COPY_MOVE`; tables mới không tham gia cluster sync.

## Phase 2 — Safe process runner and media transformer

### Step 2.1 — Implement argv-based child process execution

**Objective:** Tạo primitive chạy ffmpeg/ffprobe an toàn và có thể test độc lập.

**Files:**

- `manager/Common/BlockingProcessRunner.h`
- `manager/Common/BlockingProcessRunner.cpp`
- `tests/unit/manager/test_blocking_process_runner.cpp`
- `tests/unit/CMakeLists.txt`

**Symbols:**

- `BlockingProcessRequest`: executable, argv, log/output limit, timeout, cancellation handle.
- `BlockingProcessResult`: exit code, timeout/cancel flags, bounded stdout/stderr/error.
- `BlockingProcessRunner::run()`.
- `BlockingProcessCancellation`: atomic cancel state and active child/process-group handle.

**Change:**

- POSIX executes argv directly with `fork/execvp` and a separate process group; never invoke `/bin/sh`.
- Windows uses `CreateProcess` with one centralized argv escaping function.
- Timeout/cancel terminates the process group, waits/reaps child and returns a typed result.
- Logs are bounded and do not automatically print full argv.
- Destructor/recovery paths cannot leak child processes or descriptors.

**Verification:**

- Test success, non-zero exit, timeout, cancellation, spaces/metacharacters remaining literal, output truncation and no zombie process.
- Add target/aggregate test `unit_tier_transcode` / `unit.tier_transcode_all` in `tests/unit/CMakeLists.txt`, linking `S3MediaKit::ManagerKit`.

**Phase completion criteria:** Process runner passes without ffmpeg installed and has no string-command API.

### Step 2.2 — Implement transformer interface and capability registry

**Objective:** Probe, plan, transcode and validate one local MP4 independently of TierStorageManager.

**Files:**

- `manager/Local/TierMediaTransformer.h`
- `manager/Local/TierMediaTransformer.cpp`
- `tests/unit/manager/test_tier_media_transformer.cpp`
- `tests/integration/tier_storage/test_tier_media_transformer.cpp`
- `tests/integration/tier_storage/CMakeLists.txt`

**Symbols:**

- `ITierMediaTransformer`.
- `FfmpegTierMediaTransformer`.
- `TierMediaDescriptor`, `TierTransformDecision`, `TierTransformResult`, `TierTranscodeCapabilities`.
- `TierEncoderCapabilityRegistry::{probe, snapshot, operational}`.
- `TierMediaTransformer::{inspect,plan,transform,validate}` on the concrete implementation.

**Change:**

- Parse `ffprobe` JSON; do not parse human-formatted output.
- Build argv internally for first video plus optional first audio; audio is `copy`, subtitle/data omitted.
- Scale preserves aspect ratio/even dimensions; FPS filter is only added when source exceeds cap.
- Candidate order follows config; each capability uses a short lavfi smoke encode.
- Runtime hardware failure retries next candidate; all candidates failing returns a normalized `TIER_TRANSCODE_*` error.
- Unique staging path is `{staging_root}/{job_id}/{stream_hash}/{segment_hash}.mp4.tmp`; all path components derived from identifiers are sanitized/hashed.
- Test implementation accepts an injected `BlockingProcessRunner` interface/fake; production uses the real runner.

**Verification:**

- Unit tests cover decision/no-op, scale/FPS/bitrate argv, codec selection, backend fallback, invalid probe/output and cancellation.
- New `integration_tier_transcode_ffmpeg` target generates a short lavfi MP4 and validates real H.264/H.265 output; individual codec cases skip when the installed binary lacks that codec.
- Register discovered cases with prefix `integration.tier-transcode.` and aggregate test `integration.tier_transcode_ffmpeg_all`.
- Rework `tests/integration/tier_storage/CMakeLists.txt` so this local ffmpeg target requires only `ENABLE_TIER_STORAGE`; retain AWS guard for MinIO targets.

**Phase completion criteria:** Transformer can independently produce and validate capped output, while capability failure is observable and non-destructive.

## Phase 3 — Durable journal, atomic finalize and TimeFile size consistency

### Step 3.1 — Implement journal repositories and transitions

**Objective:** Cung cấp durable primitives cho per-segment recovery, retry và cleanup.

**Files:**

- `manager/Storage/TierQualityJob.h`
- `manager/Storage/TieringJob.h`
- `tests/unit/storage/test_tier_txn_lock.cpp`
- `tests/unit/storage/test_storage_models.cpp`

**Symbols:**

- `TieringJobSegmentRepository::{insertIfAbsent,findByJob,transitionStatusAt,recoverTransformingAt,resetFailedForRetryAt,findStoredForCleanup,pruneByJobIds}`.
- Corresponding timestamp/default decisions in `TieringJobSegmentImp`.
- `TierTimefileSizeUpdateRepository::{upsertWithTxn,findPending,findByRangeId,findByIdentities,markResultAt,pruneDoneBefore}` and facade Imp.
- `TierQualityTransferRepository::finalizeTransfer()` for the cross-table transaction.

**Change:**

- Repository methods use parameterized queries only and receive ready-to-persist state/timestamps.
- Imp validates allowed transitions and supplies current time.
- `finalizeTransfer()` begins one write transaction, compare-and-set updates the range snapshot, marks child rows `COMMITTED`, inserts outbox rows and sets parent totals/status `DONE`.
- Outbox upsert is keyed by segment identity. A later move atomically replaces the previous job/size and resets status to `PENDING`, preventing an older patch from winning.
- On any failure, rollback all DB changes and leave child targets `STORED` for retry.
- `retryTieringJob()` resets only child `FAILED` rows; valid `STORED` rows remain reusable.
- Parent prune first removes/cleans non-committed target ownership, then deletes child/outbox rows and parent in one explicit maintenance sequence.

**Verification:**

- Repository transition tests include invalid status, duplicate insert and restart recovery.
- `TierTxnLockTest` adds a concurrent-writer case for `finalizeTransfer()` to prove `beginWriteTxn()` waits instead of failing with database locked.
- Atomicity test injects failure after each DB sub-step and confirms range/job/outbox never become partially committed.

**Phase completion criteria:** All persistence and state transitions work without media I/O; atomic finalize is proven under concurrent writer contention.

### Step 3.2 — Add TimeFile size patch and overlay

**Objective:** Đồng bộ `TimeBlock.file_size` với transformed output mà không phá active writer/swap semantics.

**Files:**

- `manager/Local/TimeRebuilder.h`
- `manager/Local/TimeRebuilder.cpp`
- `manager/Local/StorageManager.h`
- `manager/Local/StorageManager.cpp`
- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `tests/unit/manager/test_time_file.cpp`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`

**Symbols:**

- Add `TimeBlockSizePatch`.
- Add `TimeRebuilder::patchTimeBlocks()` and `MultiTimeRebuilder::patchTimeBlocks()`.
- Add `StorageManager::patchCameraTimeBlockSizes()`.
- Add `TierStorageManager::processTimefileSizeUpdates()`.
- Extend `TierStorageManager::collectSegmentFilesForRange()` with outbox overlay.

**Change:**

- Match exact identity using camera, stream, start time and decoded segment suffix.
- Rebuild preserves every TimeBlock field except `file_size` and reuses current capture/dirty-version/atomic-swap path.
- Group pending patches by camera and timefile/day; schedule at most one rewrite per file per cycle.
- Outbox stays `PENDING` until a later read verifies the new size; swap conflict simply retries.
- Overlay pending `new_size` before range calculations, stale-snapshot comparison, quota and restore planning.

**Verification:**

- Unit test patches one block without changing other fields/blocks.
- Test active writer conflict/retry and idempotent second patch.
- Integration test confirms range split/quota uses outbox size before TimeFile swap and TimeFile eventually matches.

**Phase completion criteria:** Range and TimeFile sizes cannot diverge observably after finalize, including during asynchronous swap.

## Phase 4 — Admission, protected shaping and dispatcher threading

### Step 4.1 — Policy validation and capability exposure

**Objective:** FE có contract đầy đủ và server fail-closed trước khi queue transform job.

**Files:**

- `manager/Local/TierStorageBase.h`
- `manager/Local/TierStorageBase.cpp`
- `server/WebApiStorage.cpp`
- `tests/unit/server/test_storage_pool_api.cpp`

**Symbols:**

- Extend `TierStorageBase::validatePolicyTiers()` and `validateStoragePolicy()`.
- Add `TierStorageBase::qualityReductionOptionsToJson()`.
- Extend `storagePoolOptions()`.
- Existing `policyFromJson()`/`policyDetailToJson()` continue delegating nested parsing to model helpers.

**Change:**

- Reject enabled HOT profile, invalid codec, negative values, half-configured resolution and enabled profile with no positive cap.
- Reject enabling when operational switch/capability for requested codec is unavailable.
- Return `qualityReduction` capability without command paths or raw driver errors.
- Continue returning `CODE_INVALID_ARGS` for bad policy payload.

**Verification:**

- API/model unit tests cover valid H.264/H.265, every invalid shape and disabled/PROBING/UNAVAILABLE capability.
- Existing pool option fields remain byte-for-byte structurally present.

**Phase completion criteria:** FE can safely decide whether to expose/enable controls, and invalid policy never persists.

### Step 4.2 — Split protected pieces and snapshot job operation/profile

**Objective:** Tạo đúng `COPY_MOVE`/`TRANSCODE_MOVE` cho mọi trigger mà không thay đổi delete protection semantics.

**Files:**

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`
- `tests/integration/tier_storage/CMakeLists.txt`

**Symbols:**

- Add `TierStorageManager::resolveTierQualityProfile()`.
- Add `TierStorageManager::carveMovePiecesByProtection()` using existing `shapeRangePieces()`.
- Extend `queueTierMoveJob()` parameters/logic with operation/profile snapshot.
- Update callers in `processCameraTiering()`, `processCameraPressureTiering()`, `planBalancedPressureMoves()` and external delete-policy move path.

**Change:**

- Protected/evidence records from `ProtectedVideoImp::findOverlapping()` become separate `COPY_MOVE` ranges.
- Non-protected piece becomes `TRANSCODE_MOVE` only when master, destination profile and operational feature are enabled.
- External target resolves profile by exact enabled tier `pool_id`; no match means `COPY_MOVE`.
- Snapshot uses canonical JSON and stable fingerprint; later profile edits do not mutate queued job.
- `profile_fingerprint` là SHA-256 hex của compact JSON có thứ tự field cố định như `TierQualityProfile::toJson()`; không hash raw request JSON.
- Existing policy route/retention validation still decides whether a pending job is stale.

**Verification:**

- Add integration cases to CMake list:
  - `QueuesTranscodeForRetentionMove`
  - `QueuesTranscodeForPressureMove`
  - `QueuesTranscodeForExternalMoveWithMappedTier`
  - `CopiesExternalMoveWithoutMappedProfile`
  - `SplitsProtectedRangeIntoCopyAndTranscodeJobs`
  - `QueuedJobKeepsQualityProfileSnapshot`
- All existing protected deletion tests remain unchanged.

**Phase completion criteria:** Mọi trigger tạo operation đúng; protected data không bao giờ đi vào transformer.

### Step 4.3 — Move transfer dispatch off pollers

**Objective:** Sửa thread boundary trước khi thêm workload encode dài.

**Files:**

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `tests/unit/manager/test_blocking_executors.cpp`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`

**Symbols:**

- `TierStorageManager::dispatchPendingTieringJobs()`.
- `TransferDispatchState` and `TierStorageManager::{start,stop}`.

**Change:**

- Replace transfer `WorkThreadPool::getPoller()->async` with `BlockingExecutors::tiering()->async`.
- Keep claim/per-pool counters under current mutex, release before I/O.
- Add `active_transcodes` to `TransferDispatchState`; increment/decrement cùng critical section với `active_total`, và không claim transform job khi đạt `storage.tier_transcode_max_concurrent`.
- Maintain `callbacks_in_flight`; `stop()` sets cancellation, stops claiming and waits without holding manager/cache locks.
- Capability probe may start on tiering executor but must not consume a transfer slot.
- Do not move periodic scan wholesale in this phase; only actual transfer callbacks leave poller. Any discovered remaining blocking maintenance call is logged as separate follow-up unless required by this feature.

**Verification:**

- Integration test records worker thread name and confirms it is not an EventPoller.
- Shutdown test cancels a blocked fake transform and returns with job recoverable as `PENDING`.
- Existing transfer concurrency and per-pool limit tests remain green.

**Phase completion criteria:** No tier transfer, ffprobe or ffmpeg wait runs on EventPoller; shutdown leaves no callback or child process alive.

## Phase 5 — End-to-end transform, store, finalize and recovery

### Step 5.1 — Integrate transformer into job execution

**Objective:** Thực thi durable transform pipeline cho filesystem targets.

**Files:**

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `manager/Local/TierFileStorageBase.h`
- `manager/Local/TierFileStorageBase.cpp`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`

**Symbols:**

- Add private `TierStorageManager::executeTranscodeMove()` and `executeTranscodeSegment()`.
- Refactor `executePendingJob()` to branch by `job.operation`.
- Refactor `commitTierTransfer()` into legacy copy commit and quality `TierQualityTransferRepository::finalizeTransfer()`.
- Add `TierFileStorageBase::uploadSegmentAtomic()`.
- Add transformer dependency member; singleton constructs `FfmpegTierMediaTransformer`, tests inject `ITierMediaTransformer` through a friend test-access factory/private constructor overload.

**Change:**

- Materialize segment journal exactly once from verified range snapshot.
- Process segment sequentially; `COPY_POLICY_NOOP` stores source, `TRANSCODE` stores validated staging artifact.
- Local/NAS write to same-filesystem `.part-{job_id}` then rename final.
- Persist `STORED` only after final target is present and actual size known.
- Finalize all children/range/outbox/parent in one DB transaction; delete source only afterward.
- Preserve current `COPY_MOVE` functions and missing-source semantics.

**Verification:**

- Fake transformer integration cases:
  - H.264/H.265 transform result updates range/output counters.
  - Policy no-op copies without invoking encode.
  - Transform/store/finalize failure keeps source.
  - Stale range snapshot rolls back target.
  - Source deletion failure leaves cleanup backlog but job `DONE`.

**Phase completion criteria:** HOT→WARM local/NAS transform is atomic from catalog perspective and recoverable per segment.

### Step 5.2 — Add MinIO target validation and recovery

**Objective:** Hoàn thiện COLD MinIO path với cùng state machine.

**Files:**

- `manager/Local/TierObjectStorage.h`
- `manager/Local/TierObjectStorage.cpp`
- `manager/Local/TierStorageManager.cpp`
- `tests/integration/tier_storage/test_tier_object_storage.cpp`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`

**Symbols:**

- Add `TierObjectHeadInfo` and `TierObjectStorage::headSegmentInfo()` returning presence and content length.
- Extend `executeTranscodeMove()` target adapter branch.

**Change:**

- Upload validated staging file to original S3 key.
- Reuse a target only for a durable `STORED` row with matching profile fingerprint and HeadObject content length.
- A target existing without matching durable ownership is overwritten by the current attempt, never assumed valid.
- On stale finalize/final failure, delete only target keys owned by the uncommitted job.

**Verification:**

- MinIO tests cover content length, upload/reuse, mismatched target replacement, failure rollback and restart from `STORED`.
- Existing list/head distinction (404 vs unknown) remains green.

**Phase completion criteria:** WARM→COLD MinIO has the same idempotency and source-safety guarantees as local/NAS.

### Step 5.3 — Recovery, retry, cancel and cleanup

**Objective:** Đóng toàn bộ lifecycle sau crash hoặc operator action.

**Files:**

- `manager/Local/TierStorageManager.h`
- `manager/Local/TierStorageManager.cpp`
- `manager/Storage/TierQualityJob.h`
- `tests/integration/tier_storage/test_tier_storage_manager.cpp`

**Symbols:**

- Extend `TierStorageManager::{start,retryTieringJob,cancelTieringJob,pruneOldMetrics,cleanupOrphanTemporarySegments}`.
- Add `recoverQualityTieringWork()`, `cleanupCommittedSources()` and `cleanupUncommittedQualityTargets()`.

**Change:**

- Startup resets `TRANSFORMING` to `PENDING`; validates `STORED` targets before reuse.
- Retry resets child `FAILED`, preserves valid `STORED` and requeues parent.
- Cancel is allowed only before active work; cleanup owned staging/targets before terminal prune.
- `COMMITTED` source cleanup retries until source absent or retention expires.
- Staging cleanup deletes only paths whose job is absent/terminal và file cũ hơn `2 * storage.tier_transcode_timeout_seconds`.
- Operational kill switch defers `TRANSCODE_MOVE` without changing it into copy.

**Verification:**

- Add paired process scenarios for crash at `TRANSFORMING`, `STORED`, post-finalize/pre-source-delete and pending TimeFile patch.
- Verify retry does not re-run transformer for valid `STORED` segment.
- Verify downgrade gate query can report all non-terminal transcode jobs.

**Phase completion criteria:** Mọi durable state có deterministic recovery/cleanup path; không có state buộc operator sửa DB thủ công.

## Phase 6 — REST output, observability and documentation

### Step 6.1 — Expose job progress and normalized errors

**Objective:** FE/operator quan sát được hành vi và mức tiết kiệm mà không lộ command/path nhạy cảm.

**Files:**

- `manager/Local/TierStorageBase.cpp`
- `manager/Local/TierStorageManager.cpp`
- `server/WebApiStorage.cpp`
- `tests/unit/server/test_storage_pool_api.cpp`
- `tests/unit/storage/test_storage_models.cpp`

**Symbols:**

- Extend `TierStorageBase::tieringJobToJson()`.
- Extend `TierStorageManager::getDashboardSummary()` and `getDashboardDetail()` with bounded aggregate counts.

**Change:**

- Return operation, input/output/saved bytes, saving percent, segment counters and encoder summary. `saved_bytes` và `saving_percent` clamp về `0` nếu output lớn hơn input.
- Map stored `TIER_TRANSCODE_*` prefix to stable `error_code`; retain current retryable behavior.
- Log job lifecycle at INFO, fallback/failure at WARN, detailed probe/argv only at redacted DEBUG.
- Do not use camera/job/segment identifiers as metric labels.

**Verification:**

- JSON unit tests cover zero input, output larger than input, mixed backend summary and error redaction.
- Existing job list/detail fields remain compatible.

**Phase completion criteria:** FE can render configuration, status and savings; secrets/raw argv are absent from API and INFO logs.

### Step 6.2 — Update operational documentation

**Objective:** Đồng bộ docs với code và rollout gates.

**Files:**

- `docs/api-storage-tiering.md`
- `docs/storage-tiering-flow.md`
- `docs/threading-blocking-work.md`
- `docs/README.md`
- `conf/config.ini`

**Change:**

- Document policy payload, capability response and job fields.
- Add state/recovery flow, dedicated tiering executor and every new config key.
- Document ffmpeg binary requirement, staging sizing/permissions, hardware fallback and codec skip behavior.
- Document “upgrade all nodes before enabling”, kill switch, drain query and no-backfill limitation.

**Verification:**

- Examples match actual serializer field names and defaults.
- Config table matches constants in `src/Common/config.*`.

**Phase completion criteria:** Operator can deploy, enable, monitor and rollback without reading source.

## Phase 7 — Final regression and release gates

### Step 7.1 — Complete integration matrix

**Objective:** Map toàn bộ acceptance requirements tới executable tests.

**Files:**

- `tests/integration/tier_storage/test_tier_storage_manager.cpp`
- `tests/integration/tier_storage/CMakeLists.txt`
- `tests/performance/run_tier_storage_metadata_benchmark.sh`

**Change:**

- Add scenario registration and CTest entries for local/NAS/MinIO, all triggers, protected split, failures and restart recovery.
- Extend metadata benchmark with representative active child/outbox rows and queries used by claim, overlay and prune paths.
- Dùng direct CMake/CTest commands bên dưới cho các target mới; giữ nguyên script MinIO hiện có và credential validation của nó.

**Verification:** Run the command set below.

**Phase completion criteria:** All functional, migration, threading, recovery, MinIO and performance gates pass; feature remains disabled by default in a fresh config.

## Interface Changes

### Public C++

- `PolicyTierConfig` gains `quality_reduction` (`TierQualityProfile`).
- `PolicyAdvancedRules` gains `enable_tier_quality_reduction`.
- `TieringJob` gains operation/profile/output/counter fields và `encoder_summary_json`.
- `BlockingExecutors` gains `tiering()` and `tieringThreadCount()`.
- `StorageManager` gains `patchCameraTimeBlockSizes()`.
- `TierObjectStorage` gains typed HeadObject metadata access.
- New internal interfaces: `ITierMediaTransformer`, `BlockingProcessRunner`, journal/outbox repositories.

No existing method is removed. New test injection remains private/friend-based and is not an external SDK API.

### REST

- Policy create/update/detail accepts/returns `advanced_rules.enable_tier_quality_reduction` and `tiers[].quality_reduction`.
- `/media/mserver/storage/pool/options` adds `qualityReduction`.
- Tiering job list/detail adds operation/output/savings/counters/encoder summary.
- No endpoint path or existing response field is removed.

### Configuration

- Adds eight `storage.tier_transcode_*` keys and `blocking.tieringThreads`.
- Operational kill switch defaults off.

### Network/protocol

- No streaming protocol, playback URL or object storage key change.

## Data / Schema Changes

- Old job row: implicitly becomes `COPY_MOVE`, `{}` profile/encoder summary, zero output/counters.
- New transcode job: parent stores immutable profile snapshot; child rows store per-segment progress and target ownership.
- Range `size_bytes` changes to actual target size at finalize.
- TimeFile outbox bridges DB commit and asynchronous atomic TimeFile rewrite.
- Child/outbox history is pruned with parent retention after cleanup; it is not a permanent per-segment playback catalog.
- No table sync registration because these are MediaServer-local operational tables, not ESC replicated entities.

## Concurrency Changes

- Transfer execution moves from `WorkThreadPool` EventPoller to `BlockingExecutors::tiering()`.
- Dispatcher retains global/per-pool admission counters; `active_transcodes` limits CPU/GPU admission without blocking a worker thread waiting for a semaphore.
- Capability snapshot is immutable between probes and swapped under a small mutex.
- Job processing is sequential per segment and parallel only across jobs.
- TimeFile patch stays on storage maintenance path and uses existing active-reader/writer swap coordination.
- Shutdown cancels process groups and waits for `callbacks_in_flight` without holding pool/cache locks.

## Compatibility

- Old policies and jobs preserve copy behavior.
- Feature-off and tier-storage-off builds remain valid.
- No new libav link requirement.
- Existing target keys and playback/restore resolution remain unchanged.
- Mixed old/new binaries are supported only while the feature remains disabled; enabling requires all nodes upgraded.
- Downgrade requires drain/cancel of non-terminal `TRANSCODE_MOVE`; migration remains installed.

## Test Plan

| Requirement | Test target/case |
|---|---|
| Policy round-trip/default/validation | `unit_storage`, `unit_storage_pool_api` |
| Migration and legacy job preservation | `unit_tier_quality_migration` |
| BEGIN IMMEDIATE atomic finalize | `unit_tier_txn_lock` |
| Dedicated non-poller executor | `unit_blocking_executors` |
| Process timeout/cancel/injection safety | `unit_tier_transcode` |
| Transformer decision/argv/fallback/validation | `unit_tier_transcode` |
| Real ffmpeg H.264/H.265 | `integration_tier_transcode_ffmpeg` |
| Retention/pressure/external admission | `integration_tier_storage_manager` |
| Protected split/preserve | `integration_tier_storage_manager` |
| Local/NAS success and failure safety | `integration_tier_storage_manager` |
| MinIO upload/reuse/rollback | `integration_tier_storage_manager`, `integration_tier_storage_minio` |
| Restart recovery at every state | `integration_tier_storage_manager` child scenarios |
| TimeFile patch/overlay | `unit_storage`, `integration_tier_storage_manager` |
| Metadata query/prune scale | `run_tier_storage_metadata_benchmark.sh` |
| Feature disabled regression | existing tier-storage suite plus tier-storage-off build |

## Verification Commands

Configure/build test tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_TIER_STORAGE=ON -DENABLE_AWS_SDK=ON
cmake --build build --target unit_storage unit_storage_pool_api unit_blocking_executors unit_tier_txn_lock unit_tier_transcode unit_tier_quality_migration integration_tier_transcode_ffmpeg integration_tier_storage_manager integration_tier_storage_minio -j2
```

Focused unit tests:

```bash
ctest --test-dir build --output-on-failure -R '^unit\.(storage_all|storage_pool_api_all|blocking_executors_all|tier_txn_lock_all|tier_transcode_all|tier_quality_migration_all)$'
```

Local tier/transcode integration:

```bash
ctest --test-dir build --output-on-failure -R '^integration\.(tier-transcode|tier-manager\.)'
```

MinIO suite, chỉ với endpoint test explicit:

```bash
TIER_TEST_S3_ENDPOINT=http://127.0.0.1:9000 \
TIER_TEST_S3_ACCESS_KEY=minioadmin \
TIER_TEST_S3_SECRET_KEY=minioadmin \
.codex/skills/tier-storage-testing/scripts/run_tier_storage_tests.sh
```

Build feature-off:

```bash
cmake -S . -B build-tier-off -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_TIER_STORAGE=OFF
cmake --build build-tier-off -j2
```

Metadata benchmark:

```bash
tests/performance/run_tier_storage_metadata_benchmark.sh
```

Final whitespace/diff review:

```bash
git diff --check
git diff --stat
```

## Risks

- **State explosion:** child/outbox rows can grow nhanh; mitigate bằng indexes theo actual query và prune cùng parent sau cleanup.
- **Incorrect target reuse:** chỉ reuse `STORED` có fingerprint/size khớp; target existence đơn thuần không đủ.
- **TimeFile race:** mọi patch dùng existing capture/dirty-version/atomic swap, outbox chỉ DONE sau verification.
- **Thread starvation:** tiering executor tách khỏi maintenance/API/snapshot; encode có admission quota riêng.
- **Shutdown hang:** runner phải kill/reap process group và dispatch callback luôn decrement counters bằng RAII.
- **Migration drift:** update entity macro/schema test cùng migration trong Phase 1; không merge runtime phase trước migration/model phase.
- **Hardware false positive:** smoke-test capability và runtime fallback; không tin danh sách encoder.
- **Partial target leaks:** failed/cancelled job cleanup target ownership trước prune.
- **Rolling upgrade:** feature flag giữ tắt cho đến khi toàn bộ node chạy version mới.
- **Audio incompatibility:** audio stream-copy validation fails closed; audio re-encode không được tự động thêm trong v1.

## Out of Scope

- Backfill/re-transcode dữ liệu đã nằm ở WARM/COLD.
- Giảm bitrate hoặc đổi codec audio.
- Subtitle/data stream preservation.
- User-selected encoder backend hoặc raw ffmpeg parameters.
- VAAPI/Apple VideoToolbox/AMD hardware encoding.
- Distributed transcode service hoặc GPU scheduler liên node.
- Native libavformat file pipeline.
- Thay đổi playback URL, storage key hoặc restore protocol.

## Exit Criteria

- Mọi file/symbol và thứ tự dependency đã được xác định; không còn quyết định kiến trúc cho implementer.
- Migration 18 bảo toàn legacy data và schema/entity tests pass.
- Policy/API/config mới mặc định tắt và backward-compatible.
- Không có disk/S3/SQLite/ffmpeg wait của transfer chạy trên EventPoller.
- Protected/evidence video chỉ đi qua `COPY_MOVE`.
- Transform failure không xóa source và không fallback copy.
- Finalize range/job/children/outbox là một write transaction.
- Restart/retry không encode lại target `STORED` hợp lệ.
- Range/TimeFile/quota phản ánh output size thực tế.
- Local/NAS/MinIO, H.264/H.265 và mọi trigger được test.
- Feature-off build, focused unit suite, tier manager suite, MinIO suite và metadata benchmark đều pass.
- Docs/API/config examples khớp code và rollout/rollback procedure được xác nhận.
