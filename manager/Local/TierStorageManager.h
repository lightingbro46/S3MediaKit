#ifdef ENABLE_TIER_STORAGE

#ifndef LOCAL_TIERSTORAGEMANAGER_H
#define LOCAL_TIERSTORAGEMANAGER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"
#include "StorageTier.h"
#include "Storage/StoragePool.h"
#include "Storage/StoragePolicy.h"
#include "Storage/StorageTierExtra.h"
#include "Storage/PolicyAssignment.h"
#include "Storage/TieringJob.h"
#include "Storage/TierBackfillTask.h"
#include "TierObjectStorage.h"
#include "ObjectReadSource.h"
#include "TierLocalDiskStorage.h"
#include "TierNASStorage.h"
#include "Server/GlobalMonitor.h"

namespace managerkit {

struct ColdAccessRestoreResult {
    bool handled = false;
    bool job_created = false;
    std::string job_id;
    std::string status;
    std::string message;
    int estimated_restore_seconds = 120;
};

struct PlaybackPathResolveResult {
    std::string read_path;
    std::string tier;
    std::string pool_id;
    std::string range_id;
    bool restore_required = false;
    bool ready = false;
    bool fallback = false;
    bool storage_unavailable = false;
    std::string pool_health_status;
    std::string restore_job_id;
    std::string restore_status;
    std::string message;
};

struct TierRangeSegmentFile {
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    std::string storage_key;
    std::string full_path;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t file_size = 0;
    // Record encryption: which tier/pool the file sits on, which pool wrote it
    // (the encryption key is derived per origin pool) and whether it is encrypted.
    std::string tier;
    std::string pool_id;
    std::string origin_pool_id;
    bool encrypted = false;
};

// A filesystem mount exposed as a storage-pool candidate. mount_path is the
// logical recorder root persisted in StoragePool; actual_mount_path is the
// mount visible to the process, and data_path is where tier segment data is
// stored after applying Record::kAppName.
struct StorageMountCandidate {
    std::string device;
    std::string mount_path;
    std::string actual_mount_path;
    std::string data_path;
    std::string filesystem_type;
    std::string pool_type;
    std::string layout;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t used_bytes = 0;
    float usage_pct = 0.0f;

    Json::Value toJson() const;

    static StorageMountCandidate fromDiskPartition(const DiskPartition &partition,
                                                   const std::string &pool_type,
                                                   const std::string &app_name);
};

// ===================================================================
// TierStorageManager
//
// Runs independently alongside StorageManager and is responsible for:
//   1. CRUD of storage pools and policies (persisted in SQLite)
//   2. Camera-level policy assignment
//   3. Periodic tiering cycle: move segments between HOT → WARM → COLD
//      based on the effective policy for each camera
//   4. Pool health monitoring and metrics collection (for dashboard)
// ===================================================================
class TierStorageManager : public std::enable_shared_from_this<TierStorageManager> {
public:
    using Ptr = std::shared_ptr<TierStorageManager>;

    static TierStorageManager &Instance();
    ~TierStorageManager();

    void start();
    void stop();
    // Process exit: stop() and shut the object storage SDK down. Called explicitly on shutdown
    // (also after a failed startup) so none of it runs during static destruction.
    void shutdown();

    // ------------------------------------------------------------------
    // Section 1 — Storage Pool APIs
    // ------------------------------------------------------------------

    // Create a new pool; returns the assigned id on success (empty on failure)
    std::string createPool(StoragePool pool);

    // Update mutable fields of an existing pool
    bool updatePool(const StoragePool &pool);

    // Delete a pool; fails (returns false) if any policy still references it
    bool deletePool(const std::string &pool_id, int &out_ref_count, bool &out_is_default_hot_pool);

    // List pools with optional filters
    std::vector<StoragePool> listPools(const std::string &tier    = "",
                                       const std::string &type    = "",
                                       const std::string &keyword = "");

    // Get a single pool by id (returns nullptr / empty on not-found)
    std::vector<StoragePool> getPool(const std::string &pool_id);

    // Test the exact supplied pool snapshot without changing the registered
    // client/path or persisted pool state.
    bool testPoolConnection(const StoragePool &pool, std::string &out_message, int &out_latency_ms);

    // Get available filesystem roots. Returned mount_path is the logical pool
    // root and never includes the configured Record::kAppName suffix.
    std::vector<StorageMountCandidate> getAvailableMountPoints(const std::string &include_types = "");

