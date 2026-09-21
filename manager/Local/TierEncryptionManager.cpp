#ifdef ENABLE_TIER_STORAGE

#include <thread>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>

#include "Util/File.h"
#include "Common/config.h"
#include "Common/BlockingExecutors.h"

#include "StorageTier.h"
#include "TierEncryptionManager.h"

using namespace std;
using namespace mediakit;

namespace managerkit {

namespace {

static bool copyFileChunked(const std::string &src, const std::string &dst) {
    FILE *in = File::create_file(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = File::create_file(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }

    std::vector<uint8_t> buf(1 << 20);
    bool ok = true;
    while (true) {
        size_t n = fread(buf.data(), 1, buf.size(), in);
        if (n == 0) {
            if (ferror(in)) ok = false;
            break;
        }
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    }
    if (fflush(out) != 0) ok = false;
    fclose(in);
    fclose(out);
    if (!ok) remove(dst.c_str());
    return ok;
}

static constexpr int kReencryptRestoreTimeoutSec = 600;

} // namespace

INSTANCE_IMP(TierEncryptionManager)
TierEncryptionManager::TierEncryptionManager() {}

void TierEncryptionManager::bindStorageBridge(EncryptionStorageBridge bridge) {
    std::lock_guard<std::mutex> lock(_mutex);
    _storage = std::move(bridge);
}

bool TierEncryptionManager::storageReady(std::string* err) const {
    if (!_storage.getPoolInfo) {
        if (err) *err = "TierEncryptionManager: storage bridge not bound yet";
        WarnL << "TierEncryptionManager storage bridge used before TierStorageManager registered it";
        return false;
    }
    return true;
}

TierEncryptionManager::SegmentGuard::SegmentGuard(const EncryptionStorageBridge& bridge, const std::string& path)
    : _bridge(bridge), _path(path) {
    if (_bridge.lockSegmentFile) {
        _bridge.lockSegmentFile(_path);
    }
}

TierEncryptionManager::SegmentGuard::~SegmentGuard() {
    if (_bridge.unlockSegmentFile) {
        _bridge.unlockSegmentFile(_path);
    }
}

const EncryptionTimeRange* TierEncryptionManager::findTimeRangeAtTime(const std::vector<EncryptionTimeRange>& ranges, int64_t timestamp) const {
    // Half-open [start, end). A mode or password change closes the old range at
    // `now` and opens the new one at `now`; with an inclusive upper bound both
    // matched and the loop returned whichever came first in the vector, which
    // is the *old* range, so a file written in that second decrypted with the
    // wrong key.
    const EncryptionTimeRange* best = nullptr;
    for (const auto& range : ranges) {
        if (timestamp >= range.start_time && timestamp < range.end_time) {
            // Prefer the most recently opened range if the table ever ends up
            // with overlapping rows.
            if (!best || range.start_time > best->start_time) {
                best = &range;
            }
        }
    }
    return best;
}

void TierEncryptionManager::refreshStorageEncryptionFlag() {
    bool active = false;
    for (const auto& kv : _tier_configs) {
        if (kv.second.mode != RecordEncryptionMode::kNone) { active = true; break; }
    }
    // Also stays true while a tier is configured with mode none but still holds
    // encrypted history, because removeTierConfig() is what finally clears it.
    if (!active) active = !_tier_configs.empty();
    if (_storage.setEncryptionActive) {
        _storage.setEncryptionActive(active);
    }
}

bool TierEncryptionManager::reposReady(std::string* err) const {
    if (!_initialized) {
        if (err) *err = "TierEncryptionManager::initialize() has not run yet";
        WarnL << "TierEncryptionManager used before initialize()";
        return false;
    }
    return true;
}


void TierEncryptionManager::initialize() {
    {
        std::lock_guard<std::mutex> lock(_mutex);

        RecordEncryptionConfigImp config_repo;
        RecordEncryptionTierRangeImp time_range_repo;
        _initialized = true;

        _tier_configs.clear();
        _reencrypt_progress.clear();

        auto db_configs = config_repo.findAll();
        for (const auto& db_config : db_configs) {
            TierEncryptionConfig config = TierEncryptionConfig::fromRecordConfig(db_config);
            updateTimeRangesFromDb(config.id, config);
            _tier_configs[config.id] = config;
        }

        InfoL << "Loaded " << _tier_configs.size() << " tier configs";
        refreshStorageEncryptionFlag();
    }
    sweepDecryptCache(/*remove_all=*/true);

    size_t restored = 0;
    try {
        restored = restoreMountPaths();
    } catch (const std::exception& ex) {
        WarnL << "Could not restore encryption mount paths yet: " << ex.what();
    }

    NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastRecordMP4,
        [this](BroadcastRecordMP4Args) {
            onRecordMP4Finished(info);
        });

    // Deferred on purpose. This walks every segment of every encrypted pool and
    // issues a TimeQuery per range, which is slow enough to be noticeable at
    // boot, and it depends on the storage layer and the database being fully up.
    // Running it inline made a failure there fatal to server startup.
    std::weak_ptr<void> alive = _alive;
    BlockingExecutors::encryption()->async([this, alive, restored]() {
        if (alive.expired()) return;
        try {
            if (restored == 0) {
                restoreMountPaths();
            }
            reconcileStaleFilesOnStartup();
        } catch (const std::exception& ex) {
            WarnL << "Startup reconcile aborted: " << ex.what();
        } catch (...) {
            WarnL << "Startup reconcile aborted";
        }
    });
}

void TierEncryptionManager::updateTimeRangesFromDb(const std::string& tier_id, TierEncryptionConfig& config) {
    config.time_ranges.clear();
    RecordEncryptionTierRangeImp time_range_repo;
    auto db_ranges = time_range_repo.loadTimeRanges(tier_id);
    for (const auto& db_range : db_ranges) {
        config.time_ranges.push_back(EncryptionTimeRange::fromDbEntity(db_range));
    }
}

void TierEncryptionManager::saveTimeRangesToDatabase(const std::string& tier_id, const std::vector<EncryptionTimeRange>& ranges) {
    std::vector<RecordEncryptionTierRange> db_ranges;
    db_ranges.reserve(ranges.size());
    for (const auto& range : ranges) {
        db_ranges.push_back(range.toDbEntity(tier_id));
    }
    RecordEncryptionTierRangeImp time_range_repo;
    time_range_repo.saveTimeRanges(tier_id, db_ranges);
}

bool TierEncryptionManager::initialConfigTier(const std::string& tier_id, RecordEncryptionMode mode,
                           const std::string& password, bool encrypt_old_video) {

    if (!verifyHotPool(tier_id)) {
        WarnL << "Rejecting encryption configuration: pool does not exist or is not in the HOT tier: " << tier_id;
        return false;
    }

    if (mode != RecordEncryptionMode::kNone && password.empty()) {
        WarnL << "Rejecting encryption configuration: password is required for mode "
              << static_cast<int>(mode) << " on tier " << tier_id;
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady()) return false;
    if (tier_id.empty()) return false;
    if (_tier_configs.find(tier_id) != _tier_configs.end()) return false;

    std::string verify_salt = RecordEncryptionUtils::makeRandomHex(16);
    std::string verify_hash = RecordEncryptionUtils::computeVerificationHash(password, verify_salt);
    std::string salt_enc = RecordEncryptionUtils::bytesToHexStr(RecordEncryptionUtils::randomBytesVec(16));
    std::string salt_hmac = RecordEncryptionUtils::bytesToHexStr(RecordEncryptionUtils::randomBytesVec(16));
    std::string key_hash = verify_hash.substr(0, std::min(verify_hash.size(), (size_t)32));

    const int64_t now = (int64_t)time(nullptr);
    TierEncryptionConfig config;
    config.id = tier_id;
    config.mode = mode;
    config.verify_salt = verify_salt;
    config.verify_hash = verify_hash;
    config.salt_enc = salt_enc;
    config.salt_hmac = salt_hmac;
    config.encrypt_old_video = encrypt_old_video;
    config.updated_at = now;

    if (mode != RecordEncryptionMode::kNone) {
        EncryptionTimeRange range;
        range.start_time = now;
        range.end_time = INT64_MAX;
        range.salt_enc = salt_enc;
        range.salt_hmac = salt_hmac;
        range.key_hash = key_hash;
        range.mode = mode;
        config.time_ranges.push_back(range);
    }

    RecordEncryptionConfig db_config = config.toRecordConfig();
    RecordEncryptionConfigImp config_repo;
    if (!config_repo.addOrUpdate(db_config)) return false;

    if (!config.time_ranges.empty()) {
        std::vector<RecordEncryptionTierRange> db_ranges;
        db_ranges.push_back(config.time_ranges[0].toDbEntity(tier_id));
        RecordEncryptionTierRangeImp time_range_repo;
        if (!time_range_repo.insertBatch(db_ranges)) {
            config_repo.remove(db_config);
            return false;
        }
    }

    _tier_configs[tier_id] = config;
    refreshStorageEncryptionFlag();
    InfoL << "Created tier: " << tier_id;
    return true;
}

TierEncryptionConfig TierEncryptionManager::getTierConfig(const std::string& tier_id) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tier_configs.find(tier_id);
    return it == _tier_configs.end() ? TierEncryptionConfig() : it->second;
}

bool TierEncryptionManager::verifyHotPool(const std::string& tier_id, std::string* err) const {
    if (tier_id.empty()) {
        if (err) *err = "tier_id (pool_id) is empty";
        return false;
    }
    if (!storageReady(err)) return false;
    BridgePoolInfo info = _storage.getPoolInfo(tier_id);
    if (!info.found) {
        if (err) *err = "Storage pool not found: " + tier_id;
        return false;
    }
    if (info.tier != tierTypeToString(HotTier)) {
        if (err) *err = "Encryption is only supported on HOT tier pools, this pool is: " + info.tier;
        return false;
    }
    return true;
}

