#ifdef ENABLE_TIER_STORAGE

#ifndef LOCAL_TIERENCRYPTIONMANAGER_H
#define LOCAL_TIERENCRYPTIONMANAGER_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstdint>
#include <ctime>
#include <functional>

#include "Storage/RecordEncryptionConfig.h"
#include "Storage/RecordEncryptionTierRange.h"
#include "Record/Recorder.h"

#include "TierEncryptionUtils.h"

namespace managerkit {

struct EncryptionTimeRange {
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string salt_enc;
    std::string salt_hmac;
    std::string key_hash;
    RecordEncryptionMode mode = RecordEncryptionMode::kNone;

    EncryptionTimeRange() = default;
    EncryptionTimeRange(int64_t start, int64_t end,
                         const std::string& s_enc, const std::string& s_hmac,
                         const std::string& k_hash = "",
                         RecordEncryptionMode m = RecordEncryptionMode::kNone)
        : start_time(start), end_time(end), salt_enc(s_enc), salt_hmac(s_hmac),
          key_hash(k_hash), mode(m) {}

    static EncryptionTimeRange fromDbEntity(const RecordEncryptionTierRange& db) {
        EncryptionTimeRange r;
        r.start_time = db.start_time;
        r.end_time = db.end_time;
        r.salt_enc = db.salt_enc;
        r.salt_hmac = db.salt_hmac;
        r.key_hash = db.key_hash;
        r.mode = static_cast<RecordEncryptionMode>(db.mode);
        return r;
    }

    RecordEncryptionTierRange toDbEntity(const std::string& tier_id) const {
        RecordEncryptionTierRange db;
        db.tier_id = tier_id;
        db.start_time = start_time;
        db.end_time = end_time;
        db.salt_enc = salt_enc;
        db.salt_hmac = salt_hmac;
        db.key_hash = key_hash;
        db.mode = static_cast<int>(mode);
        return db;
    }
};

struct TierEncryptionConfig {
    std::string id;
    RecordEncryptionMode mode = RecordEncryptionMode::kNone;
    std::string verify_salt;
    std::string verify_hash;
    std::string salt_enc;
    std::string salt_hmac;
    bool encrypt_old_video = false;
    std::vector<EncryptionTimeRange> time_ranges;
    int64_t updated_at = 0;

    static TierEncryptionConfig fromRecordConfig(const RecordEncryptionConfig& db) {
        TierEncryptionConfig c;
        c.id = db.id;
        c.mode = static_cast<RecordEncryptionMode>(db.mode);
        c.verify_salt = db.verifySalt;
        c.verify_hash = db.verifyHash;
        c.salt_enc = db.saltEnc;
        c.salt_hmac = db.saltHmac;
        c.encrypt_old_video = static_cast<bool>(db.encryptOldVideo);
        c.updated_at = db.updatedAt;
        return c;
    }

    RecordEncryptionConfig toRecordConfig() const {
        RecordEncryptionConfig db;
        db.id = id;
        db.mode = static_cast<int>(mode);
        db.verifySalt = verify_salt;
        db.verifyHash = verify_hash;
        db.saltEnc = salt_enc;
        db.saltHmac = salt_hmac;
        db.encryptOldVideo = static_cast<int>(encrypt_old_video);
        db.updatedAt = updated_at;
        return db;
    }

    EncKeyConfig toEncKeyConfig() const {
        EncKeyConfig cfg;
        cfg.mode = mode;
        cfg.salt_enc = RecordEncryptionUtils::hexStrToBytes(salt_enc);
        cfg.salt_hmac = RecordEncryptionUtils::hexStrToBytes(salt_hmac);
        return cfg;
    }