    // Filesystem pools persist the logical recorder root. Tier storage reads
    // and writes below the configured record application directory.
    static std::string resolveFilePoolMountRoot(const StoragePool &pool);
    static std::string resolveFilePoolDataRoot(const StoragePool &pool,
                                               const std::string &app_name);

    // ------------------------------------------------------------------
    // Section 2 — Storage Policy APIs
    // ------------------------------------------------------------------

    std::string createPolicy(StoragePolicy policy);

    bool updatePolicy(const StoragePolicy &policy);

    // Delete a policy; fails if any camera assignment references it
    bool deletePolicy(const std::string &policy_id, int &out_camera_count, bool &out_is_default_policy);

    // Paginated list
    std::vector<StoragePolicy> listPolicies(const std::string &keyword = "",
                                             int enabled_filter = -1,
                                             int page = 0, int size = 20);

    int countPolicies(const std::string &keyword = "", int enabled_filter = -1);

    std::vector<StoragePolicy> getPolicy(const std::string &policy_id);

    // Clone an existing policy under a new name; returns new id
    std::string clonePolicy(const std::string &policy_id, const std::string &new_name);

    // ------------------------------------------------------------------
    // Section 3 — Policy Assignment APIs (camera level only)
    // ------------------------------------------------------------------

    bool assignPolicyToCamera(const std::string &camera_id,
                              const std::string &policy_id,
                              const std::string &reason = "");

    // Batch assign — returns how many succeeded and which ids failed
    int assignPolicyToCameras(const std::vector<std::string> &camera_ids,
                               const std::string &policy_id,
                               std::vector<std::string> &out_failed);

    bool removeCameraOverride(const std::string &camera_id);

    // Batch remove camera overrides — returns how many succeeded and which ids failed
    int removeCamerasOverride(const std::vector<std::string> &camera_ids, std::vector<std::string> &out_failed);

    // Resolve the effective policy for a camera (camera > system_default)
    EffectivePolicyResult getEffectivePolicy(const std::string &camera_id);

    // ------------------------------------------------------------------
    // Section 4 — Camera Storage APIs
    // ------------------------------------------------------------------

    // Timeline: returns compact ranges decorated with tier and status info.
    std::vector<TimelineRange> getCameraTimeline(const std::string &camera_id,
                                                  int64_t start_time,
                                                  int64_t end_time);

    // Per-tier storage summary for a camera
    CameraStorageSummary getCameraStorageSummary(const std::string &camera_id);

    // Resolve the MP4 record root for a camera from its effective storage policy.
    std::string resolveCameraRecordRoot(const std::string &camera_id,
                                        EffectivePolicyResult *out_effective = nullptr);

    // ------------------------------------------------------------------
    // Tiering engine — called internally from the timer
    // ------------------------------------------------------------------

    // Public so it can be triggered on demand (e.g. from tests)
    void runTieringCycle();

    // Dispatch durable TimeFile-to-HOT metadata backfill work.
    void runBackfillCycle();

    // Pool health check + metrics snapshot
    void checkAndRecordPoolHealth();

    // Dispatch a lightweight pool-health cycle independently from tiering.
    void runPoolHealthCycle();

    // Latest in-memory pool health snapshot. Returns false for an unknown pool.
    bool getPoolHealth(const std::string &pool_id, PoolHealthInfo &out_health) const;

    // Health gate used by playback. Disabled health checks preserve legacy behavior.
    bool isPoolAvailableForPlayback(const StoragePool &pool,
                                    PoolHealthInfo *out_health = nullptr) const;

    // Dashboard: aggregate summary across all tiers/pools
    Json::Value getDashboardSummary();

    // Dashboard: summary plus pools, policies, configured cameras and jobs.
    Json::Value getDashboardDetail();

    // Tiering job queries (for API section 6)
    std::vector<TieringJob> listTieringJobs(const std::string &camera_id   = "",
                                             const std::string &status      = "",
                                             const std::string &source_tier = "",
                                             const std::string &target_tier = "",
                                             int64_t from_time = 0, int64_t to_time = 0,
                                             int page = 0, int size = 20);

    int countTieringJobs(const std::string &camera_id = "",
                          const std::string &status    = "",
                          int64_t from_time = 0, int64_t to_time = 0);

    std::vector<TieringJob> getTieringJob(const std::string &job_id);

    bool retryTieringJob(const std::string &job_id);

    bool cancelTieringJob(const std::string &job_id);

