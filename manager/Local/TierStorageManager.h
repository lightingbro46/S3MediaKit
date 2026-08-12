#ifdef ENABLE_TIER_STORAGE

#ifndef LOCAL_TIERSTORAGEMANAGER_H
#define LOCAL_TIERSTORAGEMANAGER_H

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
#include "TierObjectStorage.h"
#include "TierLocalDiskStorage.h"
#include "TierNASStorage.h"
#include "Server/GlobalMonitor.h"

namespace managerkit {

struct ColdAccessRestoreResult {
    bool handled = false;
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

    // Test connectivity to a pool (filesystem stat for LOCAL_DISK/NAS; HEAD request for object storage)
    bool testPoolConnection(const StoragePool &pool, std::string &out_message, int &out_latency_ms);

    // Get available mount point for tier
    std::vector<DiskPartition> getAvailableMountPoints(const std::string &include_types = "");

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

    // Timeline: returns segment ranges decorated with tier and status info.
    // Merges DB-tracked SegmentTierRecord with the raw file-system timeline.
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

    // Pool health check + metrics snapshot
    void checkAndRecordPoolHealth();

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

    PlaybackPathResolveResult resolvePlaybackSegmentPath(const std::string &camera_id,
                                                         const std::string &stream_id,
                                                         int64_t segment_start_time,
                                                         const std::string &timefile_path);

    std::string getRestoreSegmentPath(const std::string &camera_id,
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
                          bool pressure);

    bool splitRangeForTieringThreshold(const SegmentTierRange &range,
                                       int64_t move_threshold,
                                       SegmentTierRange &out_move_range);

    bool expireRangeBestEffort(const SegmentTierRange &range,
                               const std::string &final_status = "EXPIRED");

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

    bool executeLocalTierMove(TieringJob &job,
                               const StoragePool &src_pool,
                               const StoragePool &dst_pool);

    bool executeObjectStorageUpload(TieringJob &job,
                                     const StoragePool &dst_pool);

    void executeRestoreSegment(const std::string &job_id,
                               const std::string &camera_id,
                               const std::string &stream_id,
                               const std::string &segment_path,
                               const StoragePool &source_pool,
                               int64_t range_start,
                               int64_t range_end);

    // Pool health helpers
    StorageHealth computePoolHealth(const StoragePool &pool,
                                    float usage_pct) const;

    void fillPoolRuntimeStats(StoragePool &pool);

    // Maintenance
    void pruneOldMetrics();
    void cleanupRestoreTempFiles();

    // Historical bootstrap from timefile blocks to compact SegmentTierRange.
    // This lets TierStorageManager take over media cleanup for old recordings
    // created before tier ranges existed.
    void backfillHotRangesFromTimeFiles();

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

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr       _timer;
    toolkit::Ticker           _ticker;

    mutable std::mutex        _pool_cache_mtx;
    // In-memory cache of enabled pools (refreshed every tick)
    std::unordered_map<std::string, StoragePool> _pool_cache;

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
};

} // namespace managerkit

#endif // LOCAL_TIERSTORAGEMANAGER_H

#endif // ENABLE_TIER_STORAGE