    std::string getEncryptionPasswordHash() const {
        if (verify_hash.empty()) return "";
        return verify_hash.substr(0, std::min(verify_hash.size(), (size_t)32));
    }
};

enum class EncryptionJobStatus {
    kIdle = 0,
    kRunning = 1,
    kCompleted = 2,
    kFailed = 3
};

struct ReencryptProgress {
    std::string tier_id;
    EncryptionJobStatus status = EncryptionJobStatus::kIdle;
    int64_t started_at = 0;
    int64_t finished_at = 0;
    size_t total_files = 0;
    size_t processed_files = 0;
    size_t failed_files = 0;
    size_t skipped_files = 0;
    size_t restored_files = 0;
    std::string current_file;
    std::string error_msg;
};

/**
 * Which segments a re-encryption pass touches.
 *
 * The two operator flows have genuinely different scopes, and conflating them
 * is what made "change the mode" quietly rewrite years of archived footage.
 */
enum class ReencryptScope {
    // Only segments already covered by a range with a real key. This is the
    // password-change scope: footage that was never encrypted stays untouched.
    kEncryptedOnly = 0,
    // Every segment of the pool, including footage recorded before encryption
    // was ever enabled. Used when the operator asks for encrypt_old_video.
    kEverything = 1,
};

/**
 * What one re-encryption pass should do.
 *
 * Bundled into a struct rather than five more parameters because the two flows
 * differ in ways that are easy to mix up: a mode change rewrites every file to
 * one target mode, while a password change rewrites every file to a new key
 * while leaving each file's own mode alone.
 */
struct ReencryptPlan {
    // Mode every processed file should end up in. Ignored when preserve_mode.
    RecordEncryptionMode target_mode = RecordEncryptionMode::kNone;

    // Keep whatever mode each file already has, taken from its trailer, and
    // change only the key. A pool can hold Light and Strong files side by side
    // once a mode change has been applied to new recordings only, so a password
    // change must not flatten them to a single mode.
    bool preserve_mode = false;

    ReencryptScope scope = ReencryptScope::kEverything;

    std::string new_salt_enc;
    std::string new_salt_hmac;
    std::string new_key_hash;
};

struct ReencryptTarget {
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    std::string full_path;
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string tier;
    std::string pool_id;
};

// ===========================================================================
// Storage bridge
// ===========================================================================

struct BridgePoolInfo {
    bool found = false;
    std::string tier;          // "HOT" / "WARM" / "COLD"
    std::string mount_path;
    std::string network_path;
};

struct BridgeSegmentFile {
    std::string camera_id;
    std::string stream_id;
    std::string segment_path;
    std::string full_path;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t file_size = 0;
    bool encrypted = false;
    std::string tier;
    std::string pool_id;
};

struct BridgePlaybackPathResult {
    bool ready = false;
    bool restore_required = false;
    std::string read_path;
    std::string message;
};

struct BridgeRestoreResult {
    bool handled = false;
    std::string job_id;
    std::string message;
};

struct EncryptionStorageBridge {
    std::function<BridgePoolInfo(const std::string& pool_id)> getPoolInfo;
    std::function<void(const std::string& path)> lockSegmentFile;
    std::function<void(const std::string& path)> unlockSegmentFile;
    std::function<bool(const std::string& camera_id, const std::string& stream_id, const std::string& full_path, std::string& out_segment_path)> deriveSegmentPathForFile;
    std::function<bool(const std::string& camera_id, const std::string& stream_id, const std::string& segment_path, const std::string& new_local_file, std::string* err)> replaceSegmentInPlace;
    std::function<std::vector<BridgeSegmentFile>(const std::string& origin_pool_id, int64_t start_time, int64_t end_time)> listSegmentFilesByOriginPool;
    std::function<BridgePlaybackPathResult(const std::string& camera_id, const std::string& stream_id, int64_t segment_start_time, const std::string& timefile_path)> resolvePlaybackSegmentPath;
    std::function<BridgeRestoreResult(const std::string& camera_id, const std::string& stream_id, int64_t segment_start_time, const std::string& path_hint, const std::string& reason)> requestRestoreSegment;
    std::function<bool(const std::string& job_id, std::string& out_status)> getRestoreJobStatus;
    std::function<std::string(const std::string& path)> resolveRecordFilePath;
    std::function<std::string(const std::string& camera_id, const std::string& stream_id, int64_t segment_start_time)> getSegmentOriginPoolId;
    std::function<bool(const std::string& path)> isSegmentFileBusy;
    std::function<void(bool active)> setEncryptionActive;
};

class TierEncryptionManager {
public:
    using Ptr = std::shared_ptr<TierEncryptionManager>;