    ColdAccessRestoreResult requestRestoreSegment(const std::string &camera_id,
                                                   const std::string &stream_id,
                                                   int64_t segment_start_time,
                                                   const std::string &timefile_path,
                                                   const std::string &reason,
                                                   int64_t segment_size = 0);

    ColdAccessRestoreResult requestRestoreWindow(const std::string &camera_id,
                                                  const std::string &stream_id,
                                                  int64_t start_time,
                                                  int64_t end_time,
                                                  const std::string &reason);

    PlaybackPathResolveResult resolvePlaybackSegmentPath(const std::string &camera_id,
                                                         const std::string &stream_id,
                                                         int64_t segment_start_time,
                                                         const std::string &timefile_path);

    // ===================== Record encryption support =====================
    // Estimated encryption footprint of the recordings this pool holds: encrypted and
    // unencrypted bytes plus their share. Read from the key history of the pool that
    // wrote each range, so a segment awaiting encryption already counts as encrypted.
    void fillPoolEncryptionUsage(StoragePool &pool);

    // Wires TierEncryptionManager to this manager through EncryptionStorageBridge.
    // Must run before TierEncryptionManager::initialize().
    void registerEncryptionStorageBridge();

    // True while at least one pool has encryption enabled; keeps the ".enc" lookups
    // off the hot path when the feature is unused.
    void setEncryptionActive(bool active);

    // Returns the variant of `path` that is actually on disk (plaintext or ".enc").
    std::string resolveRecordFilePath(const std::string &path) const;

    // Pool that originally wrote this segment; its key is the one that decrypts it.
    std::string getSegmentOriginPoolId(const std::string &camera_id,
                                       const std::string &stream_id,
                                       int64_t segment_start_time);

    // Segment files whose data was first recorded on `origin_pool_id`.
    std::vector<TierRangeSegmentFile> listSegmentFilesByOriginPool(const std::string &origin_pool_id,
                                                                   int64_t start_time,
                                                                   int64_t end_time);

    // Live (non deleted/expired) segment count still keyed to `origin_pool_id`.
    // Returns -1 when it cannot be determined; callers gating a destructive action
    // must refuse rather than treat that as zero.
    int countSegmentsByOriginPool(const std::string &origin_pool_id);

    // Splits a recorder file path into the segment path used as storage key.
    bool deriveSegmentPathForFile(const std::string &camera_id,
                                  const std::string &stream_id,
                                  const std::string &full_path,
                                  std::string &out_segment_path);

    // Writes `new_local_file` back over the segment in its pool, dropping the stale
    // plaintext/encrypted counterpart so a pool never keeps both copies.
    bool replaceSegmentInPlace(const std::string &camera_id,
                               const std::string &stream_id,
                               const std::string &segment_path,
                               const std::string &new_local_file,
                               std::string *err);

    // True while another thread is rewriting this segment file.
    bool isSegmentFileBusy(const std::string &path) const;

    PlaybackPathResolveResult accessRecordedSegment(const std::string &camera_id,
                                                    const std::string &stream_id,
                                                    int64_t segment_start_time,
                                                    const std::string &timefile_path,
                                                    int64_t segment_size,
                                                    bool enqueue_restore,
                                                    const std::string &reason,
                                                    bool retain_restore_cache = false);

    ObjectReadSource resolveObjectReadSource(const ObjectReadRequest &request);

    ObjectReadPlan planObjectReadWindow(const std::string &camera_id,
                                        const std::string &stream_id,
                                        int64_t start_time,
                                        int64_t end_time,
                                        ObjectReadPurpose purpose);

    bool validateRestoreWindowCache(const RestoreJob &job,
                                    const std::vector<SegmentTierRange> &ranges,
                                    const std::string &stream_id,
                                    int64_t start_time,
                                    int64_t end_time,
                                    int64_t restore_ttl_seconds,
                                    int64_t &expires_at) const;

    bool getRestoreJobSnapshot(const std::string &job_id,
                               RestoreJobSnapshot &snapshot) const;

    bool tryRetainRestoreCache(const std::string &job_id);
    void releaseRestoreCache(const std::string &job_id);

    std::string getRestoreSegmentPath(const std::string &job_id,
                                      const std::string &camera_id,
                                      const std::string &stream_id,
                                      const std::string &segment_path) const;

    std::vector<RestoreJob> listRestoreJobs(const std::string &camera_id   = "",
                                             const std::string &status      = "",
                                             int64_t from_time = 0, int64_t to_time = 0,
                                             int page = 0, int size = 20);

    int countRestoreJobs(const std::string &camera_id = "",
                          const std::string &status    = "",
                          int64_t from_time = 0, int64_t to_time = 0);