size_t TierEncryptionManager::restoreMountPaths() {
    std::vector<std::string> tier_ids;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& kv : _tier_configs) {
            tier_ids.push_back(kv.first);
        }
    }

    if (!storageReady()) {
        WarnL << "Cannot restore encryption mount paths: storage bridge not bound yet";
        return 0;
    }

    size_t restored = 0;
    for (const auto& tier_id : tier_ids) {
        BridgePoolInfo info = _storage.getPoolInfo(tier_id);
        if (!info.found) {
            WarnL << "Encryption configuration refers to a pool that no longer exists: " << tier_id;
            continue;
        }
        if (info.tier != tierTypeToString(HotTier)) {
            continue;
        }

        std::string root = info.mount_path;
        if (root.empty()) {
            root = info.network_path;
        }
        if (root.empty()) {
            WarnL << "HOT pool has no mount or network path, recordings on it cannot be encrypted: " << tier_id;
            continue;
        }
        registerMountPath(tier_id, root);
        ++restored;
    }
    InfoL << "Restored " << restored << " encryption mount path(s) of "
          << tier_ids.size() << " configured tier(s)";
    return restored;
}

void TierEncryptionManager::unregisterMountPath(const std::string& tier_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto it = _mount_path_to_tier.begin(); it != _mount_path_to_tier.end();) {
        it = (it->second == tier_id) ? _mount_path_to_tier.erase(it) : std::next(it);
    }
}

void TierEncryptionManager::registerMountPath(const std::string& tier_id, const std::string& mount_path) {
    std::lock_guard<std::mutex> lock(_mutex);
    _mount_path_to_tier[mount_path] = tier_id;
}

TierEncryptionConfig TierEncryptionManager::getTierConfigByMountPath(const std::string& path) const {
    std::string tier_id;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto it = _mount_path_to_tier.rbegin(); it != _mount_path_to_tier.rend(); ++it) {
            if (path.compare(0, it->first.size(), it->first) == 0) {
                tier_id = it->second;
                break;
            }
        }
    }
    return tier_id.empty() ? TierEncryptionConfig() : getTierConfig(tier_id);
}

std::vector<std::string> TierEncryptionManager::getAllTierIds() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::string> ids;
    ids.reserve(_tier_configs.size());
    for (const auto& item : _tier_configs) {
        ids.push_back(item.first);
    }
    return ids;
}

bool TierEncryptionManager::hasEncryptedData(const std::string& tier_id) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) return false;

    const TierEncryptionConfig& config = it->second;
    if (config.mode != RecordEncryptionMode::kNone) return true;
    // origin_pool_id on a segment range is never cleared, so counting segments
    // cannot tell whether anything is still encrypted. The key history can: once
    // a decrypt pass finishes, collapseTimeRangesAfterModeChange() collapses the
    // history into a single range with mode none.
    for (const auto& r : config.time_ranges) {
        if (r.mode != RecordEncryptionMode::kNone) return true;
    }
    return false;
}

bool TierEncryptionManager::isEncryptedAtTime(const std::string& tier_id, int64_t timestamp) const {
    if (tier_id.empty() || timestamp <= 0) return false;
    const EncryptionTimeRange range = getTimeRangeAtTime(tier_id, timestamp);
    return range.mode != RecordEncryptionMode::kNone && !range.key_hash.empty();
}

bool TierEncryptionManager::tierExists(const std::string& tier_id) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _tier_configs.find(tier_id) != _tier_configs.end();
}

bool TierEncryptionManager::removeTierConfig(const std::string& tier_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady()) return false;

    if (isAnyJobRunning()) {
        std::string running_tier = getRunningJobTierId();
        if (running_tier == tier_id) {
            WarnL << "Cannot remove tier " << tier_id << ": job is running";
            return false;
        }
    }

    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) return false;

    RecordEncryptionTierRangeImp time_range_repo;
    if (!time_range_repo.deleteByTierId(tier_id)) return false;

    RecordEncryptionConfig db_config;
    db_config.id = tier_id;
    RecordEncryptionConfigImp config_repo;
    config_repo.remove(db_config);

    _tier_configs.erase(it);
    for (auto mit = _mount_path_to_tier.begin(); mit != _mount_path_to_tier.end();) {
        mit = (mit->second == tier_id) ? _mount_path_to_tier.erase(mit) : std::next(mit);
    }
    refreshStorageEncryptionFlag();
    clearReencryptProgress(tier_id);
    DerivedKeyCache::clear();
    InfoL << "Removed tier: " << tier_id;
    return true;
}

bool TierEncryptionManager::isAnyJobRunning() const {
    std::lock_guard<std::mutex> lock(_progress_mtx);
    for (const auto& item : _reencrypt_progress) {
        if (item.second.status == EncryptionJobStatus::kRunning) {
            return true;
        }
    }
    return false;
}

std::string TierEncryptionManager::getRunningJobTierId() const {
    std::lock_guard<std::mutex> lock(_progress_mtx);
    for (const auto& item : _reencrypt_progress) {
        if (item.second.status == EncryptionJobStatus::kRunning) {
            return item.first;
        }
    }
    return "";
}

TierEncryptionManager::SegmentKeyDecision TierEncryptionManager::resolveKeyForSegmentTime(const TierEncryptionConfig& config, int64_t segment_start_time) const {
    SegmentKeyDecision d;

    if (segment_start_time > 0) {
        const EncryptionTimeRange range = getTimeRangeAtTime(config.id, segment_start_time);
        const bool covered = (range.end_time > range.start_time);
        if (covered) {
            if (range.mode == RecordEncryptionMode::kNone || range.key_hash.empty()) {
                d.skip_reason = "encryption was off during this segment's time range";
                return d;
            }
            if (range.salt_enc.empty() || range.salt_hmac.empty()) {
                d.skip_reason = "the key era covering this timestamp has no usable salts";
                return d;
            }
            d.encrypt   = true;
            d.mode      = range.mode;
            d.salt_enc  = range.salt_enc;
            d.salt_hmac = range.salt_hmac;
            d.key_hash  = range.key_hash;
            return d;
        }
    }

    if (!config.encrypt_old_video) {
        d.skip_reason = "no key era covers this timestamp and encrypt_old_video is off";
        return d;
    }
    if (config.mode == RecordEncryptionMode::kNone || config.time_ranges.empty()) {
        d.skip_reason = "no key era covers this timestamp and the pool is not encrypting";
        return d;
    }

    d.encrypt            = true;
    d.mode               = config.mode;
    d.salt_enc           = config.salt_enc;
    d.salt_hmac          = config.salt_hmac;
    d.key_hash           = config.getEncryptionPasswordHash();
    d.needs_range_cover  = true;
    return d;
}

void TierEncryptionManager::coverSegmentWithUsedKey(const std::string& tier_id,
                                                    int64_t segment_start_time,
                                                    RecordEncryptionMode mode,
                                                    const std::string& salt_enc,
                                                    const std::string& salt_hmac,
                                                    const std::string& key_hash) {
    if (segment_start_time <= 0 || mode == RecordEncryptionMode::kNone) return;

    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady()) return;

    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end() || it->second.time_ranges.empty()) {
        WarnL << "Cannot record the key era of a backfilled segment, tier has no key history: " << tier_id;
        return;
    }

    TierEncryptionConfig& config = it->second;
    auto earliest = std::min_element(config.time_ranges.begin(), config.time_ranges.end(),
                                     [](const EncryptionTimeRange& a, const EncryptionTimeRange& b) {
                                         return a.start_time < b.start_time;
                                     });
    if (earliest->start_time <= segment_start_time) {
        return;   // already covered
    }

    const int64_t earliest_start = earliest->start_time;
    const bool same_key = (earliest->mode == mode &&
                           earliest->key_hash == key_hash &&
                           earliest->salt_enc == salt_enc &&
                           earliest->salt_hmac == salt_hmac);

    if (same_key) {
        earliest->start_time = segment_start_time;
        InfoL << "Extended earliest encryption range of tier " << tier_id
              << " back to " << segment_start_time << " for a backfilled segment";
    } else {
        config.time_ranges.emplace_back(segment_start_time, earliest_start,
                                        salt_enc, salt_hmac, key_hash, mode);
        InfoL << "Recorded a new encryption range [" << segment_start_time << ", " << earliest_start
              << ") for backfilled segments of tier " << tier_id
              << "; the key differs from the previously earliest era";
    }
    saveTimeRangesToDatabase(tier_id, config.time_ranges);
}