    static TierEncryptionManager& Instance();
    ~TierEncryptionManager() = default;

    /**
     * Wire this manager to the storage layer.
     *
     * Must be called by TierStorageManager before initialize() runs, since
     * mount-path restoration and startup reconciliation both go through the
     * bridge. This is the only point of contact this class has with
     * TierStorageManager; it never calls back into it any other way.
     */
    void bindStorageBridge(EncryptionStorageBridge bridge);

    // Initialize manager and load persisted configuration.
    void initialize();

    // Create initial encryption configuration for a tier.
    bool initialConfigTier(const std::string& tier_id, RecordEncryptionMode mode,
                           const std::string& password, bool encrypt_old_video);

    // Get encryption configuration of a tier.
    TierEncryptionConfig getTierConfig(const std::string& tier_id) const;

    // Check whether a tier configuration exists.
    bool tierExists(const std::string& tier_id) const;

    /**
     * Whether any recording still depends on this tier's key.
     *
     * Answered from the key history rather than from the segment count:
     * origin_pool_id is stamped on a range when it is first written and is never
     * cleared, so counting segments would keep reporting a dependency long after
     * everything has been decrypted and would make the pool undeletable.
     */
    bool hasEncryptedData(const std::string& tier_id) const;

    /**
    * Check whether a key era with a valid key covers the given timestamp.
    *
    * Uses in-memory key history only. This reflects encryption configuration,
    * not the actual on-disk state.
    */
    bool isEncryptedAtTime(const std::string& tier_id, int64_t timestamp) const;

    // Remove encryption configuration of a tier.
    bool removeTierConfig(const std::string& tier_id);

    // Register mount path for a tier.
    void registerMountPath(const std::string& tier_id, const std::string& mount_path);

    /**
     * Rebuild the mount-path map from the storage layer.
     *
     * The map is in-memory only and was previously filled solely by the pool
     * create/update API, so it came back empty after a restart and newly
     * recorded files were never matched to their tier.
     */
    size_t restoreMountPaths();

    // Drop every mount-path entry pointing at this tier. Needed when pool
    // creation is rolled back, and when a pool is deleted, so a later pool on
    // the same mount is not shadowed by the dead tier id.
    void unregisterMountPath(const std::string& tier_id);

    // ------------------------------------------------------------------
    // Configuration update
    // ------------------------------------------------------------------

    /**
     * Enable or disable encryption of pre-existing recordings.
     *
     * Enabling schedules the pass over old data that the option promises.
     * Pass trigger_reencrypt=false when the caller is about to start its own job
     * (for example pool/update changing the mode in the same request), since
     * only one job may run per tier.
     *
     * Disabling only stops future passes from reaching back; recordings that
     * were already encrypted stay encrypted and remain readable.
     */
    bool updateEncryptOldVideoOption(const std::string& tier_id, bool enable,
                                     bool trigger_reencrypt = true, std::string* err = nullptr);

    // Update encryption mode and optionally re-encrypt existing files.
    bool updateEncryptionMode(const std::string& tier_id, RecordEncryptionMode new_mode,
                              const std::string& password, bool trigger_reencrypt, std::string* err = nullptr);

    // Update encryption password/salts and optionally re-encrypt existing files.
    bool updateEncryptionSalts(const std::string& tier_id, const std::string& old_password,
                               const std::string& new_password, bool trigger_reencrypt, std::string* err = nullptr);

    // ------------------------------------------------------------------
    // Query
    // ------------------------------------------------------------------

    /**
    * Decrypt one segment into a plaintext copy.
    *
    * Writes through a ".part" file and renames it to avoid exposing a
    * partially decrypted segment.
    */
    bool decryptSegmentToFile(const std::string& origin_pool_id, const std::string& enc_path,
                            int64_t segment_start_time, const std::string& out_plain_path,
                            bool* out_written = nullptr, std::string* err = nullptr);