    std::vector<RestoreJob> getRestoreJob(const std::string &job_id);

    bool approveExpiredRange(const std::string &range_id);

    bool extendRangeRetention(const std::string &range_id,
                              int64_t extend_until,
                              const std::string &reason);

    /**
     * Register a newly created HOT segment range (from record MP4) with the tiering engine.
     * This allows the tiering engine to track the range and move it to WARM/COLD later if needed.
     * @param camera_id The camera ID
     * @param stream_id The stream ID
     * @param file_path The full path to the MP4 file
     * @param start_time The start time of the segment range (epoch ms)
     * @param end_time The end time of the segment range (epoch ms)
     * @param file_size The size of the MP4 file in bytes
     * @return true if the range was successfully registered, false otherwise
     */
    bool registerHotSegmentRange(const std::string &camera_id,
                                 const std::string &stream_id,
                                 const std::string &file_path,
                                 int64_t start_time,
                                 int64_t end_time,
                                 int64_t file_size);

    std::string getSystemDefaultHotPoolId() const;

    std::string getSystemDefaultPolicyId() const;

private:
    TierStorageManager(const toolkit::EventPoller::Ptr &poller = nullptr);

    // Tiering helpers
    void processCameraTiering(const std::string &camera_id,
                               const StoragePolicy &policy);

    void processCameraPressureTiering(const std::string &camera_id,
                                      const StoragePolicy &policy);

    void enforceCameraArchiveRetention(const std::string &camera_id,
                                       const StoragePolicy &policy);

    void enforcePolicyDeleteRetention(const std::string &camera_id,
                                      const StoragePolicy &policy);

    // Old pieces (split at `cutoff`) of a camera's AVAILABLE ranges that are
    // due for age-based retention, oldest first and bounded per cycle.
    std::vector<SegmentTierRange> collectAgeRetentionPieces(const std::string &camera_id,
                                                            int64_t cutoff,
                                                            const std::string &exclude_pool_id);

    // Splits `range` so segments touching protected videos stay in their own
    // pieces, and returns the pieces that may be deleted (all of `range` when
    // nothing is protected). Returns false when the range could not be read
    // or changed concurrently.
    bool carveDeletablePieces(const SegmentTierRange &range,
                              std::vector<SegmentTierRange> &out);

    // True when a filesystem pool passes the health gate and its data root
    // exists, so an absent segment file can be trusted as really missing.
    bool isFilePoolVerifiablyReadable(const StoragePool &pool) const;

    // Drops segments older than `verify_before` whose MP4 is missing from the
    // range's filesystem pool: removes their TimeBlocks and shrinks/splits the
    // range (MISSING when nothing is left). Returns false if it could not check.
    bool reconcileRangeFiles(const SegmentTierRange &range, int64_t verify_before);

    // Time-budgeted, newest-first sweep of AVAILABLE ranges calling
    // reconcileRangeFiles(); resumes from an in-memory cursor each cycle.
    void runRangeFileReconcile();

    // Removes `missing` segments (kind 0 in `kinds`) from `range`: shrinks or
    // splits it, or marks it MISSING when nothing is left, then drops their
    // TimeBlocks and raises TIER_SEGMENT_MISSING. False if the range changed.
    bool dropMissingSegments(const SegmentTierRange &range,
                             const std::vector<TierRangeSegmentFile> &segments,
                             const std::vector<int> &kinds,
                             const std::vector<TierRangeSegmentFile> &missing,
                             const std::string &missing_from);

    // Object-storage counterpart of reconcileRangeFiles(): compares a range's
    // segments with ListObjectsV2 of its day prefix(es); a key absent from
    // the listing is missing only when HeadObject answers 404.
    enum class ObjectRangeCheck {
        SKIPPED,        // timefile unreadable; nothing to judge
        ERROR,          // listing/HEAD failed; retry later
        CONSISTENT,
        PARTIAL,        // some objects missing, at least one present
        ALL_MISSING
    };
    struct ObjectRangeFinding {
        SegmentTierRange range;
        std::vector<TierRangeSegmentFile> segments;
        std::vector<int> kinds;                    // 1 present (or too recent), 0 missing
        std::vector<TierRangeSegmentFile> missing;
        std::string first_missing_key;
    };
    // Per object pool sweep state; maintenance-cycle only.
    struct ObjectReconcilePoolState {
        std::string fingerprint;                   // endpoint|bucket|base_path
        int64_t cursor_end = std::numeric_limits<int64_t>::max();
        std::string cursor_id;
        bool seen_present = false;                 // current sweep saw a present object
        // Ranges with no object at all wait here until the pool proves it is
        // sane (a present object); N of them in a row trip the mass-loss guard.
        std::vector<ObjectRangeFinding> pending_all_missing;
        int64_t paused_until = 0;
        // Orphan-object report: prefixes left to visit with their depth
        // below base_path (0 lists cameras, 3 is a day of one stream).
        std::deque<std::pair<std::string, int>> orphan_queue;
        bool orphan_active = false;
        int64_t orphan_next_sweep = 0;
        int64_t orphan_objects = 0;
        int64_t orphan_bytes = 0;
        std::string orphan_first;
    };
    ObjectRangeCheck checkObjectRange(const StoragePool &pool,
                                      const SegmentTierRange &range,
                                      int64_t verify_before,
                                      ObjectRangeFinding &out);
    ObjectRangeCheck reconcileObjectRange(const StoragePool &pool,
                                          ObjectReconcilePoolState &state,
                                          const SegmentTierRange &range,
                                          int64_t verify_before);
    void applyObjectFinding(const StoragePool &pool, const ObjectRangeFinding &finding);
    void flushPendingObjectFindings(const StoragePool &pool, ObjectReconcilePoolState &state);
    void tripObjectMassLossGuard(const StoragePool &pool, ObjectReconcilePoolState &state);
    // Priority ranges (see requestObjectRangeReconcile), then a time-budgeted,
    // newest-first sweep of each object pool's AVAILABLE ranges.
    void runObjectRangeReconcile();
    // Report-only sweep of camera/stream/day prefixes for objects that no
    // AVAILABLE/RESTORING/EXPIRED range of the pool owns; raises
    // POOL_ORPHAN_OBJECTS once per completed sweep and never deletes.
    void runObjectOrphanSweep(const StoragePool &pool,
                              ObjectReconcilePoolState &state,
                              const std::function<bool()> &exhausted);
    // False when listing failed; the day is then retried next cycle.
    bool scanOrphanDayPrefix(const StoragePool &pool,
                             const std::string &prefix,
                             const std::string &camera_id,
                             const std::string &stream_id,
                             const std::string &day,
                             ObjectReconcilePoolState &state);
    // Queues a range for object reconciliation at the start of the next
    // maintenance cycle, ahead of the sweep; any thread.
    void requestObjectRangeReconcile(const std::string &range_id);

    // Pressure deletion on every pool (HOT/WARM/COLD, filesystem or object
    // storage) above its critical watermark; stops once usage would drop
    // below (high - 5)%. Picks data per storage.tiering_pressure_delete_mode
    // (BALANCED or OLDEST). METRICS-mode object pools cool down between runs.
    void enforcePoolPressureRetention();

    // One camera's deletable window on a pool for balanced pressure deletion:
    // [lo, hi) where hi excludes the camera's keepArchivedMinFor.
    struct PressureCameraWindow {
        std::string camera_id;
        int64_t lo = 0;
        int64_t hi = 0;
    };

    // BALANCED pressure deletion: finds the largest keep percent kp (1%
    // steps) whose per-camera cutoffs hi - (hi - lo) * kp / 100 free `need`
    // bytes, then deletes up to those cutoffs round-robin across cameras.
    // Cameras without a recorder are reclaimed before the others.
    void reclaimPoolBalanced(const StoragePool &pool,
                             int64_t now,
                             int max_pieces,
                             int64_t &need,
                             int &deleted_count,
                             int64_t &deleted_bytes);

    static int64_t balancedCutoff(const PressureCameraWindow &window, int keep_percent);

    // Largest keep percent (0..99, 1% steps) whose per-camera cutoffs cover
    // `need` bytes of deletable (or, with `for_move`, movable) ranges on
    // `pool`; 0 when nothing covers it. `out_estimated` gets the bytes covered.
    int findBalancedKeepPercent(const StoragePool &pool,
                                const std::vector<PressureCameraWindow> &cameras,
                                int64_t now,
                                int64_t need,
                                bool for_move,
                                int64_t &out_estimated);

    // BALANCED early moves: for every pool above its high watermark, picks one
    // keep percent across the cameras whose policy moves data out of it early
    // and stores each camera's move cutoff for processCameraPressureTiering().
    void planBalancedPressureMoves(
        const std::vector<std::pair<std::string, StoragePolicy>> &camera_policies);

    // Returns the number of candidate ranges it examined.
    int reclaimCamerasBalanced(const StoragePool &pool,
                               const std::vector<PressureCameraWindow> &cameras,
                               int64_t now,
                               int max_pieces,
                               int64_t &need,
                               int &deleted_count,
                               int64_t &deleted_bytes);

    void ensureDefaultHotPool();

    void ensureSystemDefaultPolicy();

    bool queueTierMoveJob(const SegmentTierRange &range,
                          const PolicyTierConfig &src_tier_cfg,
                          const PolicyTierConfig &dst_tier_cfg,
                          const StoragePolicy &policy,
                          TieringJobTrigger trigger);

    struct DestinationPoolCheck {
        bool critical = false;
        bool has_capacity = false;
        float usage_pct = 0.0f;
    };

    // True when the destination pool is at or above its critical watermark.
    // Probes each pool at most once per maintenance cycle and raises a
    // rate-limited TIER_DEST_POOL_CRITICAL alert. Unknown capacity is not critical.
    bool isDestinationPoolCritical(const std::string &pool_id);

    // Same per-cycle probe; `has_capacity` is false when the pool is unknown,
    // disabled or reports no capacity (e.g. offline).
    DestinationPoolCheck destinationPoolCheck(const std::string &pool_id);

    enum class PendingJobPolicyDecision {
        VALID,
        STALE,
        DEFER
    };

    bool loadEffectiveStoragePolicy(const std::string &camera_id,
                                    StoragePolicy &out_policy,
                                    std::string &out_error);

    PendingJobPolicyDecision validatePendingJobAgainstPolicy(
        const TieringJob &job,
        const SegmentTierRange *range,
        const StoragePolicy &policy,
        int64_t now,
        std::string &out_reason) const;

    bool splitRangeForTieringThreshold(const SegmentTierRange &range,
                                       int64_t move_threshold,
                                       SegmentTierRange &out_move_range);

    bool expireRangeBestEffort(const SegmentTierRange &range,
                               const std::string &final_status = "DELETED");

    bool isRangeDeletionProtected(const SegmentTierRange &range) const;

    void processMetadataCleanupJobs();

    bool cleanupRangeMotionMetadata(const SegmentTierRange &range);

    bool cleanupRangeBookmarkMetadata(const SegmentTierRange &range);

    // Lists the segments a range owns. Returns false (with `error`) when the
    // timefile cannot be read completely; callers that change data must stop.
    bool collectSegmentFilesForRange(const SegmentTierRange &range,
                                     std::vector<TierRangeSegmentFile> &out,
                                     std::string &error);

    // Read-only convenience wrapper; yields an empty list on read errors.
    std::vector<TierRangeSegmentFile> collectSegmentFilesForRange(const SegmentTierRange &range);

    bool collectSegmentFilesForJob(const TieringJob &job,
                                   SegmentTierRange &out_range,
                                   std::vector<TierRangeSegmentFile> &out,
                                   std::string &error);

    void executePendingJob(TieringJob &job);

    // Dispatch durable PENDING jobs to a bounded transfer worker set.
    void dispatchPendingTieringJobs();

    bool executeLocalTierMove(TieringJob &job,
                               const StoragePool &src_pool,
                               const StoragePool &dst_pool);

    bool executeObjectStorageUpload(TieringJob &job,
                                     const StoragePool &src_pool,
                                     const StoragePool &dst_pool);

    // True once a range has exhausted its missing-segment retries and the
    // source pool is verifiably reachable, so absent files are real losses.
    bool canDropMissingSegments(const TieringJob &job,
                                const StoragePool &src_pool) const;

    // Applies a finished transfer to the range snapshot. Segments flagged in
    // `missing` are dropped from the range stats and from the timeline.
    bool commitTierTransfer(TieringJob &job,
                            const SegmentTierRange &range,
                            const std::vector<TierRangeSegmentFile> &segments,
                            const std::vector<char> &missing,
                            const std::string &target_tier,
                            const std::string &target_pool_id);

    void reportMissingSegments(const SegmentTierRange &range,
                               const std::vector<TierRangeSegmentFile> &missing,
                               const std::string &missing_from = "source and target");

    void executeRestoreSegment(const RestoreJobSegment &segment);

    void processRestoreJobs();

    void updateRestoreParent(const std::string &job_id);

    // Pool health helpers
    StorageHealth computePoolHealth(const StoragePool &pool,
                                    float usage_pct) const;

    bool isPoolAvailableForTierOperation(const StoragePool &pool,
                                         bool destination_write,
                                         PoolHealthInfo *out_health = nullptr) const;