void TierEncryptionManager::onRecordMP4Finished(const mediakit::RecordInfo& info) {
    TierEncryptionConfig config = getTierConfigByMountPath(info.folder);
    if (config.id.empty()) return;

    const int64_t segment_start_time = (int64_t)info.start_time;

    const SegmentKeyDecision decision = resolveKeyForSegmentTime(config, segment_start_time);
    if (!decision.encrypt) {
        DebugL << "Leaving recording in plaintext (" << decision.skip_reason << "): " << info.file_path;
        return;
    }

    if (!verifyHotPool(config.id)) {
        WarnL << "Skipping encryption: pool " << config.id << " no longer exists or is not in the HOT tier: " << info.file_path;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_progress_mtx);
        auto it = _reencrypt_progress.find(config.id);
        if (it != _reencrypt_progress.end() && it->second.status == EncryptionJobStatus::kRunning) {
            InfoL << "Skipping immediate encryption (re-encryption job is running for tier " << config.id << "): " << info.file_path;
            return;
        }
    }

    std::string file_path = info.file_path;
    std::string tier_id   = config.id;
    std::string camera_id = info.app;
    std::string stream_id = info.stream;
    RecordEncryptionMode mode = decision.mode;
    std::string salt_enc  = decision.salt_enc;
    std::string salt_hmac = decision.salt_hmac;
    std::string key_hash  = decision.key_hash;
    const bool needs_range_cover = decision.needs_range_cover;

    std::weak_ptr<void> alive = _alive;
    BlockingExecutors::encryption()->async(
        [this, alive, file_path, tier_id, camera_id, stream_id, segment_start_time, mode, salt_enc, salt_hmac, key_hash, needs_range_cover]() {
            if (alive.expired()) return;
            try {
                // The check in the caller happens before this task is queued, so a
                // job can start in between. Re-check here, and hold the per-file
                // guard for the whole operation so the job cannot rename the same
                // file underneath us.
                {
                    std::lock_guard<std::mutex> lock(_progress_mtx);
                    auto it = _reencrypt_progress.find(tier_id);
                    if (it != _reencrypt_progress.end() && it->second.status == EncryptionJobStatus::kRunning) {
                        InfoL << "Skipping immediate encryption, job started meanwhile: " << file_path;
                        return;
                    }
                }

                SegmentGuard guard(_storage, file_path);

                if (!reencryptOneFile(file_path, RecordEncryptionMode::kNone, mode, "", "", salt_enc, salt_hmac, "", key_hash)) {
                    WarnL << "Failed to encrypt new file: " << file_path;
                    return;
                }

                // The file is now "<name>.mp4.enc" while the recording index
                // still refers to "<name>.mp4". Tell the storage layer so the
                // range statistics match what is on disk. Path resolution on the
                // read side probes both variants, so a failure here is not fatal
                // — it only leaves stale size/updated_at on the range row.
                std::string result_path = RecordEncryptionUtils::computeTargetPath(file_path, mode);
                std::string segment_path;
                if (!_storage.deriveSegmentPathForFile ||
                    !_storage.deriveSegmentPathForFile(camera_id, stream_id, result_path, segment_path)) {
                    DebugL << "Could not derive segment path after encrypting " << result_path;
                    return;
                }
                std::string replace_err;
                if (!_storage.replaceSegmentInPlace ||
                    !_storage.replaceSegmentInPlace(camera_id, stream_id, segment_path, result_path, &replace_err)) {
                    DebugL << "replaceSegmentInPlace after immediate encryption: " << replace_err
                           << " (" << result_path << ")";
                }

                if (needs_range_cover) {
                    coverSegmentWithUsedKey(tier_id, segment_start_time, mode,
                                            salt_enc, salt_hmac, key_hash);
                }
            } catch (const std::exception& ex) {
                WarnL << "Exception while encrypting new file " << file_path << ": " << ex.what();
            }
        });
}

void TierEncryptionManager::handleStaleFile(const std::string &finalized_path, const EncryptionTimeRange &range) {
    if (range.mode == RecordEncryptionMode::kNone || range.key_hash.empty()) {
        // No key was in force when this file was written, so it is meant to be
        // plaintext and there is nothing to reconcile.
        return;
    }
    try {
        bool ok = reencryptOneFile(finalized_path,
                                    RecordEncryptionMode::kNone, range.mode,
                                    "", "",
                                    range.salt_enc, range.salt_hmac,
                                    "", range.key_hash);
        if (!ok) {
            WarnL << "Failed to process orphaned file: " << finalized_path;
        }
    } catch (const std::exception &ex) {
        WarnL << "Exception while processing orphaned file " << finalized_path << ": " << ex.what();
    }
}

void TierEncryptionManager::reconcileStaleFilesOnStartup() {
    // Every tier is scanned independently so one bad configuration cannot stop
    // the others, and the whole function is contained: it runs on a worker
    // thread with no handler above it, where an escaping exception terminates
    // the process instead of failing one pool.
    for (const auto& tier_id : getAllTierIds()) {
        try {
            reconcileStaleFilesForTier(tier_id);
        } catch (const std::exception& ex) {
            WarnL << "Startup reconcile failed for tier " << tier_id << ": " << ex.what();
        } catch (...) {
            WarnL << "Startup reconcile failed for tier " << tier_id;
        }
    }
}

void TierEncryptionManager::reconcileStaleFilesForTier(const std::string& tier_id) {
    TierEncryptionConfig config = getTierConfig(tier_id);
    if (config.mode == RecordEncryptionMode::kNone) return;

    for (const auto& target : listReencryptTargets(tier_id, ReencryptScope::kEverything)) {
        if (target.full_path.empty()) continue;
        if (target.tier != "HOT") continue;

        // The recording index stores plaintext names, so the path handed to us
        // may lack the ".enc" suffix even when the file on disk has it. Decide
        // from the trailer magic, which does not depend on the name.
        if (RecordEncryptionUtils::hasEncryptionTrailer(target.full_path)) continue;

        // The mode and key that applied when this file was written, which is
        // not necessarily the pool's current configuration: a mode change with
        // encrypt_old_video off leaves earlier eras exactly as they were.
        EncryptionTimeRange range = getTimeRangeAtTime(tier_id, target.start_time);

        SegmentGuard guard(_storage, target.full_path);
        handleStaleFile(target.full_path, range);
    }
}

bool TierEncryptionManager::beginJob(const std::string& tier_id) {
    std::lock_guard<std::mutex> lock(_progress_mtx);

    for (const auto& item : _reencrypt_progress) {
        if (item.second.status == EncryptionJobStatus::kRunning) {
            WarnL << "Job already running for tier: " << item.first;
            return false;
        }
    }

    auto it = _reencrypt_progress.find(tier_id);
    if (it != _reencrypt_progress.end() && 
        it->second.status == EncryptionJobStatus::kRunning) {
        return false;
    }

    ReencryptProgress progress;
    progress.tier_id = tier_id;
    progress.status = EncryptionJobStatus::kRunning;
    progress.started_at = (int64_t)time(nullptr);
    _reencrypt_progress[tier_id] = progress;
    return true;
}

int64_t TierEncryptionManager::findEarliestSegmentStart(const std::string& tier_id) {
    if (!_storage.listSegmentFilesByOriginPool) return 0;
    int64_t earliest = 0;
    for (const auto& sf : _storage.listSegmentFilesByOriginPool(tier_id, 0, 0)) {
        if (sf.start_time > 0 && (earliest == 0 || sf.start_time < earliest)) {
            earliest = sf.start_time;
        }
    }
    return earliest;
}

bool TierEncryptionManager::extendEarliestRangeStart(const std::string& tier_id, int64_t new_start) {
    if (new_start <= 0) return true;

    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady()) return false;

    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end() || it->second.time_ranges.empty()) return false;

    TierEncryptionConfig& config = it->second;
    auto earliest = std::min_element(config.time_ranges.begin(), config.time_ranges.end(),
                                     [](const EncryptionTimeRange& a, const EncryptionTimeRange& b) {
                                         return a.start_time < b.start_time;
                                     });
    if (earliest->start_time <= new_start) {
        return true;   // already covered
    }

    earliest->start_time = new_start;
    saveTimeRangesToDatabase(tier_id, config.time_ranges);
    InfoL << "Extended earliest encryption range of tier " << tier_id << " back to " << new_start;
    return true;
}

bool TierEncryptionManager::startReencryptJob(const std::string& tier_id, std::string* err) {
    {
        std::string repo_err;
        if (!reposReady(&repo_err)) {
            if (err) *err = repo_err;
            return false;
        }
    }

    TierEncryptionConfig config;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _tier_configs.find(tier_id);
        if (it == _tier_configs.end()) {
            if (err) *err = "Tier not configured: " + tier_id;
            return false;
        }
        config = it->second;
    }

    if (config.mode == RecordEncryptionMode::kNone) {
        if (err) *err = "Encryption mode is none, nothing to do";
        return false;
    }

    if (!beginJob(tier_id)) {
        if (err) *err = "Another re-encrypt job is running for tier: " + getRunningJobTierId();
        return false;
    }

    // Widen the range before the first file is touched. Files the job has
    // already encrypted then resolve to the correct key while it is still
    // running, and stay decryptable even if the job aborts halfway. The
    // demuxer falls back to plaintext for files inside the range that have no
    // trailer yet, so widening early cannot break the not-yet-processed ones.
    if (config.encrypt_old_video) {
        int64_t earliest = findEarliestSegmentStart(tier_id);
        if (earliest > 0) {
            extendEarliestRangeStart(tier_id, earliest);
        }
    }

    std::string new_salt_enc  = config.salt_enc;
    std::string new_salt_hmac = config.salt_hmac;
    std::string new_key_hash  = config.getEncryptionPasswordHash();
    RecordEncryptionMode mode = config.mode;

    // Applying the current mode to footage that predates encryption is exactly
    // what encrypt_old_video asks for, so this pass covers the whole pool.
    ReencryptPlan plan;
    plan.target_mode   = mode;
    plan.preserve_mode = false;
    plan.scope         = ReencryptScope::kEverything;
    plan.new_salt_enc  = new_salt_enc;
    plan.new_salt_hmac = new_salt_hmac;
    plan.new_key_hash  = new_key_hash;

    std::weak_ptr<void> alive = _alive;
    BlockingExecutors::encryption()->async(
        [this, alive, tier_id, plan]() {
            if (alive.expired()) return;
            runReencryptionJob(tier_id, plan);
        });
    return true;
}