    /**
    * Return a path that can be opened by consumers.
    *
    * Returns the original path for plaintext segments or a decrypted copy
    * under kDecryptSavePath for encrypted segments. COLD segments are restored
    * by TierStorageManager before decryption.
    *
    * @return Empty on failure; err contains the reason.
    */
    std::string materializeReadablePath(const std::string& camera_id,
                                        const std::string& stream_id,
                                        int64_t segment_start_time,
                                        const std::string& local_path,
                                        std::string* err = nullptr);

    /**
    * Same as materializeReadablePath() for callers that only have the file path.
    *
    * Camera, stream, and segment timestamp are resolved from the recording path.
    * Paths outside the recording root are returned unchanged.
    */
    std::string materializeReadablePathForFile(const std::string& record_path,
                                            std::string* err = nullptr);

    /**
    * Remove stale plaintext copies from the decrypt cache.
    *
    * @param remove_all Remove all cached files regardless of age.
    */
    size_t sweepDecryptCache(bool remove_all = false);

    /**
    * Check whether a segment is pending encryption.
    *
    * Returns true when the segment is covered by an encryption key era but
    * does not yet have an encryption trailer.
    */
    bool isSegmentPendingEncryption(const std::string& origin_pool_id,
                                    const std::string& local_path,
                                    int64_t segment_start_time) const;

    // Get encryption mode effective at the specified timestamp.

    // Get encryption time range effective at the specified timestamp.
    EncryptionTimeRange getTimeRangeAtTime(const std::string& tier_id, int64_t timestamp) const;

    /**
     * Keys worth trying for a segment at this timestamp, best guess first.
     *
     * A re-encryption job that failed partway leaves files written in the same
     * second under different keys, so the time range alone is not always enough.
     * Playback walks this list and lets the HMAC decide.
     */
    std::vector<EncryptionTimeRange> getCandidateTimeRanges(const std::string& tier_id, int64_t timestamp) const;

    // ------------------------------------------------------------------
    // Re-encryption progress
    // ------------------------------------------------------------------

    // Get re-encryption progress of a tier.
    ReencryptProgress getReencryptProgress(const std::string& tier_id) const;

    // Clear re-encryption progress of a tier.
    void clearReencryptProgress(const std::string& tier_id);

    // Check whether any re-encryption job is running.
    bool isAnyJobRunning() const;

    // Get the tier ID of the currently running job.
    std::string getRunningJobTierId() const;

    /**
     * Start re-encryption using the current tier configuration.
     * Used to encrypt existing plaintext files after enabling encryption.
     */
    bool startReencryptJob(const std::string& tier_id, std::string* err = nullptr);

private:
    TierEncryptionManager();

    // ------------------------------------------------------------------
    // Tier configuration
    // ------------------------------------------------------------------

    // Get tier configuration by mount path.
    TierEncryptionConfig getTierConfigByMountPath(const std::string& path) const;

    // Get all configured tier IDs.
    std::vector<std::string> getAllTierIds() const;

    // Validate HOT pool before encryption operations.
    bool verifyHotPool(const std::string& tier_id, std::string* err = nullptr) const;

    // ------------------------------------------------------------------
    // Time range
    // ------------------------------------------------------------------

    // Find the time range containing the specified timestamp.
    const EncryptionTimeRange* findTimeRangeAtTime(const std::vector<EncryptionTimeRange>& ranges, int64_t timestamp) const;

    // Load encryption time ranges from database.
    void updateTimeRangesFromDb(const std::string& tier_id, TierEncryptionConfig& config);

    // Save encryption time ranges to database.
    void saveTimeRangesToDatabase(const std::string& tier_id, const std::vector<EncryptionTimeRange>& ranges);

    // ------------------------------------------------------------------
    // Record handling
    // ------------------------------------------------------------------

    // Encrypt a newly finalized MP4 file.
    void onRecordMP4Finished(const mediakit::RecordInfo& info);