    void fillPoolRuntimeStats(StoragePool &pool);

    // Maintenance
    void pruneOldMetrics();
    void cleanupRestoreTempFiles();
    void cleanupOrphanTemporarySegments();

    bool syncBackfillTasks();

    bool prepareBackfillChunk(const TierBackfillTask &task,
                              TierBackfillChunk &chunk,
                              std::string &error);

    bool processBackfillTask(const TierBackfillTask &task);

    bool resolveHotPoolForPath(const std::string &file_path,
                               StoragePool &out_pool,
                               std::string &out_pool_root) const;

    bool getPoolById(const std::string &pool_id,
                     StoragePool &out_pool) const;

    bool getPolicyHotPoolId(const StoragePolicy &policy,
                            std::string &out_pool_id) const;

    bool resolveHotPoolRecordRoot(const StoragePool &pool,
                                  std::string &out_record_root) const;

    std::string resolvePolicyRecordRoot(const StoragePolicy &policy) const;

    bool refreshCameraRecordRoot(const std::string &camera_id);

    void refreshCamerasRecordRootForPolicy(const std::string &policy_id);

    bool validatePolicyForExecution(const StoragePolicy &policy, std::string &error) const;

private:
    // Serializes rewrites of one segment file against readers of that file.
    void acquireSegmentLock(const std::string &path);
    void releaseSegmentLock(const std::string &path);

    class SegmentFileLock {
    public:
        SegmentFileLock(TierStorageManager *owner, const std::string &path);
        ~SegmentFileLock();
        SegmentFileLock(const SegmentFileLock &) = delete;
        SegmentFileLock &operator=(const SegmentFileLock &) = delete;

    private:
        TierStorageManager *_owner;
        std::string _key;
    };

    bool findRangeForSegment(const std::string &camera_id,
                             const std::string &stream_id,
                             int64_t segment_start_time,
                             SegmentTierRange &out_range);

    // Path resolution without the decryption step; resolvePlaybackSegmentPath() wraps it.
    PlaybackPathResolveResult resolvePlaybackSegmentPathRaw(const std::string &camera_id,
                                                            const std::string &stream_id,
                                                            int64_t segment_start_time,
                                                            const std::string &timefile_path);

private:
    struct PoolHealthRuntimeState {
        PoolHealthInfo info;
        int64_t last_metric_at = 0;
    };

    struct HealthCycleState {
        std::mutex mutex;
        std::condition_variable completed;
        bool running = false;
    };

    struct TransferDispatchState {
        std::mutex mutex;
        std::condition_variable completed;
        int active_total = 0;
        std::unordered_map<std::string, int> active_by_pool;
        // Transfer callbacks not yet finished. Unlike active_total it drops only
        // after the callback released the manager, so stop() never returns while
        // a work poller could still destroy the manager.
        int callbacks_in_flight = 0;
    };

    struct TemporaryMp4Observation {
        uint64_t device = 0;
        uint64_t inode = 0;
        int64_t size = 0;
        int64_t modified_at = 0;

        bool operator==(const TemporaryMp4Observation &other) const {
            return device == other.device &&
                   inode == other.inode &&
                   size == other.size &&
                   modified_at == other.modified_at;
        }
    };

    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr       _timer;
    toolkit::Timer::Ptr       _backfill_timer;
    toolkit::Timer::Ptr       _health_timer;
    toolkit::Ticker           _ticker;
    toolkit::Ticker           _backfill_sync_ticker;
    std::atomic<bool>         _maintenance_running{false};
    std::atomic<bool>         _backfill_running{false};
    std::atomic<bool>         _stopping{false};
    std::atomic<int>          _restore_inflight{0};
    std::atomic<bool>         _backfill_tasks_initialized{false};

    mutable std::mutex        _pool_cache_mtx;
    mutable std::mutex        _pool_health_mtx;
    mutable std::mutex        _range_registration_mtx;
    mutable std::mutex        _restore_request_mtx;
    std::unordered_map<std::string, size_t> _restore_cache_leases;
    mutable std::mutex        _restore_dispatch_mtx;
    mutable std::mutex        _temporary_mp4_observations_mtx;
    // In-memory cache of enabled pools (refreshed every tick)
    std::unordered_map<std::string, StoragePool> _pool_cache;
    std::unordered_map<std::string, PoolHealthRuntimeState> _pool_health_cache;
    std::shared_ptr<HealthCycleState> _health_cycle_state = std::make_shared<HealthCycleState>();
    std::shared_ptr<TransferDispatchState> _transfer_dispatch_state = std::make_shared<TransferDispatchState>();
    std::unordered_map<std::string, TemporaryMp4Observation> _temporary_mp4_observations;
    size_t _temporary_mp4_next_pool_index = 0;
    // Reconciliation sweep cursor (start_time, range_id), descending; maintenance-cycle only.
    int64_t _reconcile_cursor_start = std::numeric_limits<int64_t>::max();
    std::string _reconcile_cursor_id;
    std::unordered_map<std::string, ObjectReconcilePoolState> _object_reconcile;
    // Ranges whose restore found the object gone (HTTP 404); any thread.
    std::mutex _object_reconcile_priority_mtx;
    std::set<std::string> _object_reconcile_priority;
    size_t _object_reconcile_next_pool = 0;
    // Ranges already reported (REPORT mode, size mismatch); maintenance-cycle only.
    std::unordered_set<std::string> _object_reconcile_reported;
    // Last POOL_CRITICAL_NO_DELETABLE alert per pool; maintenance-cycle only.
    std::unordered_map<std::string, int64_t> _pressure_alerted_at;
    // Earliest next pressure deletion per METRICS-mode object pool; maintenance-cycle only.
    std::unordered_map<std::string, int64_t> _pressure_cooldown_until;
    // BALANCED early-move cutoffs of this cycle: source pool -> camera ->
    // move ranges starting before it; maintenance-cycle only.
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> _pressure_move_cutoffs;
    // Destination capacity probed during the current maintenance cycle and the
    // last TIER_DEST_POOL_CRITICAL alert per pool; maintenance-cycle only.
    std::unordered_map<std::string, DestinationPoolCheck> _dest_pool_checks;
    std::unordered_map<std::string, int64_t> _dest_critical_alerted_at;
    // Bucket versioning per object pool; touched by API, health and maintenance threads.
    struct PoolVersioningState {
        int64_t checked_at = 0;
        int64_t alerted_at = 0;
        std::string status;
    };
    mutable std::mutex _pool_versioning_mtx;
    std::unordered_map<std::string, PoolVersioningState> _pool_versioning;
    // QUOTA-mode usage per pool: {computed_at, bytes}; any thread.
    mutable std::mutex _quota_usage_mtx;
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> _quota_usage;
    size_t _temporary_mp4_next_camera_index = 0;

    // System default policy id (fixed built-in policy)
    mutable std::mutex _default_policy_mtx;
    std::string        _default_policy_id;

    // Object-storage client registry (one entry per MINIO/S3/ARCHIVE pool)
    TierObjectStorage  _obj_storage;

    // Filesystem registries split by backend semantics.
    TierLocalDiskStorage _local_storage;
    TierNASStorage       _nas_storage;

    // Decrypt pool secret_key_enc (currently identity — extend for real crypto)
    static std::string decryptPoolSecret(const std::string &encrypted);

    // Register / unregister a pool with _obj_storage based on its type
    bool registerPoolObjStorage(const StoragePool &pool);
    void unregisterPoolObjStorage(const std::string &pool_id);

    // Register / unregister a mount/network path pool with the matching file backend
    bool registerPoolFileStorage(const StoragePool &pool);
    void unregisterPoolFileStorage(const std::string &pool_id);
    TierFileStorageBase *fileStorageForPoolType(const std::string &type);
    const TierFileStorageBase *fileStorageForPoolType(const std::string &type) const;

    bool probePoolRuntimeStats(StoragePool &pool, std::string &out_message);

    // Reads the bucket versioning state of an object pool at most once an
    // hour (always when `force`) and raises POOL_VERSIONING_BLOCKS_RECLAIM
    // at most once a day while it is Enabled. Returns the last known state.
    std::string refreshPoolVersioning(const StoragePool &pool, bool force);

    // Summed bytes of the ranges stored on a QUOTA-mode pool, cached for
    // storage.tiering_quota_usage_cache_seconds.
    int64_t quotaUsedBytes(const std::string &pool_id);
    void mergePoolRuntimeStats(StoragePool &pool) const;

    // Segment files currently being rewritten (encrypt/decrypt/re-encrypt).
    mutable std::mutex _segment_busy_mtx;
    std::condition_variable _segment_busy_cv;
    std::unordered_set<std::string> _segment_busy;
};

} // namespace managerkit

#endif // LOCAL_TIERSTORAGEMANAGER_H

#endif // ENABLE_TIER_STORAGE