std::vector<ReencryptTarget> TierEncryptionManager::listReencryptTargets(const std::string& tier_id,
                                                                         ReencryptScope scope) {
    TierEncryptionConfig config;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _tier_configs.find(tier_id);
        if (it == _tier_configs.end()) {
            WarnL << "Tier not found: " << tier_id;
            return {};
        }
        config = it->second;
    }

    std::vector<ReencryptTarget> result;
    if (!_storage.listSegmentFilesByOriginPool) {
        WarnL << "Storage bridge not bound yet, cannot list segments for tier " << tier_id;
        return {};
    }

    std::vector<BridgeSegmentFile> seg_files;
    try {
        seg_files = _storage.listSegmentFilesByOriginPool(tier_id, 0, 0);
    } catch (const std::exception& ex) {
        WarnL << "Could not list segments for tier " << tier_id << ": " << ex.what();
        return {};
    } catch (...) {
        WarnL << "Could not list segments for tier " << tier_id;
        return {};
    }

    result.reserve(seg_files.size());
    for (const auto& sf : seg_files) {
        if (scope == ReencryptScope::kEncryptedOnly) {
            const EncryptionTimeRange* r = findTimeRangeAtTime(config.time_ranges, sf.start_time);
            const bool covered_by_key = (r != nullptr && r->mode != RecordEncryptionMode::kNone);
            if (!covered_by_key) {
                // Looks like footage that was never encrypted. Check the file
                // before skipping: a segment encrypted outside any range must
                // still be processed, otherwise a pass walks past the one file
                // it most needs to fix.
                if (!RecordEncryptionUtils::hasEncryptionTrailer(sf.full_path)) {
                    continue;
                }
            }
        }

        ReencryptTarget t;
        t.camera_id    = sf.camera_id;
        t.stream_id    = sf.stream_id;
        t.segment_path = sf.segment_path;
        t.full_path    = sf.full_path;
        t.start_time   = sf.start_time;
        t.end_time     = sf.end_time;
        t.tier         = sf.tier;
        t.pool_id      = sf.pool_id;
        result.push_back(std::move(t));
    }

    InfoL << "Listed " << result.size() << " segment(s) to re-encrypt for tier(origin_pool)=" << tier_id;
    return result;
}

bool TierEncryptionManager::ensureSegmentReadable(const ReencryptTarget& target, std::string& out_local_path, int timeout_sec, std::string* err) {
    if (!storageReady(err)) return false;
    if (!_storage.resolvePlaybackSegmentPath || !_storage.requestRestoreSegment) {
        if (err) *err = "Storage bridge is missing restore/playback callbacks";
        return false;
    }

    auto resolve = _storage.resolvePlaybackSegmentPath(target.camera_id, target.stream_id,
                                                        target.start_time, target.full_path);

    if (resolve.ready && !resolve.read_path.empty()) {
        out_local_path = resolve.read_path;
        return true;
    }

    if (!resolve.restore_required) {
        if (err) *err = "Segment is not ready for reading: " + resolve.message;
        return false;
    }

    auto restore = _storage.requestRestoreSegment(target.camera_id, target.stream_id,
                                                   target.start_time,
                                                   target.full_path.empty() ? target.segment_path : target.full_path,
                                                   "REENCRYPT");
    if (!restore.handled) {
        if (err) *err = "Failed to trigger restore for segment in COLD tier: " + restore.message;
        return false;
    }

    // Runs on BlockingExecutors::encryption(), a pool meant for long blocking work, so waiting
    // here never holds an EventPoller, an API worker or the storage maintenance thread.
    const int poll_interval_sec = 2;
    int waited = 0;
    while (waited < timeout_sec) {
        std::this_thread::sleep_for(std::chrono::seconds(poll_interval_sec));
        waited += poll_interval_sec;

        if (!restore.job_id.empty() && _storage.getRestoreJobStatus) {
            std::string status;
            if (_storage.getRestoreJobStatus(restore.job_id, status) && status == "FAILED") {
                if (err) *err = "Restore job failed for segment " + target.segment_path;
                return false;
            }
        }

        auto again = _storage.resolvePlaybackSegmentPath(target.camera_id, target.stream_id,
                                                          target.start_time, target.full_path);
        if (again.ready && !again.read_path.empty()) {
            out_local_path = again.read_path;
            return true;
        }
    }

    if (err) *err = "Restore timed out after " + std::to_string(timeout_sec) + "s for segment " + target.segment_path;
    return false;
}