    /**
    * Determine which key a finalized recording should be encrypted with.
    *
    * The key is selected from the segment timestamp, not the pool's current
    * configuration, so late-arriving recordings use the correct key era.
    */
    struct SegmentKeyDecision {
        bool encrypt = false;
        RecordEncryptionMode mode = RecordEncryptionMode::kNone;
        std::string salt_enc;
        std::string salt_hmac;
        std::string key_hash;
        bool needs_range_cover = false;
        std::string skip_reason;
    };

    SegmentKeyDecision resolveKeyForSegmentTime(const TierEncryptionConfig& config,
                                                int64_t segment_start_time) const;

    /**
    * Add coverage for a backfilled segment using the key that was actually used.
    *
    * Extends an existing range when its key and mode match; otherwise inserts
    * a separate range. Successive backfills under the same key share one range.
    */
    void coverSegmentWithUsedKey(const std::string& tier_id,
                                int64_t segment_start_time,
                                RecordEncryptionMode mode,
                                const std::string& salt_enc,
                                const std::string& salt_hmac,
                                const std::string& key_hash);

    // Find and process stale files during startup. Runs on a worker thread and
    // swallows per-tier failures: it must never be able to abort server startup.
    void reconcileStaleFilesOnStartup();

    // Reconcile a single tier. Split out so one failing pool does not skip the rest.
    void reconcileStaleFilesForTier(const std::string& tier_id);

    // Process a stale file detected during reconciliation. The target mode and
    // key come from the range covering the file's own timestamp, not from the
    // pool's current configuration, which may have moved on since it was written.
    void handleStaleFile(const std::string& finalized_path, const EncryptionTimeRange& range);

    // ------------------------------------------------------------------
    // Re-encryption
    // ------------------------------------------------------------------

    // Initialize re-encryption job state.
    bool beginJob(const std::string& tier_id);

    // List the segments a pass should visit, according to its scope.
    std::vector<ReencryptTarget> listReencryptTargets(const std::string& tier_id, ReencryptScope scope);

    // Ensure a segment is available on local disk for processing.
    bool ensureSegmentReadable(const ReencryptTarget& target, std::string& out_local_path,
                               int timeout_sec, std::string* err = nullptr);

    // Re-encrypt one file from the old key/mode to the new key/mode.
    bool reencryptOneFile(const std::string& file_path, RecordEncryptionMode old_mode,
                          RecordEncryptionMode new_mode, const std::string& old_salt_enc,
                          const std::string& old_salt_hmac, const std::string& new_salt_enc,
                          const std::string& new_salt_hmac, const std::string& old_key_hash,
                          const std::string& new_key_hash);

    // Decrypt an encrypted file to a temporary plaintext file.
    // old_mode is a hint only; the trailer in the file wins if they disagree.
    bool decryptFileToPlain(const std::string& enc_path, const std::string& out_plain_path,
                            RecordEncryptionMode old_mode, const std::string& old_salt_enc,
                            const std::string& old_salt_hmac, const std::string& old_key_hash);

    // Encrypt a plaintext file in place and append the encryption trailer.
    bool encryptPlainFile(const std::string& plain_path, RecordEncryptionMode new_mode,
                          const std::string& new_salt_enc, const std::string& new_salt_hmac,
                          const std::string& new_key_hash);

    // Run the internal re-encryption workflow.
    void runReencryptionJobInternal(const std::string& tier_id, const ReencryptPlan& plan);

    // Run the re-encryption job.
    void runReencryptionJob(const std::string& tier_id, const ReencryptPlan& plan);

    /**
     * Work out which key a file on disk is actually under.
     *
     * The time range is only a hint: a recording that was already running when
     * encryption was switched on starts before the first range does, so it ends
     * up encrypted while every timestamp lookup reports "none". This walks the
     * candidate keys and lets the HMAC decide.
     *
     * @return false only when the file is encrypted and no known key verifies
     *         it. A plaintext file returns true with mode kNone.
     */
    bool resolveOldKeyForFile(const std::string& tier_id, const std::string& local_path,
                              int64_t segment_start_time, EncryptionTimeRange& out_range) const;

    // Drop remembered key resolutions. Must be called whenever the key history
    // changes, or a file could be opened with a key that no longer applies.
    void clearResolvedKeyCache();


