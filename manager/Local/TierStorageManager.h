#ifdef ENABLE_TIER_STORAGE

#ifndef LOCAL_TIERSTORAGEMANAGER_H
#define LOCAL_TIERSTORAGEMANAGER_H

#include <atomic>
#include <condition_variable>
#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
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

    // Trigger async restore when a record MP4 access misses locally but the
    // segment is tracked in a cold tier range.
    ColdAccessRestoreResult handleColdAccessByPath(const std::string &file_path);

    ColdAccessRestoreResult requestRestoreSegment(const std::string &camera_id,
                                                   const std::string &stream_id,
                                                   int64_t segment_start_time,
                                                   const std::string &timefile_path,
                                                   const std::string &reason);

    ColdAccessRestoreResult requestRestoreWindow(const std::string &camera_id,
                                                  const std::string &stream_id,
                                                  int64_t start_time,
                                                  int64_t end_time,
                                                  const std::string &reason);

    PlaybackPathResolveResult resolvePlaybackSegmentPath(const std::string &camera_id,
                                                         const std::string &stream_id,
                                                         int64_t segment_start_time,
                                                         const std::string &timefile_path);

    bool validateRestoreWindowCache(const RestoreJob &job,
                                    const std::vector<SegmentTierRange> &ranges,
                                    const std::string &stream_id,
                                    int64_t start_time,
                                    int64_t end_time,
                                    int64_t restore_ttl_seconds,
                                    int64_t &expires_at) const;

    bool getRestoreJobSnapshot(const std::string &job_id,
                               RestoreJobSnapshot &snapshot) const;

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

    std::string getSystemDefaultHotPoolId() const;

    void ensureDefaultHotPool();

    void ensureSystemDefaultPolicy();

    std::string getSystemDefaultPolicyId() const;

    bool queueTierMoveJob(const SegmentTierRange &range,
                          const PolicyTierConfig &src_tier_cfg,
                          const PolicyTierConfig &dst_tier_cfg,
                          const StoragePolicy &policy,
                          TieringJobTrigger trigger);

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

    std::vector<TierRangeSegmentFile> collectSegmentFilesForRange(const SegmentTierRange &range);

    std::vector<TierRangeSegmentFile> collectSegmentFilesForWindow(const std::string &camera_id,
                                                                   int64_t start_time,
                                                                   int64_t end_time,
                                                                   const std::string &tier = "",
                                                                   const std::string &pool_id = "");

    std::vector<TierRangeSegmentFile> collectSegmentFilesForJob(const TieringJob &job,
                                                                SegmentTierRange &out_range);

    void notifyRebuildTimeFile(const std::string &camera_id,
                               uint64_t threshold);

    void executePendingJob(TieringJob &job);

    // Dispatch durable PENDING jobs to a bounded transfer worker set.
    void dispatchPendingTieringJobs();

    bool executeLocalTierMove(TieringJob &job,
                               const StoragePool &src_pool,
                               const StoragePool &dst_pool);

    bool executeObjectStorageUpload(TieringJob &job,
                                     const StoragePool &dst_pool);

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
    mutable std::mutex        _restore_dispatch_mtx;
    mutable std::mutex        _temporary_mp4_observations_mtx;
    // In-memory cache of enabled pools (refreshed every tick)
    std::unordered_map<std::string, StoragePool> _pool_cache;
    std::unordered_map<std::string, PoolHealthRuntimeState> _pool_health_cache;
    std::shared_ptr<HealthCycleState> _health_cycle_state = std::make_shared<HealthCycleState>();
    std::shared_ptr<TransferDispatchState> _transfer_dispatch_state = std::make_shared<TransferDispatchState>();
    std::unordered_map<std::string, TemporaryMp4Observation> _temporary_mp4_observations;
    size_t _temporary_mp4_next_pool_index = 0;
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
    void mergePoolRuntimeStats(StoragePool &pool) const;
};

} // namespace managerkit

#endif // LOCAL_TIERSTORAGEMANAGER_H

#endif // ENABLE_TIER_STORAGE