bool TierEncryptionManager::resolveOldKeyForFile(const std::string& tier_id,
                                                 const std::string& local_path,
                                                 int64_t segment_start_time,
                                                 EncryptionTimeRange& out_range) const {
    out_range = EncryptionTimeRange();

    if (!RecordEncryptionUtils::hasEncryptionTrailer(local_path)) {
        // Plaintext. mode stays kNone and the salts stay empty.
        return true;
    }

    // A segment that was mid-recording when the mode or password changed starts
    // inside the previous era, so the first candidate never verifies and every
    // open pays for a full-file HMAC scan before reaching the right key.
    // Remember the answer per file.
    {
        std::lock_guard<std::mutex> lock(_key_cache_mtx);
        auto it = _resolved_key_cache.find(local_path);
        if (it != _resolved_key_cache.end()) {
            out_range = it->second;
            return true;
        }
    }

    for (const auto& candidate : getCandidateTimeRanges(tier_id, segment_start_time)) {
        if (candidate.mode == RecordEncryptionMode::kNone) continue;
        if (candidate.key_hash.empty() || candidate.salt_enc.empty()) continue;

        auto salt_enc  = RecordEncryptionUtils::hexStrToBytes(candidate.salt_enc);
        auto salt_hmac = RecordEncryptionUtils::hexStrToBytes(candidate.salt_hmac);
        if (salt_enc.empty() || salt_hmac.empty()) continue;

        try {
            auto keys = DerivedKeyCache::get(candidate.key_hash, salt_enc, salt_hmac);
            EncryptedFileTrailer trailer;
            int64_t content_len = 0;
            // The HMAC decides which key. Key derivation is cached, so walking a
            // handful of candidates costs almost nothing after the first one.
            if (RecordFileEncryptor::readAndVerifyTrailer(local_path, keys.hmac_key, trailer, content_len)) {
                out_range = candidate;
                // The trailer knows what this file really is. The range only
                // records what the pool was configured to do at that time, and
                // the two drift apart whenever a conversion is interrupted or
                // skipped.
                out_range.mode = trailer.mode;
                {
                    std::lock_guard<std::mutex> lock(_key_cache_mtx);
                    if (_resolved_key_cache.size() >= kResolvedKeyCacheMax) {
                        _resolved_key_cache.clear();
                    }
                    _resolved_key_cache[local_path] = out_range;
                }
                return true;
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    return false;
}


bool TierEncryptionManager::decryptFileToPlain(const std::string& enc_path,
                                                const std::string& out_plain_path,
                                                RecordEncryptionMode old_mode,
                                                const std::string& old_salt_enc,
                                                const std::string& old_salt_hmac,
                                                const std::string& old_key_hash) {
    auto salt_enc_bytes  = RecordEncryptionUtils::hexStrToBytes(old_salt_enc);
    auto salt_hmac_bytes = RecordEncryptionUtils::hexStrToBytes(old_salt_hmac);
    if (salt_enc_bytes.empty() || salt_hmac_bytes.empty()) {
        WarnL << "Malformed old salts, cannot decrypt: " << enc_path;
        return false;
    }
    auto keys = DerivedKeyCache::get(old_key_hash, salt_enc_bytes, salt_hmac_bytes);

    EncryptedFileTrailer trailer;
    int64_t content_len = 0;
    if (!RecordFileEncryptor::readAndVerifyTrailer(enc_path, keys.hmac_key, trailer, content_len)) {
        WarnL << "HMAC verification failed during decryption: " << enc_path;
        return false;
    }

    FILE *in = fopen(enc_path.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(out_plain_path.c_str(), "wb");
    if (!out) { fclose(in); return false; }

    std::vector<uint8_t> buf(1 << 20);
    int64_t remaining = content_len;
    bool ok = true;
    while (remaining > 0 && ok) {
        size_t n = (size_t)std::min<int64_t>((int64_t)buf.size(), remaining);
        if (fread(buf.data(), 1, n, in) != n) { ok = false; break; }
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
        remaining -= (int64_t)n;
    }
    fclose(in);
    fclose(out);
    if (!ok) { remove(out_plain_path.c_str()); return false; }

    const std::vector<uint8_t> &iv = trailer.nonce;

    // The caller passes the mode it believes the file has, but the trailer is
    // authoritative. They disagree whenever a previous conversion was skipped.
    if (trailer.mode != old_mode) {
        WarnL << "Trailer mode (" << (int)trailer.mode << ") differs from expected ("
              << (int)old_mode << "), trusting the file: " << enc_path;
        old_mode = trailer.mode;
    }

    std::vector<ByteRange> regions;
    if (old_mode == RecordEncryptionMode::kStrong) {
        regions.emplace_back(0, content_len);
    } else {
        int64_t moov_size = trailer.moov_end - trailer.moov_start;
        if (moov_size < 8) { remove(out_plain_path.c_str()); return false; }

        FILE *fp = fopen(out_plain_path.c_str(), "rb");
        if (!fp) { remove(out_plain_path.c_str()); return false; }
        std::vector<uint8_t> moov_buf((size_t)moov_size);
        bool rok = (fseek(fp, trailer.moov_start, SEEK_SET) == 0) &&
                   (fread(moov_buf.data(), 1, moov_buf.size(), fp) == moov_buf.size());
        fclose(fp);
        if (!rok) { remove(out_plain_path.c_str()); return false; }

        // Read the real header size from the (unencrypted) box header rather
        // than assuming 8, so a largesize moov is not mangled.
        const size_t hdr = RecordFileEncryptor::boxHeaderSize(moov_buf.data());
        const int64_t moov_content_off = trailer.moov_start + (int64_t)hdr;
        const int64_t moov_content_len = trailer.moov_end - moov_content_off;
        if (moov_content_len <= 0 || (size_t)moov_size < hdr) {
            remove(out_plain_path.c_str());
            return false;
        }

        OffsetKeyedCtrCipher cipher(keys.enc_key, iv);
        auto plain_moov = cipher.xcryptAtOffset(moov_buf.data() + hdr, (size_t)moov_content_len,
                                                 moov_content_off);
        std::memcpy(moov_buf.data() + hdr, plain_moov.data(), plain_moov.size());

        regions = RecordFileEncryptor::buildLightModeRegions(moov_buf, trailer.moov_start, trailer.moov_end);
        if (regions.empty()) { remove(out_plain_path.c_str()); return false; }
    }

    if (!RecordFileEncryptor::xcryptRegionsInPlace(out_plain_path, regions, keys.enc_key, iv)) {
        remove(out_plain_path.c_str());
        return false;
    }
    return true;
}

bool TierEncryptionManager::encryptPlainFile(const std::string& plain_path,
                                              RecordEncryptionMode new_mode,
                                              const std::string& new_salt_enc,
                                              const std::string& new_salt_hmac,
                                              const std::string& new_key_hash) {
    if (new_mode == RecordEncryptionMode::kNone) return true;

    auto salt_enc_bytes  = RecordEncryptionUtils::hexStrToBytes(new_salt_enc);
    auto salt_hmac_bytes = RecordEncryptionUtils::hexStrToBytes(new_salt_hmac);
    if (salt_enc_bytes.empty() || salt_hmac_bytes.empty()) {
        WarnL << "Malformed new salts, refusing to encrypt: " << plain_path;
        return false;
    }
    auto keys = DerivedKeyCache::get(new_key_hash, salt_enc_bytes, salt_hmac_bytes);

    EncryptedFileTrailer trailer;
    std::vector<ByteRange> regions;
    if (new_mode == RecordEncryptionMode::kStrong) {
        regions = RecordFileEncryptor::collectStrongModeRegions(plain_path, trailer);
    } else {
        regions = RecordFileEncryptor::collectLightModeRegions(plain_path, trailer);
    }

    if (regions.empty()) {
        WarnL << "Skipping file without a valid moov box: " << plain_path;
        return false;
    }

    return RecordFileEncryptor::encryptFileInPlace(plain_path, regions, trailer, keys.enc_key, keys.hmac_key);
}

bool TierEncryptionManager::reencryptOneFile(const std::string& file_path, RecordEncryptionMode old_mode,
                          RecordEncryptionMode new_mode, const std::string& old_salt_enc,
                          const std::string& old_salt_hmac, const std::string& new_salt_enc,
                          const std::string& new_salt_hmac, const std::string& old_key_hash,
                          const std::string& new_key_hash) {

    const std::string plain_path  = RecordEncryptionUtils::stripEncSuffix(file_path);
    const std::string enc_path    = plain_path + RecordEncryptionUtils::encryptedFileSuffix();
    const std::string target_path = RecordEncryptionUtils::computeTargetPath(file_path, new_mode);
    const std::string tmp_path    = plain_path + ".reenc.tmp";

    // The caller may hand us the plaintext name even when the file on disk
    // carries the ".enc" suffix, because paths come from the recording index.
    std::string source_path = file_path;
    {
        struct stat st{};
        if (::stat(source_path.c_str(), &st) != 0) {
            const std::string &other = (source_path == plain_path) ? enc_path : plain_path;
            if (::stat(other.c_str(), &st) == 0) {
                source_path = other;
            } else {
                WarnL << "Source file not found in either variant: " << file_path;
                return false;
            }
        }
    }

    if (old_mode == RecordEncryptionMode::kNone) {
        if (!copyFileChunked(source_path, tmp_path)) {
            WarnL << "Failed to copy file: " << source_path;
            return false;
        }
    } else {
        if (!decryptFileToPlain(source_path, tmp_path, old_mode, old_salt_enc, old_salt_hmac, old_key_hash)) {
            remove(tmp_path.c_str());
            return false;
        }
    }

    if (new_mode != RecordEncryptionMode::kNone) {
        if (!encryptPlainFile(tmp_path, new_mode, new_salt_enc, new_salt_hmac, new_key_hash)) {
            remove(tmp_path.c_str());
            return false;
        }
    }

    if (rename(tmp_path.c_str(), target_path.c_str()) != 0) {
        WarnL << "Failed to rename " << tmp_path << " -> " << target_path << ": " << strerror(errno);
        remove(tmp_path.c_str());
        return false;
    }

    // Remove the other spelling of the same segment. Leaving it behind used to
    // double the space used by the pool and confuse path resolution, which
    // prefers the encrypted variant when both exist.
    const std::string &other_path = (target_path == plain_path) ? enc_path : plain_path;
    if (other_path != target_path) {
        remove(other_path.c_str());
    }

    // The file now has a different key or mode, so any remembered answer for
    // either spelling of its name is stale.
    {
        std::lock_guard<std::mutex> lock(_key_cache_mtx);
        _resolved_key_cache.erase(plain_path);
        _resolved_key_cache.erase(enc_path);
    }
    return true;
}

void TierEncryptionManager::runReencryptionJobInternal(const std::string& tier_id, const ReencryptPlan& plan) {

    const int restore_timeout_sec = kReencryptRestoreTimeoutSec;
    const std::string& new_salt_enc  = plan.new_salt_enc;
    const std::string& new_salt_hmac = plan.new_salt_hmac;
    const std::string& new_key_hash  = plan.new_key_hash;

    try {
        auto targets = listReencryptTargets(tier_id, plan.scope);

        {
            std::lock_guard<std::mutex> lock(_progress_mtx);
            _reencrypt_progress[tier_id].total_files = targets.size();
        }

        InfoL << "Re-encrypting tier (origin_pool)=" << tier_id
                << " -> " << (plan.preserve_mode
                                  ? std::string("rekey, per-file mode preserved")
                                  : "mode=" + std::to_string((int)plan.target_mode))
                << ", scope=" << (plan.scope == ReencryptScope::kEverything ? "everything" : "encrypted-only")
                << ", total " << targets.size() << " segment(s)";

        size_t failed = 0;
        // Earliest segment start that is now known to be stored under the new
        // key. Only files that were actually converted (or verified as already
        // converted) count: taking it from the target list meant a failed file
        // still widened the range and claimed coverage it did not have.
        int64_t earliest_processed_start = 0;
        auto noteCovered = [&earliest_processed_start](int64_t start_time) {
            if (start_time > 0 &&
                (earliest_processed_start == 0 || start_time < earliest_processed_start)) {
                earliest_processed_start = start_time;
            }
        };

        for (const auto& target : targets) {
            {
                std::lock_guard<std::mutex> lock(_progress_mtx);
                _reencrypt_progress[tier_id].current_file = target.segment_path;
            }

            bool ok = false;
            try {
                std::string local_path;
                std::string prep_err;
                bool was_cold = (target.tier == "COLD");
                if (!ensureSegmentReadable(target, local_path, restore_timeout_sec, &prep_err)) {
                    WarnL << "Skipping segment: " << target.segment_path << " -- " << prep_err;
                    throw std::runtime_error(prep_err);
                }
                if (was_cold) {
                    std::lock_guard<std::mutex> lock(_progress_mtx);
                    _reencrypt_progress[tier_id].restored_files++;
                }

                // Blocks if the record-finished hook is still working on this
                // exact file, so the two cannot rename it at the same time.
                SegmentGuard guard(_storage, local_path);

                // Resolved from the file itself. Trusting the time range alone
                // meant a segment encrypted outside any range was treated as
                // plaintext: the job then copied the ciphertext through and
                // renamed it, so it stayed encrypted with a plaintext name and
                // no key ever recorded for it.
                EncryptionTimeRange old_range;
                if (!resolveOldKeyForFile(tier_id, local_path, target.start_time, old_range)) {
                    throw std::runtime_error("File is encrypted but no known key verifies it: " + local_path);
                }

                const RecordEncryptionMode old_mode = old_range.mode;
                const std::string old_salt_enc  = old_range.salt_enc;
                const std::string old_salt_hmac = old_range.salt_hmac;
                const std::string old_key_hash  = old_range.key_hash;

                // A password change keeps each file in whatever mode it is
                // already in; only a mode change forces a single target.
                const RecordEncryptionMode new_mode =
                    plan.preserve_mode ? old_mode : plan.target_mode;

                // Comparing modes as well as keys is what stops a mode change
                // with an unchanged password from mistaking every file for one
                // that has already been converted.
                const bool same_key = (old_key_hash == new_key_hash &&
                                       old_salt_enc == new_salt_enc &&
                                       old_salt_hmac == new_salt_hmac);
                if ((old_mode == new_mode) &&
                    (new_mode == RecordEncryptionMode::kNone || same_key)) {
                    noteCovered(target.start_time);
                    std::lock_guard<std::mutex> lock(_progress_mtx);
                    _reencrypt_progress[tier_id].skipped_files++;
                    _reencrypt_progress[tier_id].processed_files++;
                    continue;
                }

                if (!reencryptOneFile(local_path, old_mode, new_mode,
                                       old_salt_enc, old_salt_hmac,
                                       new_salt_enc, new_salt_hmac,
                                       old_key_hash, new_key_hash)) {
                    throw std::runtime_error("reencryptOneFile failed");
                }

                std::string result_path = RecordEncryptionUtils::computeTargetPath(local_path, new_mode);
                std::string replace_err;
                if (!_storage.replaceSegmentInPlace ||
                    !_storage.replaceSegmentInPlace(target.camera_id, target.stream_id, target.segment_path, result_path, &replace_err)) {
                    throw std::runtime_error("replaceSegmentInPlace failed: " + replace_err);
                }

                noteCovered(target.start_time);
                ok = true;
            } catch (const std::exception& ex) {
                ok = false;
                WarnL << "Re-encrypt exception for " << target.segment_path << ": " << ex.what();
            }

            std::lock_guard<std::mutex> lock(_progress_mtx);
            if (ok) {
                _reencrypt_progress[tier_id].processed_files++;
            } else {
                _reencrypt_progress[tier_id].failed_files++;
                failed++;
                WarnL << "Failed to re-encrypt: " << target.segment_path;
            }
        }

        {
            std::lock_guard<std::mutex> lock(_progress_mtx);
            _reencrypt_progress[tier_id].current_file.clear();
            _reencrypt_progress[tier_id].finished_at = (int64_t)time(nullptr);

            if (failed != 0) {
                _reencrypt_progress[tier_id].status = EncryptionJobStatus::kFailed;
                _reencrypt_progress[tier_id].error_msg = std::to_string(failed) + " segment(s) failed -- rerun the job to continue processing (completed files will be skipped automatically)";
                WarnL << "Re-encrypt failed: " << failed << "/" << targets.size();
            } else {
                _reencrypt_progress[tier_id].status = EncryptionJobStatus::kCompleted;
            }

            InfoL << "Re-encrypt finished: "
                  << _reencrypt_progress[tier_id].processed_files << "/"
                  << _reencrypt_progress[tier_id].total_files
                  << " (skipped=" << _reencrypt_progress[tier_id].skipped_files
                  << ", failed=" << _reencrypt_progress[tier_id].failed_files
                  << ", restored_from_cold=" << _reencrypt_progress[tier_id].restored_files << ")";
        }

        // Collapsing the key history is only safe once every file is known to be
        // under the new key. After a partial failure the pool holds a mix of old
        // and new keys at overlapping timestamps, which no single time range can
        // describe; the ranges are left intact and playback resolves such files
        // through the candidate-key fallback (getCandidateTimeRanges).
        if (failed == 0) {
            if (plan.preserve_mode) {
                // Password change: every era keeps its own mode, only the key moves.
                rekeyTimeRanges(tier_id, new_salt_enc, new_salt_hmac, new_key_hash);
            } else {
                collapseTimeRangesAfterModeChange(tier_id, plan.target_mode, new_salt_enc,
                                                   new_salt_hmac, new_key_hash,
                                                   earliest_processed_start);
            }
        } else {
            WarnL << "Leaving encryption time-range history intact for tier " << tier_id
                  << " because " << failed << " segment(s) still hold the previous key";
        }

        if (failed == 0) {
            try {
                reconcileStaleFilesForTier(tier_id);
            } catch (const std::exception& ex) {
                WarnL << "Post-job reconcile failed for tier " << tier_id << ": " << ex.what();
            } catch (...) {
                WarnL << "Post-job reconcile failed for tier " << tier_id;
            }
        } else {
            WarnL << "Skipping post-job reconcile for tier " << tier_id
                  << ": the job failed, so segments finalised during it are still plaintext "
                  << "-- rerun the job to process them";
        }

    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(_progress_mtx);
        auto it = _reencrypt_progress.find(tier_id);
        if (it != _reencrypt_progress.end()) {
            it->second.status = EncryptionJobStatus::kFailed;
            it->second.error_msg = std::string("Unexpected: ") + ex.what();
            it->second.finished_at = (int64_t)time(nullptr);
        }
        WarnL << "Re-encrypt crash: " << ex.what();
    }
}

void TierEncryptionManager::clearResolvedKeyCache() {
    std::lock_guard<std::mutex> lock(_key_cache_mtx);
    _resolved_key_cache.clear();
}

void TierEncryptionManager::rekeyTimeRanges(const std::string& tier_id,
                                            const std::string& new_salt_enc,
                                            const std::string& new_salt_hmac,
                                            const std::string& new_key_hash) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) return;

    TierEncryptionConfig& config = it->second;
    if (config.time_ranges.empty()) return;

    std::vector<EncryptionTimeRange> ranges = config.time_ranges;
    std::sort(ranges.begin(), ranges.end(),
              [](const EncryptionTimeRange& a, const EncryptionTimeRange& b) {
                  return a.start_time < b.start_time;
              });

    // Every era that held a key now holds the new one. Windows and modes are
    // untouched: a pool can legitimately have a Light era and a Strong era, and
    // that boundary is the only record of which files are which.
    for (auto& r : ranges) {
        if (r.mode == RecordEncryptionMode::kNone) continue;
        r.salt_enc = new_salt_enc;
        r.salt_hmac = new_salt_hmac;
        r.key_hash = new_key_hash;
    }

    // Neighbouring eras that have become identical are folded together, so the
    // table does not gain a permanent row for every password change.
    std::vector<EncryptionTimeRange> merged;
    for (auto& r : ranges) {
        if (!merged.empty()) {
            EncryptionTimeRange& prev = merged.back();
            const bool contiguous = (prev.end_time == r.start_time);
            const bool identical = (prev.mode == r.mode && prev.key_hash == r.key_hash &&
                                    prev.salt_enc == r.salt_enc && prev.salt_hmac == r.salt_hmac);
            if (contiguous && identical) {
                prev.end_time = r.end_time;
                continue;
            }
        }
        merged.push_back(r);
    }

    config.time_ranges = merged;
    saveTimeRangesToDatabase(tier_id, config.time_ranges);
    clearResolvedKeyCache();

    InfoL << "Re-keyed " << merged.size() << " encryption time range(s) for tier " << tier_id
          << " (per-era modes preserved)";
}

void TierEncryptionManager::collapseTimeRangesAfterModeChange(const std::string& tier_id, RecordEncryptionMode new_mode,
                                       const std::string& new_salt_enc, const std::string& new_salt_hmac,
                                       const std::string& new_key_hash, int64_t earliest_processed_start) {

    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) return;

    TierEncryptionConfig& config = it->second;

    int64_t min_start = 0;
    bool has_min = false;
    for (const auto& r : config.time_ranges) {
        if (!has_min || r.start_time < min_start) { min_start = r.start_time; has_min = true; }
    }

    if (earliest_processed_start > 0 && (!has_min || earliest_processed_start < min_start)) {
        min_start = earliest_processed_start;
        has_min = true;
    }
    if (!has_min) return;

    EncryptionTimeRange merged(min_start, INT64_MAX,
                                new_salt_enc, new_salt_hmac, new_key_hash, new_mode);

    config.time_ranges.clear();
    config.time_ranges.push_back(merged);

    saveTimeRangesToDatabase(tier_id, config.time_ranges);
    clearResolvedKeyCache();

    InfoL << "Merged the time-range history for tier " << tier_id
        << " into a single range [" << min_start << ", INT64_MAX] with the new key";
}

void TierEncryptionManager::runReencryptionJob(const std::string& tier_id, const ReencryptPlan& plan) {

    try {
        runReencryptionJobInternal(tier_id, plan);
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(_progress_mtx);
        auto it = _reencrypt_progress.find(tier_id);
        if (it != _reencrypt_progress.end()) {
            it->second.status = EncryptionJobStatus::kFailed;
            it->second.error_msg = std::string("Unexpected: ") + ex.what();
            it->second.finished_at = (int64_t)time(nullptr);
        }
        WarnL << "Re-encrypt crash: " << ex.what();
    }
}

bool TierEncryptionManager::updateEncryptOldVideoOption(const std::string& tier_id, bool enable,
                                                         bool trigger_reencrypt, std::string* err) {
    {
        std::string pool_err;
        if (!verifyHotPool(tier_id, &pool_err)) {
            if (err) *err = pool_err;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady(err)) return false;

    if (isAnyJobRunning()) {
        std::string running_tier = getRunningJobTierId();
        if (err) *err = "Another re-encrypt job is running for tier: " + running_tier;
        WarnL << "Reject updateEncryptOldVideoOption: job running for " << running_tier;
        return false;
    }

    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) {
        if (err) *err = "Tier not found";
        return false;
    }

    TierEncryptionConfig& config = it->second;
    if (config.encrypt_old_video == enable) {
        if (err) *err = "encrypt_old_video unchanged";
        return false;
    }

    const int64_t now = (int64_t)time(nullptr);
    config.encrypt_old_video = enable;
    config.updated_at = now;
    RecordEncryptionConfig db_config = config.toRecordConfig();
    RecordEncryptionConfigImp config_repo;
    if (!config_repo.addOrUpdate(db_config)) {
        if (err) *err = "Failed to save config";
        return false;
    }

    // Turning the option on used to change nothing but a database column, so a
    // pool that was created with encrypt_old_video=false and had it enabled
    // later kept all of its pre-existing recordings in plaintext while the API
    // reported success. Enabling it has to schedule the pass that the option
    // describes; the caller suppresses this when it is about to start a job of
    // its own (a mode change), because two jobs cannot run at once.
    const bool want_job = enable && trigger_reencrypt &&
                          config.mode != RecordEncryptionMode::kNone;

    InfoL << "Updated encrypt_old_video for tier " << tier_id << ": " << enable
          << (want_job ? " (re-encrypt job scheduled)" : "");

    if (want_job) {
        // Dispatching without reserving first left a window between this call
        // returning 200 and the worker reaching beginJob(), during which
        // isAnyJobRunning() still said false. A changePassword arriving in that
        // window took the slot and this pass failed with nothing but a log line,
        // while the operator saw a success and assumed the old footage was being
        // encrypted. Reserve here, on the caller's thread, as the mode-change
        // path already does.
        if (!beginJob(tier_id)) {
            if (err) *err = "Another re-encrypt job is running for tier: " + getRunningJobTierId();
            return false;
        }

        ReencryptPlan plan;
        plan.target_mode   = config.mode;
        plan.preserve_mode = false;
        plan.scope         = ReencryptScope::kEverything;
        plan.new_salt_enc  = config.salt_enc;
        plan.new_salt_hmac = config.salt_hmac;
        plan.new_key_hash  = config.getEncryptionPasswordHash();

        std::weak_ptr<void> alive = _alive;
        BlockingExecutors::encryption()->async([this, alive, tier_id, plan]() {
            if (alive.expired()) return;
            int64_t earliest = findEarliestSegmentStart(tier_id);
            if (earliest > 0) extendEarliestRangeStart(tier_id, earliest);
            runReencryptionJob(tier_id, plan);
        });
    }

    // Turning it off deliberately leaves already-encrypted history alone: the
    // option controls whether a future pass reaches back over old recordings,
    // not whether existing ciphertext is reverted. The time ranges still cover
    // that data, so it stays readable.
    return true;
}

bool TierEncryptionManager::updateEncryptionMode(const std::string& tier_id, RecordEncryptionMode new_mode,
                                                const std::string& password, bool trigger_reencrypt, std::string* err) {

    {
        std::string pool_err;
        if (!verifyHotPool(tier_id, &pool_err)) {
            if (err) *err = pool_err;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady(err)) return false;

    if (isAnyJobRunning()) {
        std::string running_tier = getRunningJobTierId();
        if (err) *err = "Another re-encrypt job is running for tier: " + running_tier;
        WarnL << "Reject updateEncryptionMode: job running for " << running_tier;
        return false;
    }

    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) {
        if (err) *err = "Tier not found";
        return false;
    }

    TierEncryptionConfig& config = it->second;

    if (config.mode != RecordEncryptionMode::kNone) {
        if (!RecordEncryptionUtils::verifyPassword(password, config.verify_salt, config.verify_hash)) {
            if (err) *err = "Invalid password";
            return false;
        }
    }

    if (config.mode == new_mode) {
        if (err) *err = "Mode unchanged";
        return false;
    }

    RecordEncryptionMode old_mode = config.mode;
    std::string old_salt_enc = config.salt_enc;
    std::string old_salt_hmac = config.salt_hmac;
    std::string current_key_hash = config.getEncryptionPasswordHash();

    const int64_t now = (int64_t)time(nullptr);

    // encrypt_old_video decides how far back a mode change reaches. With it
    // off, the new mode applies only to recordings made from now on: the range
    // is closed here and a new one opened, and existing files keep the mode
    // recorded in their own trailers. With it on, everything is rewritten.
    //
    // Skipping the pass is not just an optimisation. Rewriting every segment
    // means restoring cold storage and re-encrypting tens of thousands of
    // files, so making it opt-in removes the system's heaviest operation from
    // the common path.
    const bool want_job = trigger_reencrypt && config.encrypt_old_video &&
                          (old_mode != RecordEncryptionMode::kNone ||
                           new_mode != RecordEncryptionMode::kNone);

    // Reserve the job slot before writing anything. Doing it afterwards meant a
    // refused job left the configuration already switched to the new mode with
    // no pass over the existing files.
    bool job_reserved = false;
    if (want_job) {
        if (!beginJob(tier_id)) {
            if (err) *err = "Failed to begin job (another job running)";
            return false;
        }
        job_reserved = true;
    }
    // Releases the reservation on any early return below.
    auto release_job = [&]() {
        if (job_reserved) {
            std::lock_guard<std::mutex> plock(_progress_mtx);
            _reencrypt_progress.erase(tier_id);
            job_reserved = false;
        }
    };

    RecordEncryptionTierRangeImp time_range_repo;
    if (old_mode != RecordEncryptionMode::kNone) {
        if (!time_range_repo.closeOpenRange(tier_id, now)) {
            release_job();
            if (err) *err = "Failed to close open range";
            return false;
        }
        for (auto& range : config.time_ranges) {
            if (range.end_time == INT64_MAX) {
                range.end_time = now;
                break;
            }
        }
    }
    if (new_mode != RecordEncryptionMode::kNone) {
        EncryptionTimeRange new_range(now, INT64_MAX, config.salt_enc, config.salt_hmac, current_key_hash, new_mode);
        RecordEncryptionTierRange db_range = new_range.toDbEntity(tier_id);
        if (!time_range_repo.addTimeRange(db_range)) {
            release_job();
            if (err) *err = "Failed to add time range";
            return false;
        }
        config.time_ranges.push_back(new_range);
    }

    config.mode = new_mode;
    config.updated_at = now;

    RecordEncryptionConfig db_config = config.toRecordConfig();
    RecordEncryptionConfigImp config_repo;
    if (!config_repo.addOrUpdate(db_config)) {
        release_job();
        if (err) *err = "Failed to save config";
        return false;
    }

    if (job_reserved) {
        std::string new_salt_enc = config.salt_enc;
        std::string new_salt_hmac = config.salt_hmac;
        bool cover_old = config.encrypt_old_video && new_mode != RecordEncryptionMode::kNone;

        std::weak_ptr<void> alive = _alive;
        BlockingExecutors::encryption()->async(
            [this, alive, tier_id, new_mode, new_salt_enc, new_salt_hmac, current_key_hash, cover_old]() {
                if (alive.expired()) return;
                if (cover_old) {
                    int64_t earliest = findEarliestSegmentStart(tier_id);
                    if (earliest > 0) extendEarliestRangeStart(tier_id, earliest);
                }
                ReencryptPlan plan;
                plan.target_mode   = new_mode;
                plan.preserve_mode = false;
                plan.scope         = ReencryptScope::kEverything;
                plan.new_salt_enc  = new_salt_enc;
                plan.new_salt_hmac = new_salt_hmac;
                plan.new_key_hash  = current_key_hash;
                runReencryptionJob(tier_id, plan);
            });
    }

    InfoL << "[TierEncryptionManager] Updated mode for tier " << tier_id 
          << ": " << static_cast<int>(old_mode) << " -> " << static_cast<int>(new_mode);
    return true;
}

bool TierEncryptionManager::updateEncryptionSalts(const std::string& tier_id, const std::string& old_password,
                               const std::string& new_password, bool trigger_reencrypt, std::string* err) {
    {
        std::string pool_err;
        if (!verifyHotPool(tier_id, &pool_err)) {
            if (err) *err = pool_err;
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!reposReady(err)) return false;

    if (isAnyJobRunning()) {
        std::string running_tier = getRunningJobTierId();
        if (err) *err = "Another re-encrypt job is running for tier: " + running_tier;
        WarnL << "Reject updateEncryptionSalts: job running for " << running_tier;
        return false;
    }

    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) {
        if (err) *err = "Tier not found";
        return false;
    }

    TierEncryptionConfig& config = it->second;

    if (config.mode != RecordEncryptionMode::kNone) {
        if (!RecordEncryptionUtils::verifyPassword(old_password, config.verify_salt, config.verify_hash)) {
            if (err) *err = "Invalid old password";
            return false;
        }
    }

    std::string old_key_hash = config.getEncryptionPasswordHash();

    std::string new_salt = RecordEncryptionUtils::makeRandomHex(16);
    std::string new_hashed = RecordEncryptionUtils::computeVerificationHash(new_password, new_salt);
    std::string new_key_hash = new_hashed.substr(0, std::min(new_hashed.size(), (size_t)32));

    const int64_t now = (int64_t)time(nullptr);
    std::string old_salt_enc = config.salt_enc;
    std::string old_salt_hmac = config.salt_hmac;
    RecordEncryptionMode current_mode = config.mode;

    const bool want_job = trigger_reencrypt && current_mode != RecordEncryptionMode::kNone;
    bool job_reserved = false;
    if (want_job) {
        if (!beginJob(tier_id)) {
            if (err) *err = "Failed to begin job (another job running)";
            return false;
        }
        job_reserved = true;
    }
    auto release_job = [&]() {
        if (job_reserved) {
            std::lock_guard<std::mutex> plock(_progress_mtx);
            _reencrypt_progress.erase(tier_id);
            job_reserved = false;
        }
    };

    RecordEncryptionTierRangeImp time_range_repo;
    if (current_mode != RecordEncryptionMode::kNone) {
        if (!time_range_repo.closeOpenRange(tier_id, now)) {
            release_job();
            if (err) *err = "Failed to close open range";
            return false;
        }

        EncryptionTimeRange new_range(now, INT64_MAX, config.salt_enc, config.salt_hmac, new_key_hash, current_mode);
        RecordEncryptionTierRange db_range = new_range.toDbEntity(tier_id);
        if (!time_range_repo.addTimeRange(db_range)) {
            release_job();
            if (err) *err = "Failed to add time range";
            return false;
        }

        for (auto& range : config.time_ranges) {
            if (range.end_time == INT64_MAX) {
                range.end_time = now;
                break;
            }
        }
        config.time_ranges.push_back(new_range);
    }

    config.verify_salt = new_salt;
    config.verify_hash = new_hashed;
    config.updated_at = now;

    RecordEncryptionConfig db_config = config.toRecordConfig();
    RecordEncryptionConfigImp config_repo;
    if (!config_repo.addOrUpdate(db_config)) {
        release_job();
        if (err) *err = "Failed to save config";
        return false;
    }

    if (job_reserved) {
        std::string new_salt_enc = config.salt_enc;
        std::string new_salt_hmac = config.salt_hmac;
        bool cover_old = config.encrypt_old_video;

        std::weak_ptr<void> alive = _alive;
        BlockingExecutors::encryption()->async(
            [this, alive, tier_id, current_mode, new_salt_enc, new_salt_hmac, new_key_hash, cover_old]() {
                if (alive.expired()) return;
                if (cover_old) {
                    int64_t earliest = findEarliestSegmentStart(tier_id);
                    if (earliest > 0) extendEarliestRangeStart(tier_id, earliest);
                }
                ReencryptPlan plan;
                plan.preserve_mode = true;
                plan.scope         = ReencryptScope::kEncryptedOnly;
                plan.new_salt_enc  = new_salt_enc;
                plan.new_salt_hmac = new_salt_hmac;
                plan.new_key_hash  = new_key_hash;
                runReencryptionJob(tier_id, plan);
            });
    }

    InfoL << "[TierEncryptionManager] Updated password for tier " << tier_id;
    return true;
}

static constexpr int64_t kDecryptCacheTtlSec = 300;
static std::string getDecryptRootPath() {
    GET_CONFIG(std::string, decrypt_save_path, Storage::kDecryptSavePath);
    return File::absolutePath("", decrypt_save_path);
}

static std::string buildDecryptedSegmentPath(const std::string &camera_id,
                                             const std::string &stream_id,
                                             const std::string &source_path) {
    auto root = getDecryptRootPath();
    if (root.empty() || camera_id.empty() || stream_id.empty() || source_path.empty())
        return "";
    if (root.back() == '/')
        root.pop_back();
    auto slash = source_path.find_last_of('/');
    std::string name = slash == std::string::npos ? source_path : source_path.substr(slash + 1);
    // The recording file name already carries its timestamp, so it is unique
    // within a stream and no date directory is needed here.
    name = RecordEncryptionUtils::stripEncSuffix(name);
    if (name.empty())
        return "";
    return root + "/" + camera_id + "/" + stream_id + "/" + name;
}

bool TierEncryptionManager::encryptionInUse() const {
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& kv : _tier_configs) {
        if (kv.second.mode != RecordEncryptionMode::kNone)
            return true;

        for (const auto& r : kv.second.time_ranges) {
            if (r.mode != RecordEncryptionMode::kNone && !r.key_hash.empty())
                return true;
        }
    }
    return false;
}