    /**
     * Collapse the whole key history into one range after a mode change that
     * rewrote every file. Only valid when the pass covered everything and every
     * file ended in the same mode.
     */
    void collapseTimeRangesAfterModeChange(const std::string& tier_id, RecordEncryptionMode new_mode,
                                           const std::string& new_salt_enc, const std::string& new_salt_hmac,
                                           const std::string& new_key_hash, int64_t earliest_processed_start);

    /**
     * Point every key-bearing range at the new key after a password change,
     * keeping each range's window and mode.
     *
     * Collapsing here would be wrong: the pool can legitimately hold a Light
     * era and a Strong era, and flattening them would erase the boundary that
     * says which is which. Adjacent ranges that end up identical are coalesced
     * so the table does not grow without bound.
     */
    void rekeyTimeRanges(const std::string& tier_id, const std::string& new_salt_enc,
                         const std::string& new_salt_hmac, const std::string& new_key_hash);

    /**
     * Pull the start of the earliest time range back to cover data that already
     * exists on disk, before a re-encryption job touches any file.
     *
     * Without this the job encrypts old files while the configuration still
     * reports "none" for their timestamps, so they are opened as plaintext for
     * the duration of the job, and stay that way forever if the job fails
     * before the ranges are merged.
     */
    bool extendEarliestRangeStart(const std::string& tier_id, int64_t new_start);

    // Earliest segment start time known to the storage layer for this pool.
    // Returns 0 when the pool has no segments.
    int64_t findEarliestSegmentStart(const std::string& tier_id);

    // Any pool configured with a real mode. Cheap; guards every decrypt-cache
    // path so a deployment without encryption pays nothing.
    bool encryptionInUse() const;

    // Sweep at most once per kDecryptSweepIntervalSec, called from the decrypt
    // path itself.
    void maybeSweepDecryptCache();

    // Recompute and publish "any pool uses encryption" to the storage layer.
    // Must be called with _mutex held.
    void refreshStorageEncryptionFlag();

    // Repositories are lightweight DB-access objects created on demand, one
    // per call, rather than kept as members (see e.g. initialConfigTier()).
    // This flag is what actually gates use before initialize() has run.
    bool reposReady(std::string* err = nullptr) const;

    // File-level serialization is owned by TierStorageManager, which owns the files.
    // Shared locking prevents the tiering job from copying a segment while it is
    // being encrypted, via _storage.lockSegmentFile / unlockSegmentFile.

    // Guard every use of _storage so calls made before TierStorageManager registers
    // it fail cleanly instead of invoking an empty std::function.
    bool storageReady(std::string* err = nullptr) const;

    // RAII wrapper around _storage.lockSegmentFile / unlockSegmentFile. Direct
    // replacement for the old TierStorageManager::SegmentFileLock, minus the
    // dependency on TierStorageManager's type.
    class SegmentGuard {
    public:
        SegmentGuard(const EncryptionStorageBridge& bridge, const std::string& path);
        ~SegmentGuard();
        SegmentGuard(const SegmentGuard&) = delete;
        SegmentGuard& operator=(const SegmentGuard&) = delete;
    private:
        const EncryptionStorageBridge& _bridge;
        std::string _path;
    };

private:
    mutable std::mutex _mutex;
    mutable std::mutex _progress_mtx;
    mutable std::mutex _key_cache_mtx;
    mutable std::map<std::string, EncryptionTimeRange> _resolved_key_cache;
    static constexpr size_t kResolvedKeyCacheMax = 4096;
    EncryptionStorageBridge _storage;
    bool _initialized = false;

    std::map<std::string, TierEncryptionConfig> _tier_configs;
    std::map<std::string, ReencryptProgress> _reencrypt_progress;
    std::map<std::string, std::string> _mount_path_to_tier;

    std::shared_ptr<int> _alive = std::make_shared<int>(0);

    std::atomic<int64_t> _decrypt_sweep_at{0};
    static constexpr int64_t kDecryptSweepIntervalSec = 60;
};

} // namespace managerkit

#endif // LOCAL_TIERENCRYPTIONMANAGER_H

#endif // ENABLE_TIER_STORAGE
