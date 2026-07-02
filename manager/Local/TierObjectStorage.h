#ifndef LOCAL_TIER_OBJECT_STORAGE_H
#define LOCAL_TIER_OBJECT_STORAGE_H

/**
 * TierObjectStorage
 *
 * Manages one live S3/MinIO client per registered storage pool.
 * Owned (not singleton) by TierStorageManager so its lifetime matches the
 * manager's lifetime.
 *
 * Compiled with full AWS SDK logic when ENABLE_AWS_SDK is defined; a
 * no-op stub implementation is provided otherwise so the rest of the code
 * compiles unchanged.
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_AWS_SDK

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/BucketLocationConstraint.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/DeleteObjectsRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/ObjectIdentifier.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

#endif // ENABLE_AWS_SDK

namespace managerkit {

// ============================================================================
// PoolStats — capacity snapshot returned by getPoolStats()
// ============================================================================
struct TierPoolStats {
    bool     is_online   = false;
    uint64_t total_bytes = 0;
    uint64_t used_bytes  = 0;
    uint64_t free_bytes  = 0;
    // MinIO Prometheus I/O counters (0 if not available)
    uint64_t rx_bytes    = 0;
    uint64_t tx_bytes    = 0;
};

// ============================================================================
// TierObjectStorage
// ============================================================================
class TierObjectStorage {
public:
    TierObjectStorage();
    ~TierObjectStorage();

    // ------------------------------------------------------------------
    // Pool lifecycle
    // ------------------------------------------------------------------

    /**
     * Register an object-storage pool so that upload/download/delete
     * operations can be directed to it by pool_id.
     * Safe to call repeatedly for the same pool_id (updates credentials).
     */
    bool registerPool(const std::string &pool_id,
                      const std::string &type,          // MINIO | S3 | ARCHIVE
                      const std::string &endpoint,
                      const std::string &bucket,
                      const std::string &base_path,
                      const std::string &access_key,
                      const std::string &secret_key);

    /** Remove the client entry for pool_id. */
    void unregisterPool(const std::string &pool_id);

    bool isRegistered(const std::string &pool_id) const;

    // ------------------------------------------------------------------
    // Connection test
    // ------------------------------------------------------------------

    /**
     * Test connectivity for an already-registered pool.
     * Tries HeadBucket; falls back to ListBuckets when bucket does not exist
     * and creates it automatically.
     */
    bool testConnection(const std::string &pool_id, std::string &out_message, int &out_latency_ms);

    /**
     * Ad-hoc test (before a pool is saved).
     * Builds a temporary client and runs the same HeadBucket probe.
     */
    bool testConnectionParams(const std::string &endpoint,
                               const std::string &bucket,
                               const std::string &access_key,
                               const std::string &secret_key,
                               std::string &out_message,
                               int &out_latency_ms);

    // ------------------------------------------------------------------
    // Segment I/O
    // ------------------------------------------------------------------

    /**
     * Upload a local MP4 segment to object storage.
     * @param pool_id     Target pool (must be registered).
     * @param local_path  Absolute path to the source file.
     * @param s3_key      Destination key inside the bucket (relative to base_path).
     */
    bool uploadSegment(const std::string &pool_id,
                       const std::string &local_path,
                       const std::string &s3_key);

    /**
     * Download an object from S3 to a local path.
     * Parent directories are created automatically.
     */
    bool downloadSegment(const std::string &pool_id,
                          const std::string &s3_key,
                          const std::string &local_path);

    /**
     * Delete a single object from S3.
     */
    bool deleteSegment(const std::string &pool_id, const std::string &s3_key);

    /**
     * Delete multiple objects in one or more batched requests (max 1000/call).
     * Returns true when all deletes succeed.
     */
    bool deleteSegmentsBatch(const std::string &pool_id,
                              const std::vector<std::string> &s3_keys);

    /**
     * Check whether an object exists without downloading it.
     */
    bool segmentExists(const std::string &pool_id, const std::string &s3_key);

    // ------------------------------------------------------------------
    // Capacity / health stats
    // ------------------------------------------------------------------

    /**
     * Retrieve live capacity statistics for a registered pool.
     * For MinIO: scrapes Prometheus metrics.
     * For generic S3: uses HeadBucket (returns only is_online).
     */
    TierPoolStats getPoolStats(const std::string &pool_id);

    // ------------------------------------------------------------------
    // S3 key utilities
    // ------------------------------------------------------------------

    /**
     * Build the S3 key from the pool's base_path + segment path.
     * Example: base_path="traffic", camera="cam1", segment="2024-01-15/10-00-00"
     *          → "traffic/cam1/main/2024-01-15/10-00-00.mp4"
     */
    static std::string makeS3Key(const std::string &base_path,
                                  const std::string &camera_id,
                                  const std::string &stream_id,
                                  const std::string &segment_path);

private:
#ifdef ENABLE_AWS_SDK

    // One entry per registered pool
    struct PoolEntry {
        std::string pool_id;
        std::string endpoint;
        std::string bucket;
        std::string base_path;
        std::string access_key;
        std::shared_ptr<Aws::S3::S3Client> client;
    };

    std::shared_ptr<Aws::S3::S3Client>
    buildClient(const std::string &endpoint,
                const std::string &access_key,
                const std::string &secret_key) const;

    bool ensureBucketExists(const PoolEntry &entry);

    TierPoolStats scrapeMinioPrometheus(const PoolEntry &entry) const;

    Aws::SDKOptions _sdk_options;
    bool            _sdk_initialized = false;

#endif // ENABLE_AWS_SDK

    mutable std::mutex                                  _mtx;
    std::unordered_map<std::string, std::string>        _pool_secret_map; // pool_id → plaintext secret (runtime only)

#ifdef ENABLE_AWS_SDK
    std::unordered_map<std::string, PoolEntry>          _pool_map;
#endif
};

} // namespace managerkit

#endif // LOCAL_TIER_OBJECT_STORAGE_H