bool TierEncryptionManager::isSegmentPendingEncryption(const std::string& origin_pool_id,
                                                        const std::string& local_path,
                                                        int64_t segment_start_time) const {
    if (origin_pool_id.empty() || local_path.empty() || !encryptionInUse())
        return false;
    // Already encrypted, or gone; either way there is nothing to wait for.
    if (RecordEncryptionUtils::hasEncryptionTrailer(local_path))
        return false;
    struct stat st{};
    if (::stat(local_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    return !getCandidateTimeRanges(origin_pool_id, segment_start_time).empty();
}

void TierEncryptionManager::maybeSweepDecryptCache() {
    const int64_t now = (int64_t)time(nullptr);
    int64_t next = _decrypt_sweep_at.load(std::memory_order_relaxed);
    if (now < next)
        return;
    if (!_decrypt_sweep_at.compare_exchange_strong(next, now + kDecryptSweepIntervalSec))
        return;
    sweepDecryptCache();
}

std::string TierEncryptionManager::materializeReadablePathForFile(const std::string &record_path,
                                                                std::string *err) {
    if (record_path.empty() || !encryptionInUse())
        return record_path;

    GET_CONFIG(std::string, mp4_save_path, Protocol::kMP4SavePath);
    GET_CONFIG(std::string, app_name, Record::kAppName);
    const std::string rec_root = File::absolutePath(app_name, mp4_save_path);
    if (rec_root.empty() || !start_with(record_path, rec_root))
        return record_path;   // not a managed recording

    std::string rel = record_path.substr(rec_root.size());
    if (!rel.empty() && rel.front() == '/')
        rel.erase(0, 1);
    const auto first = rel.find('/');
    if (first == std::string::npos)
        return record_path;
    const auto second = rel.find('/', first + 1);
    if (second == std::string::npos)
        return record_path;

    const std::string camera_id = rel.substr(0, first);
    const std::string stream_id = rel.substr(first + 1, second - first - 1);
    const int64_t start_time = RecordEncryptionUtils::parseRecordingTimestamp(record_path);
    if (start_time <= 0)
        return record_path;

    // The index always names the plaintext file; the encrypted one is next to it.
    const std::string on_disk = _storage.resolveRecordFilePath
                                     ? _storage.resolveRecordFilePath(record_path)
                                     : record_path;
    return materializeReadablePath(camera_id, stream_id, start_time, on_disk, err);
}

std::string TierEncryptionManager::materializeReadablePath(const std::string &camera_id,
                                                        const std::string &stream_id,
                                                        int64_t segment_start_time,
                                                        const std::string &local_path,
                                                        std::string *err) {
    if (local_path.empty() || !encryptionInUse())
        return local_path;
    if (!RecordEncryptionUtils::hasEncryptionTrailer(local_path))
        return local_path;

    const std::string plain_path = buildDecryptedSegmentPath(camera_id, stream_id, local_path);
    if (plain_path.empty()) {
        if (err) *err = "Cannot derive a decrypt cache path for " + local_path;
        return "";
    }

    SegmentGuard lock(_storage, plain_path);

    struct stat cached{};
    struct stat source{};
    if (::stat(plain_path.c_str(), &cached) == 0 && S_ISREG(cached.st_mode) && cached.st_size > 0 &&
        ::stat(local_path.c_str(), &source) == 0 && cached.st_mtime >= source.st_mtime) {
        ::utimensat(AT_FDCWD, plain_path.c_str(), nullptr, 0);
        return plain_path;
    }

    const auto slash = plain_path.rfind('/');
    if (slash != std::string::npos) {
        const std::string dir = plain_path.substr(0, slash + 1);
        if (!File::create_path(dir, 0700)) {
            if (err) *err = "Cannot create decrypt cache directory: " + dir;
            return "";
        }
    }

    const std::string origin_pool_id = _storage.getSegmentOriginPoolId
                                            ? _storage.getSegmentOriginPoolId(camera_id, stream_id, segment_start_time)
                                            : std::string();
    std::string decrypt_err;
    bool written = false;
    if (!decryptSegmentToFile(origin_pool_id, local_path, segment_start_time,
                              plain_path, &written, &decrypt_err)) {
        if (err) *err = decrypt_err;
        WarnL << "Decrypt cache: " << decrypt_err;
        return "";
    }
    if (!written) {
        return local_path;
    }
    DebugL << "Decrypted segment into cache: " << plain_path;

    maybeSweepDecryptCache();
    return plain_path;
}

size_t TierEncryptionManager::sweepDecryptCache(bool remove_all) {
    const std::string root = getDecryptRootPath();
    if (root.empty())
        return 0;

    const int64_t now = static_cast<int64_t>(time(nullptr));
    size_t removed = 0;
    File::scanDir(root, [&](const std::string &path, bool is_dir) {
        if (is_dir)
            return true;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0)
            return true;

        if (!remove_all && now - static_cast<int64_t>(st.st_mtime) < kDecryptCacheTtlSec)
            return true;
        if (!_storage.isSegmentFileBusy || _storage.isSegmentFileBusy(path))
            return true;
        if (::unlink(path.c_str()) == 0)
            ++removed;
        return true;
    }, true);

    if (removed) {
        InfoL << "Decrypt cache: removed " << removed << " plaintext file(s) from " << root;
    }
    return removed;
}

bool TierEncryptionManager::decryptSegmentToFile(const std::string& origin_pool_id,
                                                 const std::string& enc_path,
                                                 int64_t segment_start_time,
                                                 const std::string& out_plain_path,
                                                 bool* out_written,
                                                 std::string* err) {
    if (out_written) *out_written = false;

    if (!RecordEncryptionUtils::hasEncryptionTrailer(enc_path)) {
        // Not encrypted. Nothing to materialise; the caller keeps using the
        // original path.
        return true;
    }
    if (origin_pool_id.empty()) {
        if (err) *err = "Encrypted segment has no originating pool: " + enc_path;
        return false;
    }

    EncryptionTimeRange range;
    if (!resolveOldKeyForFile(origin_pool_id, enc_path, segment_start_time, range) ||
        range.mode == RecordEncryptionMode::kNone) {
        if (err) *err = "No known key decrypts " + enc_path;
        return false;
    }

    const std::string tmp_path = out_plain_path + ".part";
    remove(tmp_path.c_str());
    if (!decryptFileToPlain(enc_path, tmp_path, range.mode, range.salt_enc,
                            range.salt_hmac, range.key_hash)) {
        remove(tmp_path.c_str());
        if (err) *err = "Failed to decrypt " + enc_path;
        return false;
    }
    if (rename(tmp_path.c_str(), out_plain_path.c_str()) != 0) {
        const std::string reason = strerror(errno);
        remove(tmp_path.c_str());
        if (err) *err = "Failed to publish decrypted segment: " + reason;
        return false;
    }

    if (out_written) *out_written = true;
    return true;
}


EncryptionTimeRange TierEncryptionManager::getTimeRangeAtTime(const std::string& tier_id, int64_t timestamp) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) return EncryptionTimeRange();
    const EncryptionTimeRange* range = findTimeRangeAtTime(it->second.time_ranges, timestamp);
    return range ? *range : EncryptionTimeRange();
}

std::vector<EncryptionTimeRange> TierEncryptionManager::getCandidateTimeRanges(const std::string& tier_id,
                                                                               int64_t timestamp) const {
    std::vector<EncryptionTimeRange> out;
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tier_configs.find(tier_id);
    if (it == _tier_configs.end()) return out;

    const auto& ranges = it->second.time_ranges;
    const EncryptionTimeRange* exact = findTimeRangeAtTime(ranges, timestamp);
    if (exact && exact->mode != RecordEncryptionMode::kNone && !exact->key_hash.empty()) {
        out.push_back(*exact);
    }

    // A re-encryption job that stopped halfway leaves files from the same
    // second under two different keys, which a purely time-based lookup cannot
    // express. Offering the other known keys as fallbacks lets playback recover
    // instead of failing the segment; the HMAC check decides which one is right,
    // and key derivation is cached so the retries are cheap.
    std::vector<EncryptionTimeRange> rest;
    for (const auto& r : ranges) {
        if (!out.empty() && r.start_time == out.front().start_time) continue;
        if (r.mode == RecordEncryptionMode::kNone) continue;
        if (r.key_hash.empty() || r.salt_enc.empty()) continue;
        rest.push_back(r);
    }
    // Most recent first: after a password change the new key is the likely one.
    std::sort(rest.begin(), rest.end(), [](const EncryptionTimeRange& a, const EncryptionTimeRange& b) {
        return a.start_time > b.start_time;
    });
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

ReencryptProgress TierEncryptionManager::getReencryptProgress(const std::string& tier_id) const {
    std::lock_guard<std::mutex> lock(_progress_mtx);
    auto it = _reencrypt_progress.find(tier_id);
    if (it == _reencrypt_progress.end()) {
        ReencryptProgress progress;
        progress.tier_id = tier_id;
        progress.status = EncryptionJobStatus::kIdle;
        return progress;
    }
    return it->second;
}

void TierEncryptionManager::clearReencryptProgress(const std::string& tier_id) {
    std::lock_guard<std::mutex> lock(_progress_mtx);
    _reencrypt_progress.erase(tier_id);
}

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE
