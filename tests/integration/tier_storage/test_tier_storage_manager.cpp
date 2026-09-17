#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utime.h>
#include <unistd.h>
#include <ftw.h>

#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Camera/CameraStatistic.h"
#include "Local/SearchEngine.h"
#include "Local/StatisticRecorder.h"
#include "Local/TierStorageManager.h"
#include "Local/TimeMuxer.h"
#include "Local/TimeQuery.h"
#include "Storage/DbStorage.h"
#include "Storage/Bookmark.h"
#include "Storage/ExtractJob.h"
#include "Storage/MigrationHistory.h"
#include "Storage/MiscData.h"
#include "Storage/StorageTierExtra.h"
#include "Storage/TierBackfillTask.h"
#include "Storage/TieringJob.h"
#include "Util/File.h"
#include "Util/base64.h"
#include "Util/mini.h"
#include "Thread/WorkThreadPool.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

namespace {

std::string g_executable;

const char *envValue(const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] ? value : nullptr;
}

bool hasMinioEnvironment() {
    return envValue("TIER_TEST_S3_ENDPOINT") &&
           envValue("TIER_TEST_S3_ACCESS_KEY") &&
           envValue("TIER_TEST_S3_SECRET_KEY");
}

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

void makeDirectory(const std::string &path) {
    std::string target = path;
    if (target.empty() || target.back() != '/')
        target.push_back('/');
    require(File::create_path(target, 0755), "Cannot create directory: " + path);
}

int removeEntry(const char *path, const struct stat *, int, struct FTW *) {
    return std::remove(path);
}

void removeTree(const std::string &root) {
    if (root.find("/tmp/s3mediakit-tier-manager-") != 0)
        return;
    ::nftw(root.c_str(), removeEntry, 32, FTW_DEPTH | FTW_PHYS);
}

std::string makeTempRoot() {
    char path[] = "/tmp/s3mediakit-tier-manager-XXXXXX";
    char *created = ::mkdtemp(path);
    require(created != nullptr, "mkdtemp failed");
    return created;
}

void configureProcess(const std::string &root) {
    makeDirectory(root + "/db");
    makeDirectory(root + "/hot/record");
    makeDirectory(root + "/restore");
    mINI::Instance()[Database::kDbSavePath] = root + "/db";
    mINI::Instance()[Database::kESCMigrationSavePath] = TIER_TEST_ESC_MIGRATION_DIR;
    mINI::Instance()[Database::kMServerMigrationSavePath] = TIER_TEST_MIGRATION_DIR;
    mINI::Instance()[Database::kKeepMigrationSqlFiles] = true;
    mINI::Instance()[General::kMediaServerId] = "tier-manager-test";
    mINI::Instance()[Protocol::kMP4SavePath] = root + "/hot";
    mINI::Instance()[Record::kAppName] = "record";
    mINI::Instance()[Storage::kRestoreSavePath] = root + "/restore";
    mINI::Instance()[Storage::kTierRangeBackfillEnabled] = 0;
    mINI::Instance()[Storage::kTierTmpCleanupEnabled] = 0;
    mINI::Instance()[Storage::kPoolHealthCheckIntervalSeconds] = 3600;
    mINI::Instance()[Storage::kPoolHealthFailureThreshold] = 2;
    mINI::Instance()[Storage::kPoolHealthRecoveryThreshold] = 2;
    mINI::Instance()[Storage::kPoolHealthStaleAfterSeconds] = 90;
    mINI::Instance()[Storage::kPoolHealthMetricSnapshotIntervalSeconds] = 300;
}

void migrateDatabase(const std::string &root) {
    configureProcess(root);
    MigrationHistoryImp migration(Database::kMediaServerDb);
    migration.migrate(TIER_TEST_MIGRATION_DIR);
    require(!SqlitePoolMap::Instance().getSavePath(Database::kMediaServerDb).empty(),
            "MediaServer SQLite pool was not initialized");
}

void migrateEscDatabase(const std::string &root) {
    configureProcess(root);
    MigrationHistoryImp migration(Database::kEdgeStorageControllerDb);
    migration.migrate(TIER_TEST_ESC_MIGRATION_DIR);
    require(!SqlitePoolMap::Instance().getSavePath(Database::kEdgeStorageControllerDb).empty(),
            "ESC SQLite pool was not initialized");
}

void writeFile(const std::string &path, const std::string &payload) {
    makeDirectory(File::parentDir(path));
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    require(output.good(), "Cannot create segment: " + path);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    require(output.good(), "Cannot write segment: " + path);
}

void setFileModifiedTime(const std::string &path, time_t modified_at) {
    struct utimbuf times{};
    times.actime = modified_at;
    times.modtime = modified_at;
    require(::utime(path.c_str(), &times) == 0, "Cannot update file timestamp: " + path);
}

void configureTemporaryMp4Cleanup(bool dry_run) {
    mINI::Instance()[Storage::kTierTmpCleanupEnabled] = 1;
    mINI::Instance()[Storage::kTierTmpCleanupDryRun] = dry_run ? 1 : 0;
    mINI::Instance()[Storage::kTierTmpCleanupMaxFilesPerCycle] = 1000;
    mINI::Instance()[Storage::kTierTmpCleanupWorkBudgetMs] = 5000;
}

void writeTimeBlockAt(const std::string &record_app_path,
                      const std::string &camera,
                      const std::string &stream,
                      const std::string &segment_path,
                      int64_t start,
                      int64_t end,
                      int64_t size) {
    const std::string camera_dir = record_app_path + "/" + camera;
    makeDirectory(camera_dir);
    TimeMuxer muxer;
    muxer.openFile(camera_dir + "/tier-test.s3db");
    TimeBlock block;
    block.set_app(camera);
    block.set_stream(stream);
    block.set_start_time(static_cast<uint64_t>(start));
    block.set_time_len(static_cast<uint32_t>(end - start));
    block.set_file_size(static_cast<uint64_t>(size));
    block.set_file_path(encodeBase64(segment_path));
    require(muxer.inputBlock(block), "Cannot write time block");
    muxer.closeFile();
}

void writeTimeBlock(const std::string &root,
                    const std::string &camera,
                    const std::string &stream,
                    const std::string &segment_path,
                    int64_t start,
                    int64_t end,
                    int64_t size) {
    writeTimeBlockAt(root + "/hot/record", camera, stream, segment_path,
                     start, end, size);
}

void writeDailyTimeBlock(const std::string &root,
                         const std::string &camera,
                         const std::string &stream,
                         const std::string &date,
                         const std::string &segment_path,
                         int64_t start,
                         int64_t end,
                         int64_t size) {
    const std::string camera_dir = root + "/hot/record/" + camera;
    makeDirectory(camera_dir);
    TimeMuxer muxer;
    muxer.openFile(camera_dir + "/" + date + ".s3db");
    TimeBlock block;
    block.set_app(camera);
    block.set_stream(stream);
    block.set_start_time(static_cast<uint64_t>(start));
    block.set_time_len(static_cast<uint32_t>(end - start));
    block.set_file_size(static_cast<uint64_t>(size));
    block.set_file_path(encodeBase64(segment_path));
    require(muxer.inputBlock(block), "Cannot write daily time block");
    muxer.closeFile();
}

StoragePool makeNasPool(const std::string &root) {
    makeDirectory(root + "/nas/record");
    StoragePool pool;
    pool.id = "test-nas";
    pool.name = "Test NAS";
    pool.type = poolTypeToString(StoragePoolType::NAS);
    pool.tier = tierTypeToString(WarmTier);
    pool.mount_path = Optional<std::string>(root + "/nas");
    pool.enabled = 1;
    pool.health_check_enabled = 0;
    return pool;
}

StoragePool makeHotPool(const std::string &root,
                        const std::string &id,
                        const std::string &directory) {
    makeDirectory(root + "/" + directory + "/record");
    StoragePool pool;
    pool.id = id;
    pool.name = "Test HOT " + id;
    pool.type = poolTypeToString(StoragePoolType::LOCAL_DISK);
    pool.tier = tierTypeToString(HotTier);
    pool.mount_path = Optional<std::string>(root + "/" + directory);
    pool.enabled = 1;
    pool.health_check_enabled = 0;
    return pool;
}

StoragePolicy makeHotOnlyPolicy(const std::string &hot_pool_id,
                                const std::string &policy_id,
                                int retain_until_days = 3650) {
    PolicyTierConfig hot;
    hot.tier = tierTypeToString(HotTier);
    hot.enabled = true;
    hot.pool_id = hot_pool_id;
    hot.retain_until_days = retain_until_days;
    hot.overflow_action = overflowActionToString(OverflowAction::DELETE_OLDEST);

    StoragePolicy policy;
    policy.id = policy_id;
    policy.name = "Test HOT-only policy";
    policy.enabled = 1;
    policy.tiers_json = StoragePolicy::tiersToJson({hot});
    policy.delete_policy_json = PolicyDeleteConfig().toJson().toStyledString();
    policy.advanced_rules_json = PolicyAdvancedRules().toJson().toStyledString();
    return policy;
}

StoragePool makeObjectPool(const std::string &root,
                           const std::string &endpoint,
                           const std::string &access_key,
                           const std::string &secret_key,
                           const std::string &bucket) {
    StoragePool pool;
    pool.id = "test-minio";
    pool.name = "Test MinIO";
    pool.type = poolTypeToString(StoragePoolType::MINIO);
    pool.tier = tierTypeToString(ColdTier);
    pool.endpoint = Optional<std::string>(endpoint);
    pool.bucket = Optional<std::string>(bucket);
    pool.base_path = Optional<std::string>("agent-runs/tier-manager-" + root.substr(root.rfind('/') + 1));
    pool.access_key = Optional<std::string>(access_key);
    pool.secret_key_enc = Optional<std::string>(secret_key);
    pool.enabled = 1;
    pool.health_check_enabled = 0;
    return pool;
}

StoragePool makeMinioPool(const std::string &root) {
    require(hasMinioEnvironment(), "MinIO environment is unavailable");
    const char *bucket = envValue("TIER_TEST_S3_BUCKET");
    return makeObjectPool(root,
                          envValue("TIER_TEST_S3_ENDPOINT"),
                          envValue("TIER_TEST_S3_ACCESS_KEY"),
                          envValue("TIER_TEST_S3_SECRET_KEY"),
                          bucket ? bucket : "s3mediakit-tier-test");
}

class ObjectCleanup {
public:
    ObjectCleanup(const StoragePool &pool, const std::string &key)
        : _pool(pool), _key(key) {
        _registered = _storage.registerPool(
            pool.id, pool.type, pool.endpoint.value(), pool.bucket.value(),
            pool.base_path.value_or(""), pool.access_key.value(), pool.secret_key_enc.value());
    }

    ~ObjectCleanup() {
        if (_registered && !_key.empty())
            _storage.deleteSegment(_pool.id, _key);
    }

    TierObjectStorage &storage() { return _storage; }

private:
    StoragePool _pool;
    std::string _key;
    TierObjectStorage _storage;
    bool _registered = false;
};

StoragePolicy makeMovePolicy(const std::string &hot_pool_id, const std::string &nas_pool_id) {
    PolicyTierConfig hot;
    hot.tier = tierTypeToString(HotTier);
    hot.enabled = true;
    hot.pool_id = hot_pool_id;
    hot.retain_until_days = 0;
    hot.overflow_action = overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER);

    PolicyTierConfig warm;
    warm.tier = tierTypeToString(WarmTier);
    warm.enabled = true;
    warm.pool_id = nas_pool_id;
    warm.retain_until_days = 3650;
    warm.overflow_action = overflowActionToString(OverflowAction::DELETE_OLDEST);

    PolicyAdvancedRules rules;
    rules.enable_early_move_when_pool_high = false;
    rules.min_segment_age_minutes_before_move = 0;

    StoragePolicy policy;
    policy.id = "test-move-policy";
    policy.name = "Test move policy";
    policy.enabled = 1;
    policy.tiers_json = StoragePolicy::tiersToJson({hot, warm});
    policy.delete_policy_json = PolicyDeleteConfig().toJson().toStyledString();
    policy.advanced_rules_json = rules.toJson().toStyledString();
    return policy;
}

StoragePolicy makeColdMovePolicy(const std::string &hot_pool_id, const std::string &cold_pool_id) {
    PolicyTierConfig hot;
    hot.tier = tierTypeToString(HotTier);
    hot.enabled = true;
    hot.pool_id = hot_pool_id;
    hot.retain_until_days = 0;
    hot.overflow_action = overflowActionToString(OverflowAction::MOVE_TO_NEXT_TIER);

    PolicyTierConfig cold;
    cold.tier = tierTypeToString(ColdTier);
    cold.enabled = true;
    cold.pool_id = cold_pool_id;
    cold.retain_until_days = 3650;
    cold.overflow_action = overflowActionToString(OverflowAction::DELETE_OLDEST);

    PolicyAdvancedRules rules;
    rules.enable_early_move_when_pool_high = false;
    rules.min_segment_age_minutes_before_move = 0;

    StoragePolicy policy;
    policy.id = "test-cold-policy";
    policy.name = "Test cold policy";
    policy.enabled = 1;
    policy.tiers_json = StoragePolicy::tiersToJson({hot, cold});
    policy.delete_policy_json = PolicyDeleteConfig().toJson().toStyledString();
    policy.advanced_rules_json = rules.toJson().toStyledString();
    return policy;
}

StoragePolicy makeRetentionPolicy(const std::string &hot_pool_id,
                                  const std::string &policy_id,
                                  bool require_approval) {
    PolicyTierConfig hot;
    hot.tier = tierTypeToString(HotTier);
    hot.enabled = true;
    hot.pool_id = hot_pool_id;
    hot.retain_until_days = 3650;
    hot.overflow_action = overflowActionToString(OverflowAction::DELETE_OLDEST);

    PolicyDeleteConfig deletion;
    deletion.delete_after_days = 1;
    deletion.delete_mode = require_approval
        ? deleteModeToString(DeleteMode::MARK_EXPIRED_WAIT_APPROVAL)
        : deleteModeToString(DeleteMode::DELETE_AUTOMATICALLY);
    deletion.require_approval_before_delete = require_approval;

    PolicyAdvancedRules rules;
    rules.enable_early_move_when_pool_high = false;

    StoragePolicy policy;
    policy.id = policy_id;
    policy.name = "Test retention policy";
    policy.enabled = 1;
    policy.tiers_json = StoragePolicy::tiersToJson({hot});
    policy.delete_policy_json = deletion.toJson().toStyledString();
    policy.advanced_rules_json = rules.toJson().toStyledString();
    return policy;
}

bool waitUntil(const std::function<bool()> &predicate, int timeout_ms) {
    const int interval_us = 20000;
    for (int elapsed = 0; elapsed < timeout_ms * 1000; elapsed += interval_us) {
        if (predicate())
            return true;
        ::usleep(interval_us);
    }
    return predicate();
}

void drainWorkThreadPool() {
    WorkThreadPool::Instance().for_each([](const TaskExecutor::Ptr &executor) {
        executor->sync([]() {});
    });
}

void runTemporaryMp4CleanupPass(TierStorageManager &manager);

class SlowS3Server {
public:
    SlowS3Server() {
        _listener = ::socket(AF_INET, SOCK_STREAM, 0);
        require(_listener >= 0, "Cannot create local S3 test socket");
        int reuse = 1;
        ::setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(::bind(_listener, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == 0,
                "Cannot bind local S3 test socket");
        require(::listen(_listener, 16) == 0, "Cannot listen on local S3 test socket");
        socklen_t length = sizeof(address);
        require(::getsockname(_listener, reinterpret_cast<struct sockaddr *>(&address), &length) == 0,
                "Cannot resolve local S3 test port");
        _port = ntohs(address.sin_port);
        _thread = std::thread([this]() { acceptLoop(); });
    }

    ~SlowS3Server() {
        _stopping = true;
        ::shutdown(_listener, SHUT_RDWR);
        ::close(_listener);
        if (_thread.joinable())
            _thread.join();
        for (auto &worker : _workers) {
            if (worker.joinable())
                worker.join();
        }
    }

    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(_port);
    }

private:
    void acceptLoop() {
        while (!_stopping) {
            int client = ::accept(_listener, nullptr, nullptr);
            if (client < 0) {
                if (_stopping)
                    return;
                continue;
            }
            _workers.emplace_back([client]() {
                struct timeval timeout{};
                timeout.tv_sec = 1;
                ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                char request[1024];
                const ssize_t received = ::recv(client, request, sizeof(request), 0);
                const bool download = received >= 4 && std::string(request, 4) == "GET ";
                if (download)
                    ::usleep(750000);
                const char *response = download
                    ? "HTTP/1.1 404 Not Found\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n\r\n"
                    : "HTTP/1.1 200 OK\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                ::send(client, response, std::strlen(response), MSG_NOSIGNAL);
                ::shutdown(client, SHUT_RDWR);
                ::close(client);
            });
        }
    }

    int _listener = -1;
    uint16_t _port = 0;
    std::atomic<bool> _stopping{false};
    std::thread _thread;
    std::vector<std::thread> _workers;
};

struct SegmentFixture {
    std::string camera;
    std::string stream;
    std::string segment_key;
    std::string source_path;
    std::string payload;
    int64_t start = 0;
    int64_t end = 0;
};

SegmentFixture makeSegmentFixture(const std::string &root,
                                  const std::string &camera,
                                  const std::string &segment_key,
                                  const std::string &payload,
                                  int64_t start) {
    SegmentFixture fixture;
    fixture.camera = camera;
    fixture.stream = "main";
    fixture.segment_key = segment_key;
    fixture.source_path = root + "/hot/record/" + camera + "/" + fixture.stream + "/" + segment_key + ".mp4";
    fixture.payload = payload;
    fixture.start = start;
    fixture.end = start + 60;
    writeFile(fixture.source_path, payload);
    writeTimeBlock(root, fixture.camera, fixture.stream, fixture.source_path,
                   fixture.start, fixture.end, static_cast<int64_t>(payload.size()));
    return fixture;
}

void moveSegmentToMinio(TierStorageManager &manager,
                        const std::string &root,
                        const StoragePool &pool,
                        const SegmentFixture &fixture) {
    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    require(manager.createPool(pool) == pool.id, "MinIO pool creation failed");
    auto policy = makeColdMovePolicy(hot_pools.front().id, pool.id);
    require(manager.createPolicy(policy) == policy.id, "Cold policy creation failed");
    require(manager.assignPolicyToCamera(fixture.camera, policy.id, "MinIO integration test"),
            "Cold policy assignment failed");
    require(manager.registerHotSegmentRange(
                fixture.camera, fixture.stream, fixture.source_path,
                fixture.start, fixture.end, static_cast<int64_t>(fixture.payload.size())),
            "HOT range registration failed");

    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(fixture.camera);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 15000), "HOT to MinIO job did not reach a terminal state");
    auto jobs = manager.listTieringJobs(fixture.camera);
    require(!jobs.empty() && jobs.front().status == "DONE",
            jobs.empty() ? "HOT to MinIO job was not created"
                         : "HOT to MinIO failed: " + jobs.front().error_message.value_or(""));
    require(!File::fileExist(fixture.source_path), "HOT source remained after MinIO upload");
    auto ranges = SegmentTierRangeImp().queryByCamera(
        fixture.camera, fixture.start - 1, fixture.end + 1);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(ColdTier) &&
            ranges.front().pool_id == pool.id,
            "Tier range was not updated to COLD/MinIO");
    drainWorkThreadPool();
}

int scenarioRegisterIdempotent(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-idempotent";
    const std::string stream = "main";
    const std::string path = root + "/hot/record/" + camera + "/" + stream + "/segment.mp4";
    writeFile(path, "idempotent-segment");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    require(manager.registerHotSegmentRange(camera, stream, path, now - 120, now - 60, 18),
            "First HOT registration failed");
    require(manager.registerHotSegmentRange(camera, stream, path, now - 120, now - 60, 18),
            "Second HOT registration failed");

    SegmentTierRangeImp range_imp;
    auto ranges = range_imp.queryByCamera(camera, now - 180, now);
    require(ranges.size() == 1 && ranges.front().segment_count == 1 &&
                ranges.front().size_bytes == 18,
            "Duplicate registration changed compact range counters");

    require(range_imp.updateTierByRangeId(
                ranges.front().range_id, tierTypeToString(WarmTier), "warm-pool",
                segmentStatusToString(SegmentStatus::AVAILABLE)),
            "Cannot simulate an already-moved range");
    require(manager.registerHotSegmentRange(camera, stream, path, now - 120, now - 60, 18),
            "Delayed duplicate registration failed");
    ranges = range_imp.queryByCamera(camera, now - 180, now);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(WarmTier) &&
                ranges.front().pool_id == "warm-pool" &&
                ranges.front().segment_count == 1 && ranges.front().size_bytes == 18,
            "Delayed duplicate registration recreated HOT metadata");
    manager.stop();
    return 0;
}

int scenarioHotToNas(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    const StoragePool nas = makeNasPool(root);
    require(manager.createPool(nas) == nas.id, "NAS pool creation failed");
    const StoragePolicy policy = makeMovePolicy(hot_pools.front().id, nas.id);
    require(manager.createPolicy(policy) == policy.id, "Move policy creation failed");

    const std::string camera = "camera-nas";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "integration test"),
            "Policy assignment failed");
    const std::string segment_key = "2026-08-20/12-00-00";
    const std::string source = root + "/hot/record/" + camera + "/" + stream + "/" + segment_key + ".mp4";
    const std::string destination = root + "/nas/record/" + camera + "/" + stream + "/" + segment_key + ".mp4";
    const std::string payload = "tier-manager-hot-to-nas";
    writeFile(source, payload);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    writeTimeBlock(root, camera, stream, source, now - 180, now - 120,
                   static_cast<int64_t>(payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, source, now - 180, now - 120,
                                            static_cast<int64_t>(payload.size())),
            "HOT range registration failed");

    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(camera);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 5000), "Tiering cycle did not reach a terminal job state");
    auto jobs = manager.listTieringJobs(camera);
    require(!jobs.empty() && jobs.front().status == "DONE",
            jobs.empty() ? "Tiering job was not created" : "Tiering job failed: " + jobs.front().error_message.value_or(""));
    require(!File::fileExist(source), "HOT source was not removed after successful move");
    require(!File::is_dir(File::parentDir(source)),
            "Empty HOT date directory was not removed after successful move");
    require(File::loadFile(destination) == payload, "NAS destination content mismatch");
    auto ranges = SegmentTierRangeImp().queryByCamera(camera, now - 240, now);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(WarmTier) &&
            ranges.front().pool_id == nas.id, "Tier range was not updated to NAS/WARM");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioOverlappingSegmentsUseStartOwnership(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    const StoragePool nas = makeNasPool(root);
    require(manager.createPool(nas) == nas.id, "NAS pool creation failed");
    const StoragePolicy policy = makeMovePolicy(hot_pools.front().id, nas.id);
    require(manager.createPolicy(policy) == policy.id, "Move policy creation failed");

    const std::string camera = "camera-overlap-ownership";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "overlap ownership test"),
            "Policy assignment failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const int64_t first_start = now - 63;
    const int64_t first_end = now - 1;
    const int64_t second_start = now - 2;
    const int64_t second_end = now + 60;
    const std::string first_key = "2026-08-20/12-00-00";
    const std::string second_key = "2026-08-20/12-01-01";
    const std::string first_source = root + "/hot/record/" + camera + "/" + stream + "/" + first_key + ".mp4";
    const std::string second_source = root + "/hot/record/" + camera + "/" + stream + "/" + second_key + ".mp4";
    const std::string first_target = root + "/nas/record/" + camera + "/" + stream + "/" + first_key + ".mp4";
    const std::string second_target = root + "/nas/record/" + camera + "/" + stream + "/" + second_key + ".mp4";
    const std::string first_payload = "overlap-first";
    const std::string second_payload = "overlap-second";
    writeFile(first_source, first_payload);
    writeFile(second_source, second_payload);
    writeTimeBlock(root, camera, stream, first_source, first_start, first_end,
                   static_cast<int64_t>(first_payload.size()));
    writeTimeBlock(root, camera, stream, second_source, second_start, second_end,
                   static_cast<int64_t>(second_payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, first_source, first_start, first_end,
                                            static_cast<int64_t>(first_payload.size())),
            "First HOT range registration failed");
    require(manager.registerHotSegmentRange(camera, stream, second_source, second_start, second_end,
                                            static_cast<int64_t>(second_payload.size())),
            "Second HOT range registration failed");

    auto registered = SegmentTierRangeImp().queryByCamera(camera, first_start - 1, second_end + 1);
    require(registered.size() == 1, "Overlapping segments were not registered as one range");
    require(manager.extendRangeRetention(registered.front().range_id, now + 86400,
                                         "split keeps override"),
            "Cannot extend overlap range retention");

    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(camera);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 5000), "Overlap tiering job did not finish");
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == "DONE",
            jobs.empty() ? "Overlap tiering job was not created"
                         : "Overlap tiering job failed: " + jobs.front().error_message.value_or(""));
    require(!File::fileExist(first_source) && File::loadFile(first_target) == first_payload,
            "Eligible first segment was not moved");
    require(File::fileExist(second_source) && !File::fileExist(second_target),
            "Overlapping second segment was owned by both ranges");
    require(File::is_dir(File::parentDir(first_source)),
            "Date directory was removed while the second segment remained");

    auto ranges = SegmentTierRangeImp().queryByCamera(camera, first_start - 1, second_end + 1);
    require(ranges.size() == 2, "Overlap range was not split into two ownership windows");
    require(ranges[0].tier == tierTypeToString(WarmTier) &&
                ranges[0].end_time == second_start && ranges[0].segment_count == 1 &&
                ranges[1].tier == tierTypeToString(HotTier) &&
                ranges[1].start_time == second_start && ranges[1].segment_count == 1,
            "Split ranges do not follow segment start-time ownership");
    require(SegmentRetentionOverrideImp().isActive(ranges[0].range_id, now) &&
                SegmentRetentionOverrideImp().isActive(ranges[1].range_id, now),
            "Split range pieces did not keep the retention override");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioReusesExistingNasTarget(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    const StoragePool nas = makeNasPool(root);
    require(manager.createPool(nas) == nas.id, "NAS pool creation failed");
    const StoragePolicy policy = makeMovePolicy(hot_pools.front().id, nas.id);
    require(manager.createPolicy(policy) == policy.id, "Move policy creation failed");

    const std::string camera = "camera-reuse-nas";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "reuse target test"),
            "Policy assignment failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const int64_t start = now - 180;
    const int64_t end = now - 120;
    const std::string key = "2026-08-20/13-00-00";
    const std::string source = root + "/hot/record/" + camera + "/" + stream + "/" + key + ".mp4";
    const std::string target = root + "/nas/record/" + camera + "/" + stream + "/" + key + ".mp4";
    const std::string payload = "already-copied-to-nas";
    writeFile(source, payload);
    writeFile(target, payload);
    writeTimeBlock(root, camera, stream, source, start, end,
                   static_cast<int64_t>(payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, source, start, end,
                                            static_cast<int64_t>(payload.size())),
            "HOT range registration failed");
    require(::unlink(source.c_str()) == 0, "Cannot simulate missing source segment");

    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(camera);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 5000), "Reused-target tiering job did not finish");
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == "DONE",
            jobs.empty() ? "Reused-target job was not created"
                         : "Reused-target job failed: " + jobs.front().error_message.value_or(""));
    require(File::loadFile(target) == payload, "Existing NAS target was changed or removed");
    require(!File::is_dir(File::parentDir(source)),
            "Empty source date directory was not pruned after target reuse");
    auto ranges = SegmentTierRangeImp().queryByCamera(camera, start - 1, end + 1);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(WarmTier),
            "Reused-target range was not moved to WARM");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioBacksOffMissingSegmentJob(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    const StoragePool nas = makeNasPool(root);
    require(manager.createPool(nas) == nas.id, "NAS pool creation failed");
    const StoragePolicy policy = makeMovePolicy(hot_pools.front().id, nas.id);
    require(manager.createPolicy(policy) == policy.id, "Move policy creation failed");

    const std::string camera = "camera-missing-both";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "missing segment test"),
            "Policy assignment failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const int64_t start = now - 180;
    const int64_t end = now - 120;
    const std::string source = root + "/hot/record/" + camera + "/" + stream +
                               "/2026-08-20/14-00-00.mp4";
    const std::string payload = "missing-from-both-pools";
    writeFile(source, payload);
    writeTimeBlock(root, camera, stream, source, start, end,
                   static_cast<int64_t>(payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, source, start, end,
                                            static_cast<int64_t>(payload.size())),
            "HOT range registration failed");
    require(::unlink(source.c_str()) == 0, "Cannot simulate missing source segment");

    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(camera);
        return !jobs.empty() && jobs.front().status == "FAILED";
    }, 5000), "Missing segment job did not fail");
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().error_message.has_value() &&
                jobs.front().error_message.value().find("SOURCE_AND_TARGET_NOT_FOUND") == 0,
            "Missing source/target did not produce a retryable missing-segment error");
    manager.runTieringCycle();
    drainWorkThreadPool();
    require(manager.listTieringJobs(camera).size() == 1,
            "Missing segment job was queued again before its retry backoff elapsed");
    auto ranges = SegmentTierRangeImp().queryByCamera(camera, start - 1, end + 1);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(HotTier) &&
                ranges.front().status == segmentStatusToString(SegmentStatus::AVAILABLE),
            "Range changed before missing-segment retries were exhausted");
    manager.stop();
    return 0;
}

int scenarioDropsMissingSegmentAfterMaxAttempts(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kTieringMissingSegmentMaxAttempts] = 1;
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    const StoragePool nas = makeNasPool(root);
    require(manager.createPool(nas) == nas.id, "NAS pool creation failed");
    const StoragePolicy policy = makeMovePolicy(hot_pools.front().id, nas.id);
    require(manager.createPolicy(policy) == policy.id, "Move policy creation failed");

    const std::string camera = "camera-missing-dropped";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "drop missing segment test"),
            "Policy assignment failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const int64_t lost_start = now - 300;
    const int64_t kept_start = now - 240;
    const int64_t kept_end = now - 180;
    const std::string lost_key = "2026-08-20/15-00-00";
    const std::string kept_key = "2026-08-20/15-01-00";
    const std::string lost_source = root + "/hot/record/" + camera + "/" + stream + "/" + lost_key + ".mp4";
    const std::string kept_source = root + "/hot/record/" + camera + "/" + stream + "/" + kept_key + ".mp4";
    const std::string lost_target = root + "/nas/record/" + camera + "/" + stream + "/" + lost_key + ".mp4";
    const std::string kept_target = root + "/nas/record/" + camera + "/" + stream + "/" + kept_key + ".mp4";
    const std::string lost_payload = "lost-segment";
    const std::string kept_payload = "kept-segment-payload";
    writeFile(lost_source, lost_payload);
    writeFile(kept_source, kept_payload);
    writeTimeBlock(root, camera, stream, lost_source, lost_start, kept_start,
                   static_cast<int64_t>(lost_payload.size()));
    writeTimeBlock(root, camera, stream, kept_source, kept_start, kept_end,
                   static_cast<int64_t>(kept_payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, lost_source, lost_start, kept_start,
                                            static_cast<int64_t>(lost_payload.size())),
            "Lost segment range registration failed");
    require(manager.registerHotSegmentRange(camera, stream, kept_source, kept_start, kept_end,
                                            static_cast<int64_t>(kept_payload.size())),
            "Kept segment range registration failed");
    require(SegmentTierRangeImp().queryByCamera(camera, lost_start - 1, kept_end + 1).size() == 1,
            "Adjacent segments were not registered as one range");
    require(::unlink(lost_source.c_str()) == 0, "Cannot simulate lost source segment");

    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(camera);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 5000), "Missing-segment drop job did not finish");
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == "DONE",
            jobs.empty() ? "Missing-segment drop job was not created"
                         : "Missing-segment drop job failed: " + jobs.front().error_message.value_or(""));
    require(!File::fileExist(kept_source) && File::loadFile(kept_target) == kept_payload,
            "Present segment was not moved when its sibling was missing");
    require(!File::fileExist(lost_target), "A target was invented for the lost segment");

    auto ranges = SegmentTierRangeImp().queryByCamera(camera, lost_start - 1, kept_end + 1);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(WarmTier) &&
                ranges.front().segment_count == 1 &&
                ranges.front().size_bytes == static_cast<int64_t>(kept_payload.size()),
            "Range stats still include the dropped segment");

    bool alerted = false;
    for (const auto &alert : StorageAlertImp().query("", -1, 0, 50)) {
        if (alert.type == "TIER_SEGMENT_MISSING" &&
            alert.message.find(ranges.front().range_id) != std::string::npos) {
            alerted = true;
        }
    }
    require(alerted, "Dropping a missing segment did not raise a storage alert");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioHotToMinio(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-minio", "2026-08-20/13-00-00",
                                      "tier-manager-hot-to-minio", now - 180);
    auto pool = makeMinioPool(root);
    const std::string object_key = TierObjectStorage::makeS3Key(
        pool.base_path.value_or(""), fixture.camera, fixture.stream, fixture.segment_key);
    ObjectCleanup cleanup(pool, object_key);
    moveSegmentToMinio(manager, root, pool, fixture);
    require(cleanup.storage().segmentExists(pool.id, object_key),
            "Uploaded MinIO object does not exist");
    const std::string download = root + "/verify-minio.mp4";
    require(cleanup.storage().downloadSegment(pool.id, object_key, download),
            "Cannot download uploaded MinIO object");
    require(File::loadFile(download) == fixture.payload, "MinIO object content mismatch");
    manager.stop();
    return 0;
}

int scenarioRestoreFromMinio(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-restore", "2026-08-20/14-00-00",
                                      "tier-manager-restore-from-minio", now - 180);
    auto pool = makeMinioPool(root);
    const std::string object_key = TierObjectStorage::makeS3Key(
        pool.base_path.value_or(""), fixture.camera, fixture.stream, fixture.segment_key);
    ObjectCleanup cleanup(pool, object_key);
    moveSegmentToMinio(manager, root, pool, fixture);

    const int64_t declared_size = static_cast<int64_t>(fixture.payload.size()) + 17;
    auto request = manager.requestRestoreSegment(
        fixture.camera, fixture.stream, fixture.start, fixture.source_path, "integration test",
        declared_size);
    require(request.handled && !request.job_id.empty(), "Restore request was not accepted");
    require(waitUntil([&]() {
        auto jobs = manager.getRestoreJob(request.job_id);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 15000), "Restore job did not reach a terminal state");
    auto jobs = manager.getRestoreJob(request.job_id);
    require(jobs.size() == 1 && jobs.front().status == "DONE",
            jobs.empty() ? "Restore job disappeared"
                         : "Restore failed: " + jobs.front().error_message.value_or(""));
    auto children = RestoreJobSegmentImp().queryByJob(request.job_id);
    require(children.size() == 1 &&
                children.front().total_bytes == declared_size &&
                children.front().processed_bytes == static_cast<int64_t>(fixture.payload.size()) &&
                jobs.front().total_bytes == declared_size &&
                jobs.front().processed_bytes == static_cast<int64_t>(fixture.payload.size()),
            "Restore completion changed total_bytes instead of preserving TimeBlock metadata");
    const std::string restored = manager.getRestoreSegmentPath(
        request.job_id, fixture.camera, fixture.stream, fixture.segment_key);
    require(File::loadFile(restored) == fixture.payload, "Restored MinIO content mismatch");
    manager.stop();
    return 0;
}

int scenarioDuplicateRestore(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto pool = makeObjectPool(root, "http://127.0.0.1:1", "test", "test", "test-bucket");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    pool.created_at = now;
    pool.updated_at = now;
    require(StoragePoolImp().add(pool), "Object pool persistence failed");
    auto fixture = makeSegmentFixture(root, "camera-duplicate", "2026-08-20/15-00-00",
                                      "duplicate-restore", now - 180);
    SegmentTierRange range;
    range.range_id = "range-duplicate";
    range.camera_id = fixture.camera;
    range.stream_id = fixture.stream;
    range.tier = tierTypeToString(ColdTier);
    range.pool_id = pool.id;
    range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
    range.start_time = fixture.start;
    range.end_time = fixture.end;
    range.segment_count = 1;
    range.size_bytes = fixture.payload.size();
    require(SegmentTierRangeImp().mergeOrInsert(range), "Cannot seed COLD range");

    RestoreJob existing;
    existing.job_id = "existing-restore";
    existing.camera_id = fixture.camera;
    existing.source_tier = "COLD";
    existing.target_tier = "HOT";
    existing.status = "PENDING";
    existing.start_time = fixture.start;
    existing.end_time = fixture.end;
    existing.created_at = now;
    existing.updated_at = now;
    RestoreJobSegment child;
    child.job_id = existing.job_id;
    child.camera_id = fixture.camera;
    child.stream_id = fixture.stream;
    child.range_id = range.range_id;
    child.segment_path = fixture.segment_key;
    child.source_pool_id = pool.id;
    child.range_start = fixture.start;
    child.range_end = fixture.end;
    child.status = "PENDING";
    child.created_at = now;
    child.updated_at = now;
    require(RestoreJobImp().addWithSegments(existing, {child}), "Cannot seed active restore child");

    auto first = manager.requestRestoreSegment(
        fixture.camera, fixture.stream, fixture.start, fixture.source_path, "duplicate one");
    auto second = manager.requestRestoreSegment(
        fixture.camera, fixture.stream, fixture.start, fixture.source_path, "duplicate two");
    require(first.handled && second.handled, "Duplicate restore was not handled");
    require(first.job_id == existing.job_id && second.job_id == existing.job_id,
            "Duplicate restore created a different parent job");
    require(RestoreJobSegmentImp().queryActiveSegment(
                fixture.camera, fixture.stream, fixture.segment_key).size() == 1,
            "More than one active child exists for the same physical segment");
    manager.stop();
    return 0;
}

int scenarioRestoreWindowIncludesAllStreams(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto pool = makeObjectPool(root, "http://127.0.0.1:1", "test", "test", "test-bucket");
    require(manager.createPool(pool) == pool.id, "Cannot create multi-stream restore pool");

    const std::string camera = "camera-restore-all-streams";
    const int64_t start = static_cast<int64_t>(::time(nullptr)) - 180;
    const int64_t end = start + 60;
    const std::vector<std::string> streams = {"main", "sub"};
    for (const auto &stream : streams) {
        const std::string segment_key = "2026-09-12/10-00-00-" + stream;
        const std::string path = root + "/hot/record/" + camera + "/" + stream + "/" + segment_key + ".mp4";
        writeTimeBlock(root, camera, stream, path, start, end, 64);

        SegmentTierRange range;
        range.range_id = "range-restore-all-" + stream;
        range.camera_id = camera;
        range.stream_id = stream;
        range.tier = tierTypeToString(ColdTier);
        range.pool_id = pool.id;
        range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
        range.start_time = start;
        range.end_time = end;
        range.segment_count = 1;
        range.size_bytes = 64;
        require(SegmentTierRangeImp().mergeOrInsert(range),
                "Cannot seed multi-stream COLD range");
    }

    auto request = manager.requestRestoreWindow(camera, "", start + 10, end - 10,
                                                "all stream restore");
    require(request.handled && !request.job_id.empty(),
            "Restore without stream id was not accepted");
    const auto children = RestoreJobSegmentImp().queryByJob(request.job_id);
    std::set<std::string> restored_streams;
    for (const auto &child : children) {
        restored_streams.insert(child.stream_id);
        require(child.total_bytes == 64,
                "Restore child did not preserve the TimeBlock byte size");
    }
    require(children.size() == 2 && restored_streams.size() == 2 &&
                restored_streams.count("main") == 1 && restored_streams.count("sub") == 1,
            "Restore without stream id did not include both camera streams");
    const auto jobs = manager.getRestoreJob(request.job_id);
    require(jobs.size() == 1 && jobs.front().total_bytes == 128,
            "Restore parent did not aggregate child TimeBlock byte sizes");

    ExtractJob extract;
    extract.file_id = "extract-restoring-state";
    extract.request_hash = "request-hash";
    extract.camera_id = camera;
    extract.start_time = start + 10;
    extract.end_time = end - 10;
    extract.format = "mp4";
    extract.upload_url = "https://example.invalid/upload";
    extract.created_at = start;
    extract.updated_at = start;
    ExtractJob stored;
    ExtractJobImp extract_imp;
    require(extract_imp.createOrFind(extract, stored) == ExtractJobRepository::CreateResult::CREATED,
            "Cannot seed persistent extract job");
    require(extract_imp.claimExtract(extract.file_id, 0, start),
            "Cannot claim persistent extract job");
    require(extract_imp.updateLifecycleState(extract.file_id, "RESTORING", 15.0f,
                                             "main", start + 1),
            "Cannot persist extract RESTORING state");
    auto extracts = extract_imp.findByFileId(extract.file_id);
    require(extracts.size() == 1 && extracts.front().status == "RESTORING",
            "Extract job did not persist its restore stage");
    require(extract_imp.updateLifecycleState(extract.file_id, "EXTRACTING", 30.0f,
                                             "main", start + 2),
            "Cannot persist transition from restore to extraction");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioRestoreStatusPreservesDeclaredTotals(const std::string &root) {
    migrateDatabase(root);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    RestoreJob job;
    job.job_id = "restore-fixed-total";
    job.camera_id = "camera-fixed-total";
    job.source_tier = "COLD";
    job.target_tier = "HOT";
    job.status = "PENDING";
    job.start_time = now - 60;
    job.end_time = now;
    job.total_bytes = 777;
    job.created_at = now;
    job.updated_at = now;

    RestoreJobSegment child;
    child.job_id = job.job_id;
    child.camera_id = job.camera_id;
    child.stream_id = "main";
    child.segment_path = "2026-09-12/10-00-00";
    child.source_pool_id = "cold-pool";
    child.range_start = job.start_time;
    child.range_end = job.end_time;
    child.status = "PENDING";
    child.total_bytes = 777;
    child.created_at = now;
    child.updated_at = now;
    require(RestoreJobImp().addWithSegments(job, {child}),
            "Cannot seed restore byte-accounting fixture");

    require(RestoreJobSegmentImp().updateStatus(child, "DONE", 700),
            "Cannot update restore child status");
    require(RestoreJobImp().updateStatus(job.job_id, "DONE", 700),
            "Cannot update restore parent status");
    const auto jobs = RestoreJobImp().findByJobId(job.job_id);
    const auto children = RestoreJobSegmentImp().queryByJob(job.job_id);
    require(jobs.size() == 1 && children.size() == 1 &&
                jobs.front().total_bytes == 777 && jobs.front().processed_bytes == 700 &&
                children.front().total_bytes == 777 && children.front().processed_bytes == 700,
            "Restore status transition mutated the declared TimeBlock totals");

    ExtractJob extract;
    extract.file_id = "extract-lifecycle-recovery";
    extract.request_hash = "extract-lifecycle-recovery-hash";
    extract.camera_id = job.camera_id;
    extract.start_time = job.start_time;
    extract.end_time = job.end_time;
    extract.format = "mp4";
    extract.upload_url = "https://example.invalid/upload";
    extract.created_at = now;
    extract.updated_at = now;
    ExtractJob stored;
    ExtractJobImp extract_imp;
    require(extract_imp.createOrFind(extract, stored) == ExtractJobRepository::CreateResult::CREATED,
            "Cannot seed extraction lifecycle fixture");
    require(extract_imp.claimExtract(extract.file_id, 0, now + 1),
            "Cannot claim extraction lifecycle fixture");
    require(extract_imp.updateLifecycleState(extract.file_id, "RESTORING", 12.0f,
                                             "main", now + 2),
            "Cannot persist extraction restore stage");
    auto extracts = extract_imp.findByFileId(extract.file_id);
    require(extracts.size() == 1 && extracts.front().status == "RESTORING",
            "Extractor lifecycle did not persist its restore stage");
    extract_imp.recoverInterrupted(now + 4);
    extracts = extract_imp.findByFileId(extract.file_id);
    require(extracts.size() == 1 && extracts.front().status == "PENDING" &&
                extracts.front().progress_percent == 0.0f,
            "Interrupted extractor lifecycle was not reset for automatic replanning");
    return 0;
}

int scenarioReusesEquivalentRestoreWindows(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kRestoreTTLSeconds] = 30;
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto pool = makeObjectPool(root, "http://127.0.0.1:1", "test", "test", "test-bucket");
    require(manager.createPool(pool) == pool.id, "Cannot create restore reuse test pool");

    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-independent-restore",
                                      "2026-09-11/10-00-00",
                                      "independent-restore", now - 60);
    SegmentTierRange range;
    range.range_id = "range-independent-restore";
    range.camera_id = fixture.camera;
    range.stream_id = fixture.stream;
    range.tier = tierTypeToString(ColdTier);
    range.pool_id = pool.id;
    range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
    range.start_time = fixture.start;
    range.end_time = fixture.end;
    range.segment_count = 1;
    range.size_bytes = static_cast<int64_t>(fixture.payload.size());
    require(SegmentTierRangeImp().mergeOrInsert(range),
            "Cannot seed independent restore range");

    auto first = manager.requestRestoreWindow(
        fixture.camera, fixture.stream, fixture.start, fixture.end, "first restore window");
    auto second = manager.requestRestoreWindow(
        fixture.camera, fixture.stream, fixture.start, fixture.end, "second restore window");
    require(first.handled && first.job_created && second.handled && !second.job_created &&
                !first.job_id.empty() && first.job_id == second.job_id,
            "Equivalent active restore window was not reused");

    auto first_children = RestoreJobSegmentImp().queryByJob(first.job_id);
    require(first_children.size() == 1,
            "Reused active restore job did not retain its segment list");

    const auto completed_fixture = makeSegmentFixture(
        root, "camera-completed-restore-reuse", "2026-09-11/11-00-00",
        "completed-restore-reuse", now - 120);
    SegmentTierRange completed_range = range;
    completed_range.range_id = "range-completed-restore-reuse";
    completed_range.camera_id = completed_fixture.camera;
    completed_range.stream_id = completed_fixture.stream;
    completed_range.start_time = completed_fixture.start;
    completed_range.end_time = completed_fixture.end;
    completed_range.size_bytes = static_cast<int64_t>(completed_fixture.payload.size());
    require(SegmentTierRangeImp().mergeOrInsert(completed_range),
            "Cannot seed completed restore reuse range");

    RestoreJob completed_job;
    completed_job.job_id = "completed-restore-reuse";
    completed_job.camera_id = completed_fixture.camera;
    completed_job.source_tier = tierTypeToString(ColdTier);
    completed_job.target_tier = tierTypeToString(HotTier);
    completed_job.status = jobStatusToString(JobStatus::DONE);
    completed_job.start_time = completed_fixture.start;
    completed_job.end_time = completed_fixture.end;
    completed_job.total_bytes = static_cast<int64_t>(completed_fixture.payload.size());
    completed_job.processed_bytes = completed_job.total_bytes;
    completed_job.created_at = now - 120;
    completed_job.updated_at = now - 60;

    RestoreJobSegment completed_child;
    completed_child.job_id = completed_job.job_id;
    completed_child.camera_id = completed_fixture.camera;
    completed_child.stream_id = completed_fixture.stream;
    completed_child.range_id = completed_range.range_id;
    completed_child.segment_path = completed_fixture.segment_key;
    completed_child.source_pool_id = pool.id;
    completed_child.range_start = completed_fixture.start;
    completed_child.range_end = completed_fixture.end;
    completed_child.status = jobStatusToString(JobStatus::DONE);
    completed_child.total_bytes = completed_job.total_bytes;
    completed_child.processed_bytes = completed_job.processed_bytes;
    completed_child.created_at = completed_job.created_at;
    completed_child.updated_at = completed_job.updated_at;
    require(RestoreJobImp().addWithSegments(completed_job, {completed_child}),
            "Cannot seed completed restore job for TTL extension");
    const std::string completed_cache = manager.getRestoreSegmentPath(
        completed_job.job_id, completed_fixture.camera, completed_fixture.stream,
        completed_fixture.segment_key);
    writeFile(completed_cache, completed_fixture.payload);

    auto reused_done = manager.requestRestoreWindow(
        completed_fixture.camera, completed_fixture.stream,
        completed_fixture.start, completed_fixture.end, "reuse completed restore");
    require(reused_done.handled && !reused_done.job_created &&
                reused_done.job_id == completed_job.job_id &&
                reused_done.status == jobStatusToString(JobStatus::DONE),
            "Equivalent completed restore job was not reused");
    auto refreshed_jobs = RestoreJobImp().findByJobId(completed_job.job_id);
    require(refreshed_jobs.size() == 1 && refreshed_jobs.front().updated_at >= now,
            "Reused completed restore job TTL was not extended");
    RestoreJobSnapshot snapshot;
    require(manager.getRestoreJobSnapshot(completed_job.job_id, snapshot) &&
                snapshot.playback_ready && snapshot.playback_status == "READY" &&
                snapshot.expires_at > now,
            "TTL-extended restore job was not playback-ready");

    require(::unlink(completed_cache.c_str()) == 0,
            "Cannot remove completed cache before missing-cache reuse check");
    auto replacement = manager.requestRestoreWindow(
        completed_fixture.camera, completed_fixture.stream,
        completed_fixture.start, completed_fixture.end, "replace missing restore cache");
    require(replacement.handled && replacement.job_created &&
                replacement.job_id != completed_job.job_id,
            "Completed restore job with missing cache was incorrectly reused");

    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioRetentionApproval(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    auto policy = makeRetentionPolicy(hot_pools.front().id, "retention-approval", true);
    require(manager.createPolicy(policy) == policy.id, "Retention policy creation failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-retention", "2026-08-10/10-00-00",
                                      "retention-approval", now - 3 * 86400);
    require(manager.assignPolicyToCamera(fixture.camera, policy.id, "retention integration"),
            "Retention policy assignment failed");
    require(manager.registerHotSegmentRange(
                fixture.camera, fixture.stream, fixture.source_path,
                fixture.start, fixture.end, fixture.payload.size()),
            "Retention range registration failed");
    auto ranges = SegmentTierRangeImp().queryByCamera(
        fixture.camera, fixture.start - 1, fixture.end + 1);
    require(ranges.size() == 1, "Retention range was not persisted");
    const std::string range_id = ranges.front().range_id;
    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto current = SegmentTierRangeImp().findByRangeIds({range_id});
        return current.size() == 1 && current.front().status == "EXPIRED";
    }, 5000), "Retention cycle did not mark range EXPIRED");
    drainWorkThreadPool();
    require(File::fileExist(fixture.source_path), "Approval policy deleted data before approval");
    require(manager.approveExpiredRange(range_id), "Expired range approval failed");
    require(!File::fileExist(fixture.source_path), "Approved expiry did not delete physical data");
    auto current = SegmentTierRangeImp().findByRangeIds({range_id});
    require(current.size() == 1 && current.front().status == "DELETED",
            "Approved expiry did not mark range DELETED");
    manager.stop();
    return 0;
}

int scenarioProtectedRetention(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    auto policy = makeRetentionPolicy(hot_pools.front().id, "retention-protected", false);
    require(manager.createPolicy(policy) == policy.id, "Retention policy creation failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-protected", "2026-08-10/11-00-00",
                                      "protected-retention", now - 3 * 86400);
    require(manager.assignPolicyToCamera(fixture.camera, policy.id, "protection integration"),
            "Retention policy assignment failed");
    require(manager.registerHotSegmentRange(
                fixture.camera, fixture.stream, fixture.source_path,
                fixture.start, fixture.end, fixture.payload.size()),
            "Protected range registration failed");
    auto ranges = SegmentTierRangeImp().queryByCamera(
        fixture.camera, fixture.start - 1, fixture.end + 1);
    require(ranges.size() == 1, "Protected range was not persisted");
    const std::string range_id = ranges.front().range_id;

    ProtectedVideo video;
    video.protected_id = "protected-video";
    video.camera_id = fixture.camera;
    video.start_time = fixture.start;
    video.end_time = fixture.end;
    video.type = "EVIDENCE";
    video.created_at = now;
    require(ProtectedVideoImp().add(video), "Cannot protect video range");
    require(manager.extendRangeRetention(range_id, now + 86400, "integration test"),
            "Cannot extend range retention");
    manager.runTieringCycle();
    PoolHealthInfo health;
    require(waitUntil([&]() {
        return manager.getPoolHealth(hot_pools.front().id, health) &&
               health.health_status != healthToString(StorageHealth::UNKNOWN);
    }, 5000), "Protected retention cycle did not complete runtime health check");
    drainWorkThreadPool();
    require(File::fileExist(fixture.source_path), "Protected range was physically deleted");
    auto current = SegmentTierRangeImp().findByRangeIds({range_id});
    require(current.size() == 1 && current.front().status == "AVAILABLE",
            "Protected range status changed unexpectedly");

    require(ProtectedVideoImp().remove(video.protected_id), "Cannot remove video protection");
    SegmentRetentionOverride expired;
    expired.range_id = range_id;
    expired.extend_until = now - 1;
    expired.updated_at = now;
    require(SegmentRetentionOverrideImp().upsert(expired), "Cannot expire retention override");
    require(waitUntil([&]() {
        manager.runTieringCycle();
        return !File::fileExist(fixture.source_path);
    }, 5000), "Range was not deleted after protections expired");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioDeletionWaitsForActiveTransfer(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    auto policy = makeRetentionPolicy(hot_pools.front().id, "retention-active-job", false);
    require(manager.createPolicy(policy) == policy.id, "Retention policy creation failed");
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-delete-active-job", "2026-08-10/12-00-00",
                                      "active-transfer", now - 3 * 86400);
    require(manager.assignPolicyToCamera(fixture.camera, policy.id, "active job deletion guard"),
            "Retention policy assignment failed");
    require(manager.registerHotSegmentRange(
                fixture.camera, fixture.stream, fixture.source_path,
                fixture.start, fixture.end, fixture.payload.size()),
            "Range registration failed");
    auto ranges = SegmentTierRangeImp().queryByCamera(
        fixture.camera, fixture.start - 1, fixture.end + 1);
    require(ranges.size() == 1, "Range was not persisted");
    const SegmentTierRange range = ranges.front();

    // start() recovers RUNNING jobs, so the in-flight transfer is seeded afterwards.
    TieringJob job;
    job.job_id = "active-transfer-job";
    job.camera_id = range.camera_id;
    job.stream_id = range.stream_id;
    job.range_id = range.range_id;
    job.source_tier = range.tier;
    job.target_tier = tierTypeToString(WarmTier);
    job.source_pool_id = range.pool_id;
    job.target_pool_id = "some-warm-pool";
    job.status = jobStatusToString(JobStatus::RUNNING);
    job.segment_start_time = range.start_time;
    job.segment_end_time = range.end_time;
    job.bytes_total = range.size_bytes;
    job.created_at = now;
    job.updated_at = now;
    require(TieringJobImp().add(job), "Cannot seed running tiering job");

    manager.runTieringCycle();
    PoolHealthInfo health;
    require(waitUntil([&]() {
        return manager.getPoolHealth(hot_pools.front().id, health) &&
               health.health_status != healthToString(StorageHealth::UNKNOWN);
    }, 5000), "Retention cycle did not complete runtime health check");
    drainWorkThreadPool();
    require(TieringJobImp().findByJobId(job.job_id).front().status ==
                jobStatusToString(JobStatus::RUNNING),
            "Seeded running job was touched by the cycle");
    require(File::fileExist(fixture.source_path),
            "Retention deleted files while a transfer job was active");
    auto current = SegmentTierRangeImp().findByRangeIds({range.range_id});
    require(current.size() == 1 && current.front().status == "AVAILABLE",
            "Retention changed a range owned by an active transfer");

    // A snapshot that no longer matches the row must not be finalized.
    SegmentTierRange stale = current.front();
    stale.pool_id = "another-pool";
    TierMetadataCleanupJob cleanup_job;
    cleanup_job.range_id = stale.range_id;
    cleanup_job.camera_id = stale.camera_id;
    cleanup_job.status = "PENDING";
    cleanup_job.created_at = now;
    cleanup_job.updated_at = now;
    require(!SegmentTierRangeImp().markDeletedBySnapshotAndEnqueue(stale, "DELETED", cleanup_job),
            "Stale range snapshot was marked deleted");
    require(SegmentTierRangeImp().findByRangeIds({range.range_id}).front().status == "AVAILABLE",
            "Stale snapshot changed the range status");

    require(TieringJobImp().updateStatus(job.job_id, jobStatusToString(JobStatus::FAILED)),
            "Cannot finish seeded tiering job");
    require(waitUntil([&]() {
        manager.runTieringCycle();
        return !File::fileExist(fixture.source_path);
    }, 5000), "Retention did not resume after the transfer finished");
    require(waitUntil([&]() {
        auto rows = SegmentTierRangeImp().findByRangeIds({range.range_id});
        return rows.size() == 1 && rows.front().status == "DELETED";
    }, 5000), "Range was not finalized after deletion resumed");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioRestoreConcurrencyLimit(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kAutoRestoreMaxConcurrent] = 2;
    SlowS3Server server;
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto pool = makeObjectPool(root, server.endpoint(), "test", "test", "test-bucket");
    require(manager.createPool(pool) == pool.id, "Object pool creation failed");

    const std::string camera = "camera-concurrency";
    const std::string stream = "main";
    const int64_t start = static_cast<int64_t>(::time(nullptr)) - 600;
    const int segment_count = 6;
    for (int index = 0; index < segment_count; ++index) {
        const std::string key = "2026-08-20/16-00-0" + std::to_string(index);
        const std::string path = root + "/hot/record/" + camera + "/" + stream + "/" + key + ".mp4";
        writeTimeBlock(root, camera, stream, path, start + index * 60,
                       start + (index + 1) * 60, 32);
    }

    SegmentTierRange range;
    range.range_id = "range-concurrency";
    range.camera_id = camera;
    range.stream_id = stream;
    range.tier = tierTypeToString(ColdTier);
    range.pool_id = pool.id;
    range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
    range.start_time = start;
    range.end_time = start + segment_count * 60;
    range.segment_count = segment_count;
    range.size_bytes = segment_count * 32;
    require(SegmentTierRangeImp().mergeOrInsert(range), "Cannot seed COLD restore range");

    auto request = manager.requestRestoreWindow(
        camera, stream, range.start_time, range.end_time, "concurrency integration test");
    require(request.handled && !request.job_id.empty(), "Restore window was not accepted");

    int maximum_running = 0;
    bool observed_full_capacity = false;
    bool observed_pending_at_capacity = false;
    require(waitUntil([&]() {
        auto children = RestoreJobSegmentImp().queryByJob(request.job_id);
        int running = 0;
        int pending = 0;
        int terminal = 0;
        for (const auto &child : children) {
            running += child.status == "RUNNING" ? 1 : 0;
            pending += child.status == "PENDING" ? 1 : 0;
            terminal += (child.status == "DONE" || child.status == "FAILED") ? 1 : 0;
        }
        maximum_running = std::max(maximum_running, running);
        require(running <= 2, "Restore concurrency exceeded configured limit");
        observed_full_capacity = observed_full_capacity || running == 2;
        observed_pending_at_capacity = observed_pending_at_capacity || (running == 2 && pending > 0);
        return static_cast<int>(children.size()) == segment_count && terminal == segment_count;
    }, 15000), "Restore children did not reach terminal states");
    require(maximum_running == 2 && observed_full_capacity,
            "Restore workers never reached configured concurrency");
    require(observed_pending_at_capacity,
            "No pending restore remained while worker capacity was full");
    drainWorkThreadPool();
    manager.stop();
    return 0;
}

int scenarioSearchEngineReflectsTierMetadata(const std::string &root) {
    migrateDatabase(root);
    migrateEscDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    auto fixture = makeSegmentFixture(root, "camera-search", "2026-08-20/17-00-00",
                                      "search-tier-metadata", now - 180);
    require(manager.registerHotSegmentRange(
                fixture.camera, fixture.stream, fixture.source_path,
                fixture.start, fixture.end, fixture.payload.size()),
            "Search range registration failed");

    auto query = [&](Json::Value &result) {
        bool called = false;
        MediaTuple tuple(DEFAULT_VHOST, fixture.camera, fixture.stream, "");
        SearchEngine::findTimePeriod(
            tuple, fixture.start, fixture.end, 0, 0, false, false, "", true,
            [&](const SockException &ex, const Json::Value &data) {
                require(!ex, "SearchEngine returned an error");
                result = data;
                called = true;
            });
        require(called, "SearchEngine callback was not invoked synchronously for edge query");
        require(result["periods"].isArray() && result["periods"].size() == 1,
                "SearchEngine did not return the recorded segment");
    };

    Json::Value hot_result;
    query(hot_result);
    const Json::ArrayIndex first = 0;
    require(hot_result["periods"][first]["tier"].asString() == "HOT" &&
            !hot_result["periods"][first]["restore_required"].asBool(),
            "SearchEngine did not expose HOT metadata");

    auto object_pool = makeObjectPool(root, "http://127.0.0.1:1", "test", "test", "test-bucket");
    object_pool.created_at = now;
    object_pool.updated_at = now;
    require(StoragePoolImp().add(object_pool), "Cannot seed object pool for search metadata");
    auto ranges = SegmentTierRangeImp().queryByCamera(
        fixture.camera, fixture.start - 1, fixture.end + 1);
    require(ranges.size() == 1, "Search tier range is missing");
    require(SegmentTierRangeImp().updateTierByRangeId(
                ranges.front().range_id, tierTypeToString(ColdTier), object_pool.id,
                segmentStatusToString(SegmentStatus::AVAILABLE)),
            "Cannot change search range to COLD");

    Json::Value cold_result;
    query(cold_result);
    require(cold_result["periods"][first]["tier"].asString() == "COLD" &&
            cold_result["periods"][first]["restore_required"].asBool(),
            "SearchEngine did not expose COLD restore metadata");
    manager.stop();
    return 0;
}

int scenarioLoadsSavedCameraRecordRoot(const std::string &root) {
    migrateDatabase(root);
    const std::string camera = "camera-saved-stats";
    const std::string camera_dir = root + "/hot/record/" + camera;
    makeDirectory(camera_dir);
    {
        CameraStatisticImp saved(camera_dir, 0);
        DeviceTuple tuple;
        tuple.vhost = DEFAULT_VHOST;
        tuple.device_id = camera;
        tuple.name = "Saved statistics camera";
        saved.setDeviceTuple(tuple);
        CameraOption option;
        option.enableRecord = true;
        option.recordRootPath = root + "/legacy-record-root";
        saved.setCameraOption(option);
    }

    int loaded_count = 0;
    CameraStatisticImp::Ptr loaded;
    StatisticRecorder::Instance().loadSavedCameraStatistics(
        [&](CameraStatisticImp::Ptr &stats) {
            if (stats->getParams().tuple.device_id == camera) {
                loaded = stats;
                ++loaded_count;
            }
        });
    require(loaded_count == 1 && loaded,
            "loadSavedCameraStatistics did not restore the camera");
    require(loaded->getParams().option.recordRootPath == root + "/legacy-record-root",
            "Saved CameraOption::recordRootPath was not restored");

    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto hot_pool = makeHotPool(root, "saved-stats-hot", "saved-stats-hot");
    require(manager.createPool(hot_pool) == hot_pool.id, "Saved-stats HOT pool creation failed");
    auto policy = makeHotOnlyPolicy(hot_pool.id, "saved-stats-policy");
    require(manager.createPolicy(policy) == policy.id, "Saved-stats policy creation failed");
    require(manager.assignPolicyToCamera(camera, policy.id, "reload integration"),
            "Saved camera policy assignment failed");
    const std::string effective_root = manager.resolveCameraRecordRoot(camera);
    require(effective_root == root + "/saved-stats-hot",
            "Tier policy did not resolve the new camera record root");

    CameraOption refreshed = loaded->getParams().option;
    refreshed.recordRootPath = effective_root;
    loaded->setCameraOption(refreshed);
    CameraStatisticImp reloaded(camera_dir + "/info.txt", 0);
    require(reloaded.getParams().option.recordRootPath == effective_root,
            "Effective record root was not persisted back to camera statistics");
    manager.stop();
    return 0;
}

int scenarioPolicyLifecycleChangesCameraStorage(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    auto hot_a = makeHotPool(root, "policy-hot-a", "policy-hot-a");
    auto hot_b = makeHotPool(root, "policy-hot-b", "policy-hot-b");
    auto warm = makeNasPool(root);
    require(manager.createPool(hot_a) == hot_a.id, "HOT A pool creation failed");
    require(manager.createPool(hot_b) == hot_b.id, "HOT B pool creation failed");
    require(manager.createPool(warm) == warm.id, "WARM pool creation failed");

    auto policy = makeMovePolicy(hot_a.id, warm.id);
    policy.id = "policy-lifecycle";
    policy.name = "Policy lifecycle integration";
    require(manager.createPolicy(policy) == policy.id, "Lifecycle policy creation failed");
    require(StoragePoolImp().countPolicyReferences(hot_a.id) == 1,
            "Exact HOT pool policy reference was not found");
    require(StoragePoolImp().countPolicyReferences("policy-hot") == 0,
            "Pool policy reference guard matched an id substring");
    const std::string camera = "camera-policy-lifecycle";
    require(manager.assignPolicyToCamera(camera, policy.id, "lifecycle integration"),
            "Lifecycle policy assignment failed");
    require(manager.resolveCameraRecordRoot(camera) == root + "/policy-hot-a",
            "Created policy did not select HOT A record root");

    auto persisted = manager.getPolicy(policy.id);
    require(persisted.size() == 1, "Lifecycle policy was not persisted");
    persisted.front().tiers_json = makeMovePolicy(hot_b.id, warm.id).tiers_json;
    require(manager.updatePolicy(persisted.front()), "Lifecycle policy update failed");
    require(manager.resolveCameraRecordRoot(camera) == root + "/policy-hot-b",
            "Updated policy did not select HOT B record root");

    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const std::string segment_key = "2026-08-20/18-00-00";
    const std::string source = root + "/policy-hot-b/record/" + camera + "/main/" + segment_key + ".mp4";
    const std::string destination = root + "/nas/record/" + camera + "/main/" + segment_key + ".mp4";
    const std::string payload = "policy-updated-storage-path";
    writeFile(source, payload);
    // TimeRecorderManager keeps timeline indexes in the global metadata root,
    // while CameraOption::recordRootPath controls the physical MP4 location.
    writeTimeBlock(root, camera, "main", source,
                   now - 180, now - 120, payload.size());
    require(manager.registerHotSegmentRange(
                camera, "main", source, now - 180, now - 120, payload.size()),
            "Updated-policy HOT range registration failed");
    manager.runTieringCycle();
    require(waitUntil([&]() {
        auto jobs = manager.listTieringJobs(camera);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 5000), "Updated policy tiering job did not finish");
    require(File::loadFile(destination) == payload,
            "Updated policy did not move data from HOT B to WARM");
    drainWorkThreadPool();

    int camera_count = 0;
    bool is_default = false;
    require(!manager.deletePolicy(policy.id, camera_count, is_default) &&
            camera_count == 1 && !is_default,
            "Assigned policy deletion guard did not reject deletion");
    require(manager.removeCameraOverride(camera), "Cannot remove camera policy override");
    require(manager.getEffectivePolicy(camera).source == "SYSTEM_DEFAULT" &&
            manager.resolveCameraRecordRoot(camera) == root + "/hot",
            "Camera did not fall back to system default storage");
    require(manager.deletePolicy(policy.id, camera_count, is_default),
            "Unassigned policy could not be deleted");
    require(manager.getPolicy(policy.id).empty(), "Deleted policy is still queryable");
    manager.stop();
    return 0;
}

int scenarioResolveLocalPlaybackPaths(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-playback-path";
    const std::string stream = "main";
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    const std::string base = root + "/hot/record/" + camera + "/" + stream;
    const std::string existing = base + "/2026-08-27/10-00-00.mp4";
    const std::string empty = base + "/2026-08-27/10-01-00.mp4";
    const std::string symlink_path = base + "/2026-08-27/10-02-00.mp4";
    writeFile(existing, "playback-data");
    writeFile(empty, "");
    require(::symlink(existing.c_str(), symlink_path.c_str()) == 0,
            std::string("Cannot create playback symlink: ") + std::strerror(errno));

    const PlaybackPathResolveResult existing_result =
        manager.resolvePlaybackSegmentPath(camera, stream, now, existing);
    require(existing_result.ready && existing_result.fallback &&
                existing_result.read_path == existing && !existing_result.restore_required,
            "Existing playback file was not selected as fallback");

    const PlaybackPathResolveResult empty_result =
        manager.resolvePlaybackSegmentPath(camera, stream, now + 60, empty);
    require(empty_result.ready && empty_result.fallback &&
                empty_result.read_path == empty && !empty_result.restore_required,
            "Existing empty playback file was treated as missing");

    const PlaybackPathResolveResult symlink_result =
        manager.resolvePlaybackSegmentPath(camera, stream, now + 120, symlink_path);
    require(symlink_result.ready && symlink_result.fallback &&
                symlink_result.read_path == symlink_path && !symlink_result.restore_required,
            "Readable playback symlink was not followed");

    manager.stop();
    return 0;
}

int scenarioRejectUnreadablePlaybackPath(const std::string &root) {
    require(::chmod(root.c_str(), 0755) == 0,
            std::string("Cannot make test root traversable: ") + std::strerror(errno));
    const std::string unreadable = root + "/unreadable.mp4";
    writeFile(unreadable, "unreadable-playback-data");
    require(::chmod(unreadable.c_str(), 0000) == 0,
            std::string("Cannot remove playback file permissions: ") + std::strerror(errno));

    if (::geteuid() == 0) {
        // Restricted user namespaces may map only uid/gid 0. Signal the
        // wrapper to skip instead of pretending root can test unreadability.
        if (::setgid(65534) != 0 || ::setuid(65534) != 0)
            return 77;
    }

    const PlaybackPathResolveResult result =
        TierStorageManager::Instance().resolvePlaybackSegmentPath(
            "camera-unreadable", "main", static_cast<int64_t>(::time(nullptr)), unreadable);
    require(!result.ready && !result.fallback && result.read_path == unreadable,
            "Unreadable playback file was reported as ready");
    return 0;
}

int scenarioMissingRestoreCacheRequiresRestore(const std::string &root) {
    migrateDatabase(root);
    StoragePool cold_pool;
    cold_pool.id = "playback-cold-pool";
    cold_pool.name = "Playback COLD pool";
    cold_pool.type = poolTypeToString(StoragePoolType::MINIO);
    cold_pool.tier = tierTypeToString(ColdTier);
    cold_pool.enabled = 0;
    cold_pool.health_check_enabled = 0;
    require(StoragePoolImp().add(cold_pool),
            "Cannot persist disabled object pool for playback resolution");

    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-cold-playback";
    const std::string stream = "main";
    const std::string segment_path = "2026-08-27/11-00-00";
    const std::string original = root + "/hot/record/" + camera + "/" + stream +
                                 "/" + segment_path + ".mp4";
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    SegmentTierRange range;
    range.range_id = "playback-cold-range";
    range.camera_id = camera;
    range.stream_id = stream;
    range.tier = tierTypeToString(ColdTier);
    range.pool_id = cold_pool.id;
    range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
    range.start_time = now - 10;
    range.end_time = now + 10;
    range.segment_count = 1;
    range.size_bytes = 128;
    require(SegmentTierRangeImp().mergeOrInsert(range),
            "Cannot seed COLD range for playback resolution");
    require(!File::fileExist(manager.getRestoreSegmentPath(
                "missing-restore", camera, stream, segment_path)),
            "Restore cache unexpectedly exists before resolution");

    const PlaybackPathResolveResult result =
        manager.resolvePlaybackSegmentPath(camera, stream, now, original);
    require(!result.ready && result.restore_required && !result.fallback &&
                result.read_path.empty() && result.tier == tierTypeToString(ColdTier) &&
                result.pool_id == cold_pool.id && result.range_id == range.range_id,
            "Missing object restore cache did not request restore");

    RestoreJob older_job;
    older_job.job_id = "playback-restore-older";
    older_job.camera_id = camera;
    older_job.source_tier = tierTypeToString(ColdTier);
    older_job.target_tier = tierTypeToString(HotTier);
    older_job.status = jobStatusToString(JobStatus::DONE);
    older_job.start_time = range.start_time;
    older_job.end_time = range.end_time;
    older_job.created_at = now - 2;
    older_job.updated_at = now - 2;
    RestoreJobSegment older_child;
    older_child.job_id = older_job.job_id;
    older_child.camera_id = camera;
    older_child.stream_id = stream;
    older_child.range_id = range.range_id;
    older_child.segment_path = segment_path;
    older_child.source_pool_id = cold_pool.id;
    older_child.range_start = range.start_time;
    older_child.range_end = range.end_time;
    older_child.status = jobStatusToString(JobStatus::DONE);
    older_child.created_at = older_job.created_at;
    older_child.updated_at = older_job.updated_at;
    require(RestoreJobImp().addWithSegments(older_job, {older_child}),
            "Cannot seed older restore cache job");

    RestoreJob newer_job = older_job;
    newer_job.job_id = "playback-restore-newer";
    newer_job.created_at = now - 1;
    newer_job.updated_at = now - 1;
    RestoreJobSegment newer_child = older_child;
    newer_child.job_id = newer_job.job_id;
    newer_child.created_at = newer_job.created_at;
    newer_child.updated_at = newer_job.updated_at;
    require(RestoreJobImp().addWithSegments(newer_job, {newer_child}),
            "Cannot seed newer restore cache job");

    const std::string older_cache = manager.getRestoreSegmentPath(
        older_job.job_id, camera, stream, segment_path);
    const std::string newer_cache = manager.getRestoreSegmentPath(
        newer_job.job_id, camera, stream, segment_path);
    require(older_cache != newer_cache, "Restore jobs did not receive isolated paths");
    writeFile(older_cache, "older-cache");
    writeFile(newer_cache, "newer-cache");

    const PlaybackPathResolveResult newest_result =
        manager.resolvePlaybackSegmentPath(camera, stream, now, original);
    require(newest_result.ready && newest_result.read_path == newer_cache &&
                newest_result.restore_job_id == newer_job.job_id,
            "Playback did not select the newest valid restore job cache");

    require(::unlink(newer_cache.c_str()) == 0,
            "Cannot remove newer restore cache fixture");
    const PlaybackPathResolveResult fallback_result =
        manager.resolvePlaybackSegmentPath(camera, stream, now, original);
    require(fallback_result.ready && fallback_result.read_path == older_cache &&
                fallback_result.restore_job_id == older_job.job_id,
            "Playback did not fall back to an older valid restore job cache");

    RestoreJobSnapshot snapshot;
    require(manager.getRestoreJobSnapshot(newer_job.job_id, snapshot) &&
                snapshot.playback_status == "NOT_READY" && !snapshot.playback_ready,
            "Missing job-scoped cache was reported ready");
    require(manager.getRestoreJobSnapshot(older_job.job_id, snapshot) &&
                snapshot.playback_status == "READY" && snapshot.playback_ready &&
                snapshot.total_segment_count == 1 &&
                snapshot.restored_segment_count == 1 &&
                snapshot.remaining_segment_count == 0,
            "Complete job-scoped cache snapshot was not ready");

    manager.stop();
    return 0;
}

int scenarioValidatesRestoreWindowCache(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-restore-window";
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    SegmentTierRange main_range;
    main_range.range_id = "range-restore-main";
    main_range.camera_id = camera;
    main_range.stream_id = "main";
    main_range.tier = tierTypeToString(ColdTier);
    main_range.pool_id = "cold-pool";
    main_range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
    main_range.start_time = now - 120;
    main_range.end_time = now + 120;

    SegmentTierRange secondary_range = main_range;
    secondary_range.range_id = "range-restore-secondary";
    secondary_range.stream_id = "secondary";

    RestoreJob secondary_job;
    secondary_job.job_id = "restore-secondary-only";
    secondary_job.camera_id = camera;
    secondary_job.source_tier = tierTypeToString(ColdTier);
    secondary_job.target_tier = tierTypeToString(HotTier);
    secondary_job.status = jobStatusToString(JobStatus::DONE);
    secondary_job.start_time = now - 60;
    secondary_job.end_time = now + 60;
    secondary_job.created_at = now;
    secondary_job.updated_at = now;

    RestoreJobSegment secondary_child;
    secondary_child.job_id = secondary_job.job_id;
    secondary_child.camera_id = camera;
    secondary_child.stream_id = secondary_range.stream_id;
    secondary_child.range_id = secondary_range.range_id;
    secondary_child.segment_path = "2026-09-10/10-00-00";
    secondary_child.source_pool_id = secondary_range.pool_id;
    secondary_child.range_start = now - 30;
    secondary_child.range_end = now + 30;
    secondary_child.status = jobStatusToString(JobStatus::DONE);
    secondary_child.created_at = now;
    secondary_child.updated_at = now;
    require(RestoreJobImp().addWithSegments(secondary_job, {secondary_child}),
            "Cannot seed secondary restore job");
    writeFile(manager.getRestoreSegmentPath(
                  secondary_job.job_id, camera, secondary_child.stream_id,
                  secondary_child.segment_path),
              "secondary-cache");

    require(RestoreJobImp().queryOverlapping(
                camera, now - 10, now + 10, 0, "main").empty(),
            "Restore query reused a job from another stream");
    require(!RestoreJobImp().queryOverlapping(
                 camera, now - 10, now + 10, 0, "secondary").empty(),
            "Restore query did not return the matching stream job");

    int64_t expires_at = 0;
    require(!manager.validateRestoreWindowCache(
                secondary_job, {main_range}, "main", now - 10, now + 10,
                3600, expires_at),
            "Restore cache from another stream validated the main stream");

    RestoreJob main_job = secondary_job;
    main_job.job_id = "restore-main";
    RestoreJobSegment main_child = secondary_child;
    main_child.job_id = main_job.job_id;
    main_child.stream_id = main_range.stream_id;
    main_child.range_id = main_range.range_id;
    main_child.segment_path = "2026-09-10/10-01-00";
    require(RestoreJobImp().addWithSegments(main_job, {main_child}),
            "Cannot seed main restore job");
    const std::string main_cache = manager.getRestoreSegmentPath(
        main_job.job_id, camera, main_child.stream_id, main_child.segment_path);
    writeFile(main_cache, "main-cache");

    require(manager.validateRestoreWindowCache(
                main_job, {main_range}, "main", now - 10, now + 10,
                3600, expires_at) && expires_at == now + 3600,
            "Complete restore cache was not valid until the exact job expiry");
    require(manager.validateRestoreWindowCache(
                main_job, {main_range}, "main", now - 10, now + 10,
                -1, expires_at) && expires_at == 0,
            "Unlimited restore cache TTL did not return expires_at=0");

    RestoreJob multi_child_job = main_job;
    multi_child_job.job_id = "restore-main-multi-child";
    RestoreJobSegment done_child = main_child;
    done_child.job_id = multi_child_job.job_id;
    done_child.segment_path = "2026-09-10/10-02-00";
    RestoreJobSegment failed_child = done_child;
    failed_child.segment_path = "2026-09-10/10-03-00";
    failed_child.status = jobStatusToString(JobStatus::FAILED);
    require(RestoreJobImp().addWithSegments(
                multi_child_job, {done_child, failed_child}),
            "Cannot seed multi-child restore job");
    writeFile(manager.getRestoreSegmentPath(
                  multi_child_job.job_id, camera, done_child.stream_id,
                  done_child.segment_path),
              "done-child-cache");
    require(!manager.validateRestoreWindowCache(
                multi_child_job, {main_range}, "main", now - 10, now + 10,
                3600, expires_at),
            "Failed child in a covered range was ignored");

    require(!manager.validateRestoreWindowCache(
                main_job, {main_range, secondary_range}, "", now - 10, now + 10,
                3600, expires_at),
            "Partial multi-stream restore cache was reported complete");

    require(::unlink(main_cache.c_str()) == 0,
            "Cannot remove main restore cache fixture");
    require(!manager.validateRestoreWindowCache(
                main_job, {main_range}, "main", now - 10, now + 10,
                3600, expires_at),
            "Missing restore cache file was reported ready");

    writeFile(main_cache, "main-cache");
    RestoreJob expired_job = main_job;
    expired_job.updated_at = now - 3600;
    require(!manager.validateRestoreWindowCache(
                expired_job, {main_range}, "main", now - 10, now + 10,
                3600, expires_at),
            "Restore cache at its TTL boundary was reported ready");

    manager.stop();
    return 0;
}

int scenarioCleansJobScopedRestoreCache(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kRestoreTTLSeconds] = 120;
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const int64_t now = static_cast<int64_t>(::time(nullptr));
    RestoreJob job;
    job.job_id = "restore-cleanup-retry";
    job.camera_id = "camera-restore-cleanup";
    job.source_tier = tierTypeToString(ColdTier);
    job.target_tier = tierTypeToString(HotTier);
    job.status = jobStatusToString(JobStatus::DONE);
    job.start_time = now - 120;
    job.end_time = now - 60;
    job.created_at = now - 120;
    job.updated_at = now - 60;

    RestoreJobSegment child;
    child.job_id = job.job_id;
    child.camera_id = job.camera_id;
    child.stream_id = "main";
    child.range_id = "range-restore-cleanup";
    child.segment_path = "2026-09-10/11-00-00";
    child.source_pool_id = "cold-pool";
    child.range_start = job.start_time;
    child.range_end = job.end_time;
    child.status = jobStatusToString(JobStatus::DONE);
    child.created_at = job.created_at;
    child.updated_at = job.updated_at;
    require(RestoreJobImp().addWithSegments(job, {child}),
            "Cannot seed restore cleanup job");

    RestoreJob fresh_job = job;
    fresh_job.job_id = "restore-cleanup-fresh";
    fresh_job.created_at = now;
    fresh_job.updated_at = now;
    RestoreJobSegment fresh_child = child;
    fresh_child.job_id = fresh_job.job_id;
    fresh_child.created_at = now;
    fresh_child.updated_at = now;
    require(RestoreJobImp().addWithSegments(fresh_job, {fresh_child}),
            "Cannot seed fresh restore cleanup job");

    const std::string expired_cache_path = manager.getRestoreSegmentPath(
        job.job_id, child.camera_id, child.stream_id, child.segment_path);
    const std::string fresh_cache_path = manager.getRestoreSegmentPath(
        fresh_job.job_id, fresh_child.camera_id, fresh_child.stream_id,
        fresh_child.segment_path);
    require(expired_cache_path != fresh_cache_path,
            "Restore jobs shared the same cache path");
    writeFile(expired_cache_path, "expired-cache");
    writeFile(fresh_cache_path, "fresh-cache");

    require(manager.tryRetainRestoreCache(job.job_id),
            "Cannot retain ready restore cache");
    mINI::Instance()[Storage::kRestoreTTLSeconds] = 30;
    manager.runTieringCycle();
    drainWorkThreadPool();
    auto rows = RestoreJobImp().findByJobId(job.job_id);
    require(rows.size() == 1 && rows.front().status == jobStatusToString(JobStatus::DONE) &&
                File::loadFile(expired_cache_path) == "expired-cache",
            "Leased restore cache was deleted while an extractor could still use it");

    manager.releaseRestoreCache(job.job_id);
    manager.runTieringCycle();
    drainWorkThreadPool();
    rows = RestoreJobImp().findByJobId(job.job_id);
    require(rows.size() == 1 && rows.front().status == "EXPIRED" &&
                !File::fileExist(expired_cache_path),
            "Expired restore job directory was not cleaned");
    rows = RestoreJobImp().findByJobId(fresh_job.job_id);
    require(rows.size() == 1 && rows.front().status == jobStatusToString(JobStatus::DONE) &&
                File::loadFile(fresh_cache_path) == "fresh-cache",
            "Cleanup of one restore job affected another job cache");

    manager.stop();
    return 0;
}

int scenarioTracksPoolHealthAndBlocksPlayback(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    drainWorkThreadPool();

    const std::string pool_root = root + "/health-pool";
    makeDirectory(pool_root + "/record");
    StoragePool pool;
    pool.id = "health-local-pool";
    pool.name = "Health local pool";
    pool.type = poolTypeToString(StoragePoolType::LOCAL_DISK);
    pool.tier = tierTypeToString(WarmTier);
    pool.mount_path = Optional<std::string>(pool_root);
    pool.enabled = 1;
    pool.health_check_enabled = 1;
    require(manager.createPool(pool) == pool.id, "Cannot create health-check pool");
    drainWorkThreadPool();

    manager.checkAndRecordPoolHealth();
    PoolHealthInfo health;
    require(manager.getPoolHealth(pool.id, health) &&
                health.health_status != healthToString(StorageHealth::UNKNOWN) &&
                health.health_status != healthToString(StorageHealth::OFFLINE),
            "Successful initial probe did not mark pool online");

    removeTree(pool_root);
    manager.checkAndRecordPoolHealth();
    require(manager.getPoolHealth(pool.id, health) &&
                health.health_status != healthToString(StorageHealth::OFFLINE),
            "Pool became offline before reaching the failure threshold");
    manager.checkAndRecordPoolHealth();
    require(manager.getPoolHealth(pool.id, health) &&
                health.health_status == healthToString(StorageHealth::OFFLINE) &&
                !manager.isPoolAvailableForPlayback(pool, nullptr),
            "Pool was not marked offline after consecutive failures");

    const std::string camera = "camera-offline-playback";
    const std::string stream = "main";
    const std::string segment_path = "2026-09-05/10-00-00";
    const std::string full_path = pool_root + "/record/" + camera + "/" + stream +
                                  "/" + segment_path + ".mp4";
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    SegmentTierRange range;
    range.range_id = "range-offline-playback";
    range.camera_id = camera;
    range.stream_id = stream;
    range.tier = pool.tier;
    range.pool_id = pool.id;
    range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
    range.start_time = now - 10;
    range.end_time = now + 10;
    range.segment_count = 1;
    range.size_bytes = 128;
    require(SegmentTierRangeImp().mergeOrInsert(range), "Cannot seed offline playback range");

    const PlaybackPathResolveResult unavailable =
        manager.resolvePlaybackSegmentPath(camera, stream, now, full_path);
    require(!unavailable.ready && unavailable.storage_unavailable &&
                !unavailable.restore_required && unavailable.read_path.empty() &&
                unavailable.pool_health_status == healthToString(StorageHealth::OFFLINE),
            "Playback resolver did not report the offline storage pool");

    makeDirectory(pool_root + "/record");
    manager.checkAndRecordPoolHealth();
    require(manager.getPoolHealth(pool.id, health) &&
                health.health_status == healthToString(StorageHealth::OFFLINE),
            "Pool recovered before reaching the success threshold");
    manager.checkAndRecordPoolHealth();
    require(manager.getPoolHealth(pool.id, health) &&
                health.health_status != healthToString(StorageHealth::OFFLINE) &&
                manager.isPoolAvailableForPlayback(pool, nullptr),
            "Pool did not recover after consecutive successful probes");

    manager.stop();
    return 0;
}

int scenarioPoolHealthDefersTierMove(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    drainWorkThreadPool();

    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");

    StoragePool warm = makeHotPool(root, "health-gated-warm", "health-warm");
    warm.name = "Health-gated WARM pool";
    warm.tier = tierTypeToString(WarmTier);
    warm.health_check_enabled = 1;
    require(manager.createPool(warm) == warm.id, "Health-gated WARM pool creation failed");
    drainWorkThreadPool();

    const std::string warm_root = root + "/health-warm";
    removeTree(warm_root);
    manager.checkAndRecordPoolHealth();
    manager.checkAndRecordPoolHealth();
    PoolHealthInfo health;
    require(manager.getPoolHealth(warm.id, health) &&
                health.health_status == healthToString(StorageHealth::OFFLINE),
            "WARM pool did not become OFFLINE");

    const StoragePolicy policy = makeMovePolicy(hot_pools.front().id, warm.id);
    require(manager.createPolicy(policy) == policy.id, "Health-gated move policy creation failed");
    const std::string camera = "camera-health-gated-move";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "health gate integration"),
            "Health-gated policy assignment failed");

    const std::string segment_key = "2026-08-20/17-00-00";
    const std::string source = root + "/hot/record/" + camera + "/" + stream +
                               "/" + segment_key + ".mp4";
    const std::string destination = warm_root + "/record/" + camera + "/" + stream +
                                    "/" + segment_key + ".mp4";
    const std::string payload = "health-gated-tier-move";
    writeFile(source, payload);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    writeTimeBlock(root, camera, stream, source, now - 180, now - 120,
                   static_cast<int64_t>(payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, source, now - 180, now - 120,
                                            static_cast<int64_t>(payload.size())),
            "Health-gated HOT range registration failed");

    manager.runTieringCycle();
    drainWorkThreadPool();
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == jobStatusToString(JobStatus::PENDING),
            "Offline destination did not keep tiering job PENDING");
    require(File::fileExist(source) && !File::fileExist(destination),
            "Tier move performed I/O while destination was OFFLINE");

    makeDirectory(warm_root + "/record");
    manager.checkAndRecordPoolHealth();
    jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == jobStatusToString(JobStatus::PENDING),
            "Tiering job resumed before the pool recovery threshold");

    manager.runPoolHealthCycle();
    require(waitUntil([&]() {
        auto current = manager.listTieringJobs(camera);
        return current.size() == 1 && current.front().status == jobStatusToString(JobStatus::DONE);
    }, 5000), "Tiering job did not resume after destination recovery");
    require(!File::fileExist(source) && File::loadFile(destination) == payload,
            "Recovered tier move did not preserve destination data");

    manager.stop();
    return 0;
}

int scenarioPolicyReassignmentCancelsPendingJob(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    drainWorkThreadPool();

    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");

    StoragePool old_warm = makeHotPool(root, "policy-old-warm", "policy-old-warm");
    old_warm.tier = tierTypeToString(WarmTier);
    old_warm.health_check_enabled = 1;
    require(manager.createPool(old_warm) == old_warm.id, "Old WARM pool creation failed");

    StoragePool new_warm = makeHotPool(root, "policy-new-warm", "policy-new-warm");
    new_warm.tier = tierTypeToString(WarmTier);
    new_warm.health_check_enabled = 0;
    require(manager.createPool(new_warm) == new_warm.id, "New WARM pool creation failed");
    drainWorkThreadPool();

    const std::string old_warm_root = root + "/policy-old-warm";
    removeTree(old_warm_root);
    manager.checkAndRecordPoolHealth();
    manager.checkAndRecordPoolHealth();

    StoragePolicy old_policy = makeMovePolicy(hot_pools.front().id, old_warm.id);
    old_policy.id = "policy-old-route";
    require(manager.createPolicy(old_policy) == old_policy.id, "Old move policy creation failed");
    StoragePolicy new_policy = makeMovePolicy(hot_pools.front().id, new_warm.id);
    new_policy.id = "policy-new-route";
    require(manager.createPolicy(new_policy) == new_policy.id, "New move policy creation failed");

    const std::string camera = "camera-policy-reassignment";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, old_policy.id, "seed pending job"),
            "Old policy assignment failed");

    const std::string segment_key = "2026-08-20/18-00-00";
    const std::string source = root + "/hot/record/" + camera + "/" + stream +
                               "/" + segment_key + ".mp4";
    const std::string old_destination = old_warm_root + "/record/" + camera + "/" +
                                        stream + "/" + segment_key + ".mp4";
    const std::string new_destination = root + "/policy-new-warm/record/" + camera +
                                        "/" + stream + "/" + segment_key + ".mp4";
    const std::string payload = "policy-reassignment-tier-move";
    writeFile(source, payload);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    writeTimeBlock(root, camera, stream, source, now - 180, now - 120,
                   static_cast<int64_t>(payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, source, now - 180, now - 120,
                                            static_cast<int64_t>(payload.size())),
            "Policy reassignment range registration failed");

    manager.runTieringCycle();
    drainWorkThreadPool();
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == jobStatusToString(JobStatus::PENDING) &&
                jobs.front().job_trigger == tieringJobTriggerToString(TieringJobTrigger::RETENTION) &&
                jobs.front().policy_id == old_policy.id,
            "Old policy did not leave one contextual PENDING job");

    require(manager.assignPolicyToCamera(camera, new_policy.id, "change destination"),
            "New policy assignment failed");
    manager.runTieringCycle();
    drainWorkThreadPool();
    jobs = manager.listTieringJobs(camera);
    bool found_cancelled = false;
    for (const auto &job : jobs) {
        if (job.policy_id == old_policy.id &&
            job.status == jobStatusToString(JobStatus::CANCELLED) &&
            job.error_message.value_or("").find("POLICY_CHANGED:") == 0) {
            found_cancelled = true;
        }
    }
    require(found_cancelled,
            "Reassigned policy did not cancel the stale PENDING job");
    require(!File::fileExist(old_destination),
            "Stale job wrote to the old destination after policy reassignment");

    bool replacement_done = false;
    for (const auto &job : jobs) {
        replacement_done = replacement_done ||
            (job.status == jobStatusToString(JobStatus::DONE) &&
             job.target_pool_id == new_warm.id && job.policy_id == new_policy.id);
    }
    if (!replacement_done)
        manager.runTieringCycle();
    require(waitUntil([&]() {
        auto current = manager.listTieringJobs(camera);
        if (current.size() != 2)
            return false;
        bool cancelled = false;
        bool done = false;
        for (const auto &job : current) {
            cancelled = cancelled || job.status == jobStatusToString(JobStatus::CANCELLED);
            done = done || (job.status == jobStatusToString(JobStatus::DONE) &&
                            job.target_pool_id == new_warm.id &&
                            job.policy_id == new_policy.id);
        }
        return cancelled && done;
    }, 5000), "A replacement job was not completed with the reassigned policy");
    drainWorkThreadPool();
    require(!File::fileExist(source) && File::loadFile(new_destination) == payload,
            "Replacement job did not move data to the reassigned destination");

    manager.stop();
    return 0;
}

int scenarioPolicyRetentionChangeCancelsPendingJob(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    drainWorkThreadPool();

    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");
    StoragePool warm = makeHotPool(root, "policy-retention-warm", "policy-retention-warm");
    warm.tier = tierTypeToString(WarmTier);
    warm.health_check_enabled = 1;
    require(manager.createPool(warm) == warm.id, "Retention WARM pool creation failed");
    drainWorkThreadPool();
    removeTree(root + "/policy-retention-warm");
    manager.checkAndRecordPoolHealth();
    manager.checkAndRecordPoolHealth();

    StoragePolicy policy = makeMovePolicy(hot_pools.front().id, warm.id);
    policy.id = "policy-retention-change";
    require(manager.createPolicy(policy) == policy.id, "Retention policy creation failed");
    const std::string camera = "camera-policy-retention-change";
    const std::string stream = "main";
    require(manager.assignPolicyToCamera(camera, policy.id, "seed pending job"),
            "Retention policy assignment failed");

    const std::string source = root + "/hot/record/" + camera + "/" + stream +
                               "/2026-08-20/19-00-00.mp4";
    const std::string payload = "policy-retention-change";
    writeFile(source, payload);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    writeTimeBlock(root, camera, stream, source, now - 180, now - 120,
                   static_cast<int64_t>(payload.size()));
    require(manager.registerHotSegmentRange(camera, stream, source, now - 180, now - 120,
                                            static_cast<int64_t>(payload.size())),
            "Retention change range registration failed");

    manager.runTieringCycle();
    drainWorkThreadPool();
    auto jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == jobStatusToString(JobStatus::PENDING),
            "Retention change fixture did not create a PENDING job");

    auto persisted = manager.getPolicy(policy.id);
    require(persisted.size() == 1, "Cannot reload retention policy");
    auto tiers = persisted.front().parsedTiers();
    require(!tiers.empty(), "Retention policy has no tiers");
    for (auto &tier : tiers) {
        if (tier.tier == tierTypeToString(HotTier))
            tier.retain_until_days = 30;
    }
    persisted.front().tiers_json = StoragePolicy::tiersToJson(tiers);
    require(manager.updatePolicy(persisted.front()), "Retention policy update failed");

    manager.runTieringCycle();
    drainWorkThreadPool();
    jobs = manager.listTieringJobs(camera);
    require(jobs.size() == 1 && jobs.front().status == jobStatusToString(JobStatus::CANCELLED) &&
                jobs.front().error_message.value_or("").find("POLICY_CHANGED:") == 0,
            "Retention increase did not cancel the no-longer-eligible job");
    require(File::fileExist(source), "Cancelled retention job removed source data");

    manager.stop();
    return 0;
}

int scenarioPoolHealthGuardsDeletion(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    drainWorkThreadPool();

    StoragePool hot = makeHotPool(root, "health-gated-hot", "health-hot");
    hot.health_check_enabled = 1;
    require(manager.createPool(hot) == hot.id, "Health-gated HOT pool creation failed");
    drainWorkThreadPool();

    const std::string hot_root = root + "/health-hot";
    removeTree(hot_root);
    manager.checkAndRecordPoolHealth();
    manager.checkAndRecordPoolHealth();
    PoolHealthInfo health;
    require(manager.getPoolHealth(hot.id, health) &&
                health.health_status == healthToString(StorageHealth::OFFLINE),
            "HOT pool did not become OFFLINE");

    const std::string camera = "camera-health-gated-delete";
    const std::string stream = "main";
    const std::string date = "2020-01-02";
    const std::string segment_name = "2020-01-02-12-00-00-0";
    const std::string orphan_name = "2020-01-02-12-01-00-0";
    const std::string segment_path = date + "/" + segment_name;
    const std::string source = hot_root + "/record/" + camera + "/" + stream +
                               "/" + segment_path + ".mp4";
    const std::string orphan = hot_root + "/record/" + camera + "/" + stream +
                               "/" + date + "/" + orphan_name + ".mp4";
    writeFile(source, "health-gated-delete");
    writeFile(orphan, "health-gated-orphan");

    const int64_t start = static_cast<int64_t>(
        StrTimeUtils::getTsFromDateTimeStr2(segment_path));
    writeDailyTimeBlock(root, camera, stream, date, source, start, start + 60,
                        static_cast<int64_t>(std::strlen("health-gated-delete")));
    const std::string keeper_path = date + "/2020-01-02-13-00-00-0";
    writeDailyTimeBlock(root, camera, stream, date, keeper_path, start + 3600,
                        start + 3660, 1);

    StoragePolicy policy = makeHotOnlyPolicy(hot.id, "health-gated-delete-policy", 1);
    require(manager.createPolicy(policy) == policy.id, "Health-gated delete policy creation failed");
    require(manager.assignPolicyToCamera(camera, policy.id, "health gate deletion"),
            "Health-gated delete policy assignment failed");

    SegmentTierRange range;
    range.range_id = "health-gated-delete-range";
    range.camera_id = camera;
    range.stream_id = stream;
    range.tier = tierTypeToString(HotTier);
    range.pool_id = hot.id;
    range.status = segmentStatusToString(SegmentStatus::EXPIRED);
    range.start_time = start;
    range.end_time = start + 60;
    range.segment_count = 1;
    range.size_bytes = static_cast<int64_t>(std::strlen("health-gated-delete"));
    require(SegmentTierRangeImp().mergeOrInsert(range), "Cannot seed health-gated delete range");

    manager.runTieringCycle();
    drainWorkThreadPool();
    require(File::fileExist(orphan), "Orphan cleanup scanned an OFFLINE HOT pool");
    require(!manager.approveExpiredRange(range.range_id),
            "Range deletion succeeded while source pool was OFFLINE");
    require(File::fileExist(source), "Range data was deleted while source pool was OFFLINE");

    manager.checkAndRecordPoolHealth();
    manager.checkAndRecordPoolHealth();
    require(manager.getPoolHealth(hot.id, health) &&
                health.health_status != healthToString(StorageHealth::OFFLINE),
            "HOT pool did not recover");
    require(manager.approveExpiredRange(range.range_id),
            "Range deletion did not resume after source pool recovery");
    require(!File::fileExist(source), "Recovered range deletion left source data behind");

    runTemporaryMp4CleanupPass(manager);
    require(!File::fileExist(orphan), "Recovered orphan cleanup did not delete orphan MP4");

    manager.stop();
    return 0;
}

int scenarioBackfillAtomicIdempotent(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kTierRangeBackfillEnabled] = 1;
    mINI::Instance()[Storage::kTierRangeBackfillChunkSeconds] = 3600;
    mINI::Instance()[Storage::kTierRangeBackfillWorkBudgetMs] = 5000;

    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-backfill-idempotent";
    const std::string stream = "main";
    const int64_t start = static_cast<int64_t>(
        StampUtils::getStartOfDay(static_cast<uint64_t>(::time(nullptr) - 86400))) + 3600;
    const int64_t middle = start + 60;
    const int64_t end = middle + 60;
    const std::string first_key = "2026-08-28_12-00-00";
    const std::string second_key = "2026-08-28_12-01-00";
    const std::string first_path = root + "/hot/record/" + camera + "/" +
                                   stream + "/" + first_key + ".mp4";
    const std::string second_path = root + "/hot/record/" + camera + "/" +
                                    stream + "/" + second_key + ".mp4";
    const std::string first_payload = "already-indexed";
    const std::string second_payload = "durable-backfill";
    writeFile(first_path, first_payload);
    writeFile(second_path, second_payload);
    writeTimeBlock(root, camera, stream, first_path, start, middle,
                   static_cast<int64_t>(first_payload.size()));
    writeTimeBlock(root, camera, stream, second_path, middle, end,
                   static_cast<int64_t>(second_payload.size()));
    require(manager.registerHotSegmentRange(
                camera, stream, first_path, start, middle,
                static_cast<int64_t>(first_payload.size())),
            "Cannot seed partially indexed HOT metadata");

    TierBackfillTask desired;
    desired.camera_id = camera;
    desired.timeline_path = root + "/hot/record/" + camera;
    desired.window_start = start;
    desired.cutoff_time = end;
    desired.cursor_time = start;
    TierBackfillTask task = TierBackfillTaskImp().ensureTask(desired);
    require(!task.task_id.empty(), "Cannot seed durable backfill task");

    manager.runBackfillCycle();
    require(waitUntil([&]() {
        auto rows = TierBackfillTaskImp().findByCameraId(camera);
        return rows.size() == 1 && rows.front().status == "COMPLETED";
    }, 5000), "Backfill task did not complete");

    auto ranges = SegmentTierRangeImp().queryByCamera(camera, start - 1, end + 1);
    const int64_t expected_size = static_cast<int64_t>(first_payload.size() +
                                                        second_payload.size());
    require(ranges.size() == 1 && ranges.front().segment_count == 2 &&
                ranges.front().size_bytes == expected_size,
            "Backfill inflated the partially indexed HOT range");

    task = TierBackfillTaskImp().findByCameraId(camera).front();
    task.cursor_time = start;
    task.status = "PENDING";
    task.completed_at = 0;
    task.updated_at = static_cast<int64_t>(::time(nullptr));
    require(TierBackfillTaskImp().updateDefinitionAt(task),
            "Cannot replay completed backfill task");
    manager.runBackfillCycle();
    require(waitUntil([&]() {
        auto rows = TierBackfillTaskImp().findByCameraId(camera);
        return rows.size() == 1 && rows.front().status == "COMPLETED";
    }, 5000), "Replayed backfill task did not complete");

    ranges = SegmentTierRangeImp().queryByCamera(camera, start - 1, end + 1);
    require(ranges.size() == 1 && ranges.front().segment_count == 2 &&
                ranges.front().size_bytes == expected_size,
            "Backfill replay inflated normalized range counters");

    manager.stop();
    drainWorkThreadPool();
    return 0;
}

int scenarioBackfillUsesLocalDayBuckets(const std::string &root) {
    require(::setenv("TZ", "Asia/Ho_Chi_Minh", 1) == 0,
            "Cannot configure test timezone");
    ::tzset();
    migrateDatabase(root);
    mINI::Instance()[Storage::kTierRangeBackfillEnabled] = 1;
    mINI::Instance()[Storage::kTierRangeBackfillChunkSeconds] = 180;
    mINI::Instance()[Storage::kTierRangeBackfillWorkBudgetMs] = 5000;

    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-backfill-local-day";
    const std::string stream = "main";
    const int64_t local_midnight = static_cast<int64_t>(
        StampUtils::getStartOfDay(static_cast<uint64_t>(::time(nullptr) - 86400)));
    const int64_t first_start = local_midnight - 60;
    const int64_t first_end = local_midnight;
    const int64_t second_start = local_midnight;
    const int64_t second_end = local_midnight + 60;
    const std::string first_path = root + "/hot/record/" + camera + "/" +
                                   stream + "/before-local-midnight.mp4";
    const std::string second_path = root + "/hot/record/" + camera + "/" +
                                    stream + "/after-local-midnight.mp4";
    writeTimeBlock(root, camera, stream, first_path, first_start, first_end, 100);
    writeTimeBlock(root, camera, stream, second_path, second_start, second_end, 100);

    TierBackfillTask desired;
    desired.camera_id = camera;
    desired.timeline_path = root + "/hot/record/" + camera;
    desired.window_start = first_start;
    desired.cutoff_time = second_end;
    desired.cursor_time = first_start;
    TierBackfillTask task = TierBackfillTaskImp().ensureTask(desired);
    require(!task.task_id.empty(), "Cannot seed local-day backfill task");

    manager.runBackfillCycle();
    require(waitUntil([&]() {
        auto rows = TierBackfillTaskImp().findByCameraId(camera);
        return rows.size() == 1 && rows.front().status == "COMPLETED";
    }, 5000), "Local-day backfill task did not complete");

    auto ranges = SegmentTierRangeImp().queryByCamera(
        camera, first_start - 1, second_end + 1);
    require(ranges.size() == 2 &&
                ranges[0].start_time == first_start &&
                ranges[1].start_time == second_start,
            "Backfill merged ranges across local midnight");

    manager.stop();
    drainWorkThreadPool();
    return 0;
}

int scenarioCompactRangeRepository(const std::string &root) {
    require(::setenv("TZ", "Asia/Ho_Chi_Minh", 1) == 0,
            "Cannot configure test timezone");
    ::tzset();
    migrateDatabase(root);
    const int64_t day = static_cast<int64_t>(
        StampUtils::getStartOfDay(static_cast<uint64_t>(::time(nullptr) - 2 * 86400)));
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    SegmentTierRangeImp range_imp;

    auto make_range = [&](const std::string &id, const std::string &camera,
                          const std::string &tier, int64_t start) {
        SegmentTierRange range;
        range.range_id = id;
        range.camera_id = camera;
        range.stream_id = "main";
        range.tier = tier;
        range.pool_id = tier == "HOT" ? "hot-pool" : "warm-pool";
        range.status = segmentStatusToString(SegmentStatus::AVAILABLE);
        range.start_time = start;
        range.end_time = start + 60;
        range.segment_count = 1;
        range.size_bytes = 100;
        range.created_at = now;
        range.updated_at = now;
        return range;
    };

    require(range_imp.mergeOrInsert(make_range("day-a", "camera-day", "HOT", day + 60)),
            "Cannot insert first daily range");
    require(range_imp.mergeOrInsert(make_range("day-b", "camera-day", "HOT", day + 120)),
            "Cannot merge same-day range");
    require(range_imp.mergeOrInsert(make_range("day-c", "camera-day", "HOT", day + 86400)),
            "Cannot insert next-day range");
    auto daily = range_imp.queryByCamera("camera-day");
    require(daily.size() == 2 && daily.front().segment_count == 2,
            "Compact ranges crossed a date bucket or failed to merge within it");

    require(range_imp.mergeOrInsert(make_range(
                "local-day-a", "camera-local-day", "HOT", day + 86400 - 60)),
            "Cannot insert range before local midnight");
    require(range_imp.mergeOrInsert(make_range(
                "local-day-b", "camera-local-day", "HOT", day + 86400)),
            "Cannot insert range after local midnight");
    auto local_day_ranges = range_imp.queryByCamera("camera-local-day");
    require(local_day_ranges.size() == 2,
            "Compact ranges crossed local midnight");

    require(range_imp.add(make_range("move-hot", "camera-move", "HOT", day + 600)),
            "Cannot seed source range");
    require(range_imp.add(make_range("move-warm", "camera-move", "WARM", day + 660)),
            "Cannot seed destination range");
    require(range_imp.updateTierByRangeId("move-hot", "WARM", "warm-pool",
                                          segmentStatusToString(SegmentStatus::AVAILABLE)),
            "Cannot update moved range tier");
    require(range_imp.normalizeRangeById("move-hot", 90, now),
            "Cannot normalize moved range");
    auto moved = range_imp.queryByCamera("camera-move");
    require(moved.size() == 1 && moved.front().segment_count == 2 &&
                moved.front().size_bytes == 200,
            "Adjacent destination ranges were not compacted after move");

    for (int index = 0; index < 5; ++index) {
        require(range_imp.add(make_range(
                    "page-" + std::to_string(index), "camera-page", "HOT",
                    day + 1200 + index * 300)),
                "Cannot seed paginated range");
    }
    require(range_imp.findByCameraTierStartedBefore(
                "camera-page", "HOT", day + 86400, 2).size() == 2,
            "Eligible range query ignored its per-cycle limit");
    require(range_imp.add(make_range(
                "other-camera", "camera-other", "HOT", day + 300)),
            "Cannot seed an earlier range for another camera");
    auto pressure_page = range_imp.findByCameraTierPoolAndAge(
        "camera-page", "HOT", "hot-pool", day + 86400, 2);
    require(pressure_page.size() == 2 &&
                pressure_page.front().camera_id == "camera-page" &&
                pressure_page.back().camera_id == "camera-page",
            "Pressure range query applied its limit before filtering camera");

    SegmentTierRange terminal = make_range("terminal-range", "camera-terminal", "HOT", day);
    terminal.status = segmentStatusToString(SegmentStatus::DELETED);
    terminal.updated_at = day;
    require(range_imp.add(terminal), "Cannot seed terminal range");

    TierMetadataCleanupJob cleanup_job;
    cleanup_job.range_id = terminal.range_id;
    cleanup_job.camera_id = terminal.camera_id;
    cleanup_job.status = "PENDING";
    cleanup_job.created_at = day;
    cleanup_job.updated_at = day;
    TierMetadataCleanupJobRepository cleanup_repo;
    require(cleanup_repo.add(cleanup_job),
            "Cannot seed pending metadata cleanup job");
    range_imp.pruneDeletedOlderThan(day + 1);
    require(!range_imp.findByRangeIds({terminal.range_id}).empty(),
            "Terminal range was pruned while metadata cleanup was pending");
    require(cleanup_repo.updateResultAt(terminal.range_id, "DONE", day),
            "Cannot complete metadata cleanup job");
    range_imp.pruneDeletedOlderThan(day + 1);
    require(range_imp.findByRangeIds({terminal.range_id}).empty(),
            "Terminal range retention did not prune old metadata");
    cleanup_repo.pruneDoneOlderThan(day + 1);
    auto cleanup_rows = cleanup_repo.getExecutor()->executeRaw(
        toolkit::QueryBuilder()
            .select({"range_id"})
            .from(EntityTraits<TierMetadataCleanupJob>::tableName())
            .where("range_id = ?", {terminal.range_id}));
    require(cleanup_rows.empty(),
            "Completed metadata cleanup job was not pruned");

    TieringJob job;
    job.job_id = "terminal-job";
    job.camera_id = "camera-terminal";
    job.stream_id = "main";
    job.source_tier = "HOT";
    job.target_tier = "WARM";
    job.source_pool_id = "hot-pool";
    job.target_pool_id = "warm-pool";
    job.status = jobStatusToString(JobStatus::DONE);
    job.created_at = day;
    job.updated_at = day;
    require(TieringJobImp().add(job), "Cannot seed terminal job");
    TieringJobImp().pruneTerminalOlderThan(day + 1);
    require(TieringJobImp().findByJobId(job.job_id).empty(),
            "Tiering job retention did not prune old history");
    return 0;
}

int scenarioBackfillOrphanRetriesWithoutAdvancing(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kTierRangeBackfillEnabled] = 1;
    mINI::Instance()[Storage::kTierRangeBackfillChunkSeconds] = 3600;
    mINI::Instance()[Storage::kTierRangeBackfillRetryBaseSeconds] = 60;
    mINI::Instance()[Storage::kTierRangeBackfillMaxAttempts] = 3;

    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-backfill-orphan";
    const int64_t start = static_cast<int64_t>(::time(nullptr)) - 3600;
    const int64_t end = start + 60;
    const std::string unmatched = root + "/unmapped/record/" + camera +
                                  "/main/2026-08-28_12-01-00.mp4";
    writeTimeBlock(root, camera, "main", unmatched, start, end, 128);

    TierBackfillTask desired;
    desired.camera_id = camera;
    desired.timeline_path = root + "/hot/record/" + camera;
    desired.window_start = start;
    desired.cutoff_time = end;
    desired.cursor_time = start;
    require(!TierBackfillTaskImp().ensureTask(desired).task_id.empty(),
            "Cannot seed orphan backfill task");

    manager.runBackfillCycle();
    require(waitUntil([&]() {
        auto rows = TierBackfillTaskImp().findByCameraId(camera);
        return rows.size() == 1 && rows.front().status == "RETRY";
    }, 5000), "Orphan backfill task did not enter RETRY");
    auto rows = TierBackfillTaskImp().findByCameraId(camera);
    require(rows.front().cursor_time == start && rows.front().attempt_count == 1 &&
                rows.front().orphan_blocks == 1 && !rows.front().last_error.empty(),
            "Orphan retry advanced cursor or lost diagnostic counters");
    require(SegmentTierRangeImp().queryByCamera(camera).empty(),
            "Orphan chunk partially committed range metadata");

    manager.stop();
    drainWorkThreadPool();
    return 0;
}

int scenarioBackfillMissingTimelineCompletes(const std::string &root) {
    migrateDatabase(root);
    mINI::Instance()[Storage::kTierRangeBackfillEnabled] = 1;
    mINI::Instance()[Storage::kTierRangeBackfillRetryBaseSeconds] = 60;
    mINI::Instance()[Storage::kTierRangeBackfillMaxAttempts] = 3;

    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string camera = "camera-backfill-no-timefile";
    const int64_t start = static_cast<int64_t>(::time(nullptr)) - 3600;
    TierBackfillTask desired;
    desired.camera_id = camera;
    desired.timeline_path = root + "/hot/record/" + camera;
    desired.window_start = start;
    desired.cutoff_time = start + 60;
    desired.cursor_time = start;
    require(!TierBackfillTaskImp().ensureTask(desired).task_id.empty(),
            "Cannot seed missing-timeline backfill task");

    manager.runBackfillCycle();
    require(waitUntil([&]() {
        auto rows = TierBackfillTaskImp().findByCameraId(camera);
        return rows.size() == 1 && rows.front().status == "COMPLETED";
    }, 5000), "Missing timeline did not complete as an empty backfill task");
    auto rows = TierBackfillTaskImp().findByCameraId(camera);
    require(rows.front().cursor_time == desired.cutoff_time &&
                rows.front().attempt_count == 0 && rows.front().last_error.empty(),
            "Empty timeline did not advance cleanly to the backfill cutoff");
    require(SegmentTierRangeImp().queryByCamera(camera).empty(),
            "Empty timeline unexpectedly created tier range metadata");

    manager.stop();
    drainWorkThreadPool();
    return 0;
}

int scenarioRecoveryPrepare(const std::string &root) {
    migrateDatabase(root);
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    TieringJob job;
    job.job_id = "recovery-tier-job";
    job.camera_id = "camera-recovery";
    job.stream_id = "main";
    job.range_id = "range-recovery";
    job.source_tier = "HOT";
    job.target_tier = "WARM";
    job.source_pool_id = "source-pool";
    job.target_pool_id = "target-pool";
    job.status = "RUNNING";
    job.created_at = now;
    job.updated_at = now;
    require(TieringJobImp().add(job), "Cannot seed RUNNING tiering job");

    RestoreJob restore;
    restore.job_id = "recovery-restore-job";
    restore.camera_id = "camera-recovery";
    restore.source_tier = "COLD";
    restore.target_tier = "HOT";
    restore.status = "RUNNING";
    restore.created_at = now;
    restore.updated_at = now;
    RestoreJobSegment segment;
    segment.job_id = restore.job_id;
    segment.camera_id = restore.camera_id;
    segment.stream_id = "main";
    segment.range_id = "range-recovery";
    segment.segment_path = "segment-recovery";
    segment.source_pool_id = "cold-pool";
    segment.status = "RUNNING";
    segment.created_at = now;
    segment.updated_at = now;
    require(RestoreJobImp().addWithSegments(restore, {segment}),
            "Cannot seed RUNNING restore job segment");

    TierBackfillTask backfill;
    backfill.task_id = "recovery-backfill-task";
    backfill.camera_id = "camera-recovery-backfill";
    backfill.timeline_path = root + "/hot/record/camera-recovery-backfill";
    backfill.window_start = now - 3600;
    backfill.cutoff_time = now - 120;
    backfill.cursor_time = backfill.window_start;
    backfill.status = "RUNNING";
    backfill.started_at = now;
    backfill.created_at = now;
    backfill.updated_at = now;
    require(TierBackfillTaskImp().insertTask(backfill),
            "Cannot seed RUNNING backfill task");
    return 0;
}

int scenarioRecoveryVerify(const std::string &root) {
    migrateDatabase(root);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();
    require(waitUntil([&]() {
        auto jobs = TieringJobImp().findByJobId("recovery-tier-job");
        return jobs.size() == 1 && jobs.front().status == "FAILED";
    }, 5000),
            "Recovered tiering job was not automatically dispatched");
    auto segments = RestoreJobSegmentImp().queryByJob("recovery-restore-job");
    require(segments.size() == 1 && segments.front().status == "PENDING",
            "RUNNING restore segment was not recovered to PENDING");
    auto backfill = TierBackfillTaskImp().findByTaskId("recovery-backfill-task");
    require(backfill.size() == 1 && backfill.front().status == "PENDING" &&
                backfill.front().cursor_time == backfill.front().window_start,
            "RUNNING backfill task was not recovered without advancing cursor");
    manager.stop();
    return 0;
}

void runTemporaryMp4CleanupPass(TierStorageManager &manager) {
    // Pool metrics use a (pool_id, epoch-second) primary key. Production
    // maintenance runs every five minutes, so keep test-triggered cycles on
    // distinct seconds as well.
    const time_t current_second = ::time(nullptr);
    while (::time(nullptr) == current_second)
        ::usleep(1000);
    manager.runTieringCycle();
    drainWorkThreadPool();
}

int scenarioTemporaryMp4CleanupStableStale(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    StoragePool second_hot = makeHotPool(root, "tmp-cleanup-hot-two", "hot-two");
    require(manager.createPool(second_hot) == second_hot.id,
            "Cannot create second HOT pool for temporary cleanup");

    const std::string first = root +
        "/hot/record/camera-tmp/main/2020-01-02/.2020-01-02-12-00-00-0.mp4";
    const std::string second = root +
        "/hot-two/record/camera-tmp-two/main/2020-01-02/.2020-01-02-12-01-00-0.mp4";
    writeFile(first, "abandoned-temporary-one");
    writeFile(second, "abandoned-temporary-two");
    const time_t stale_time = ::time(nullptr) - 7200;
    setFileModifiedTime(first, stale_time);
    setFileModifiedTime(second, stale_time);

    runTemporaryMp4CleanupPass(manager);
    require(File::fileExist(first) && File::fileExist(second),
            "Temporary MP4 was deleted on its first observation");

    runTemporaryMp4CleanupPass(manager);
    require(!File::fileExist(first) && !File::fileExist(second),
            "Stable stale temporary MP4 was not deleted from every HOT pool");
    require(SegmentTierRangeImp().queryByCamera("camera-tmp").empty() &&
                SegmentTierRangeImp().queryByCamera("camera-tmp-two").empty(),
            "Temporary MP4 cleanup created tier ranges");

    manager.stop();
    return 0;
}

int scenarioTemporaryMp4CleanupPreservesUnsafe(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string date_root = root + "/hot/record/camera-safe/main/2020-01-02";
    const std::string changing = date_root + "/.2020-01-02-13-00-00-0.mp4";
    const std::string current_date = getTimeStr("%Y-%m-%d");
    const std::string current_segment = getTimeStr("%Y-%m-%d-%H-%M-%S");
    const std::string recent_root = root + "/hot/record/camera-safe/main/" + current_date;
    const std::string fresh = recent_root + "/." + current_segment + "-0.mp4";
    const std::string fresh_normal = recent_root + "/" + current_segment + "-1.mp4";
    const std::string malformed = date_root + "/.not-a-segment.mp4";
    const std::string symlink_target = root + "/symlink-target.mp4";
    const std::string symlink_path = date_root + "/.2020-01-02-13-03-00-0.mp4";
    writeFile(changing, "initial");
    writeFile(fresh, "active-temporary");
    writeFile(fresh_normal, "fresh-normal-without-timefile");
    writeFile(malformed, "malformed-hidden-mp4");
    writeFile(symlink_target, "symlink-target");
    require(::symlink(symlink_target.c_str(), symlink_path.c_str()) == 0,
            "Cannot create temporary MP4 symlink");

    const std::string external_date = root + "/external-date";
    const std::string external_file = external_date + "/.2020-01-03-14-00-00-0.mp4";
    writeFile(external_file, "outside-directory-symlink");
    const std::string linked_date = root + "/hot/record/camera-safe/main/2020-01-03";
    require(::symlink(external_date.c_str(), linked_date.c_str()) == 0,
            "Cannot create date directory symlink");

    const time_t stale_time = ::time(nullptr) - 7200;
    setFileModifiedTime(changing, stale_time);
    setFileModifiedTime(malformed, stale_time);
    setFileModifiedTime(symlink_target, stale_time);
    setFileModifiedTime(external_file, stale_time);

    runTemporaryMp4CleanupPass(manager);
    writeFile(changing, "changed-between-observations");
    runTemporaryMp4CleanupPass(manager);

    require(File::fileExist(changing), "Changing temporary MP4 was deleted");
    require(File::fileExist(fresh), "Temporary MP4 inside retention was deleted");
    require(File::fileExist(fresh_normal), "Completed MP4 inside retention was deleted");
    require(File::fileExist(malformed), "Malformed hidden MP4 was deleted");
    require(File::fileExist(symlink_path) && File::fileExist(symlink_target),
            "Temporary MP4 symlink or its target was deleted");
    require(File::fileExist(external_file), "Date directory symlink was followed");

    manager.stop();
    return 0;
}

int scenarioCompletedMp4Cleanup(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string date = "2020-01-02";
    const std::string missing_timefile = root +
        "/hot/record/camera-missing-timefile/main/" + date + "/2020-01-02-13-59-00-0.mp4";
    const std::string missing_daily_timefile = root +
        "/hot/record/camera-missing-daily-timefile/main/" + date +
        "/2020-01-02-13-58-00-0.mp4";
    const std::string orphan = root +
        "/hot/record/camera-completed/main/" + date + "/2020-01-02-14-00-00-0.mp4";
    const std::string referenced = root +
        "/hot/record/camera-completed/main/" + date + "/2020-01-02-14-01-00-0.mp4";
    const std::string relay_referenced = root +
        "/hot/record/camera-completed/main.relay.fixture/" + date +
        "/2020-01-02-14-02-00-0.mp4";
    writeFile(missing_timefile, "completed-without-timefile");
    writeFile(missing_daily_timefile, "completed-without-daily-timefile");
    writeFile(orphan, "completed-without-timeblock");
    writeFile(referenced, "completed-with-timeblock");
    writeFile(relay_referenced, "completed-relay-with-timeblock");

    const std::string other_date = "2020-01-03";
    const std::string other_segment = root +
        "/hot/record/camera-missing-daily-timefile/main/" + other_date +
        "/2020-01-03-00-00-00-0.mp4";
    const int64_t other_start = static_cast<int64_t>(
        StrTimeUtils::getTsFromDateTimeStr2(other_date + "/2020-01-03-00-00-00-0"));
    writeDailyTimeBlock(root, "camera-missing-daily-timefile", "main", other_date,
                        other_segment, other_start, other_start + 60, 1);

    const int64_t referenced_start = static_cast<int64_t>(
        StrTimeUtils::getTsFromDateTimeStr2(date + "/2020-01-02-14-01-00-0"));
    writeDailyTimeBlock(root, "camera-completed", "main", date, referenced,
                        referenced_start, referenced_start + 60,
                        static_cast<int64_t>(std::strlen("completed-with-timeblock")));
    const int64_t relay_start = static_cast<int64_t>(
        StrTimeUtils::getTsFromDateTimeStr2(date + "/2020-01-02-14-02-00-0"));
    writeDailyTimeBlock(root, "camera-completed", "main.relay.fixture", date,
                        relay_referenced, relay_start, relay_start + 60,
                        static_cast<int64_t>(std::strlen("completed-relay-with-timeblock")));
    {
        MediaTuple tuple;
        tuple.app = "camera-completed";
        tuple.stream = "main";
        std::vector<TimeBlock> seeded_blocks;
        TimeQuery query(tuple,
                        root + "/hot/record/camera-completed/" + date + ".s3db");
        query.getRecordedTimePeriod(
            static_cast<uint64_t>(referenced_start - 60),
            static_cast<uint64_t>(referenced_start + 60),
            [&](std::vector<TimeBlock> &data) { seeded_blocks = data; });
        require(seeded_blocks.size() == 1 &&
                    decodeBase64(seeded_blocks.front().file_path()) == referenced,
                "Daily TimeFile fixture cannot be queried by TimeQuery");
    }

    runTemporaryMp4CleanupPass(manager);

    require(!File::fileExist(orphan),
            "Completed MP4 without a matching TimeBlock was not deleted immediately");
    require(File::fileExist(referenced),
            "Completed MP4 referenced by the daily TimeFile was deleted");
    require(File::fileExist(relay_referenced),
            "Completed relay MP4 referenced by the daily TimeFile was deleted");
    require(!File::fileExist(missing_timefile),
            "Completed orphan MP4 was retained when the camera had no TimeFile");
    require(!File::fileExist(missing_daily_timefile),
            "Completed MP4 without a matching TimeBlock was retained because its daily TimeFile was missing");
    require(SegmentTierRangeImp().queryByCamera("camera-completed").empty(),
            "Completed orphan cleanup created tier ranges");

    manager.stop();
    return 0;
}

int scenarioTemporaryMp4CleanupDryRun(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(true);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string path = root +
        "/hot/record/camera-dry-run/main/2020-01-02/.2020-01-02-15-00-00-0.mp4";
    const std::string completed_path = root +
        "/hot/record/camera-dry-run/main/2020-01-02/2020-01-02-15-01-00-0.mp4";
    const std::string different_path = root +
        "/hot/record/camera-dry-run/main/2020-01-02/2020-01-02-15-01-00-1.mp4";
    writeFile(path, "dry-run-temporary");
    writeFile(completed_path, "dry-run-completed-orphan");
    const int64_t completed_start = static_cast<int64_t>(
        StrTimeUtils::getTsFromDateTimeStr2("2020-01-02/2020-01-02-15-01-00-0"));
    writeDailyTimeBlock(root, "camera-dry-run", "main", "2020-01-02",
                        different_path, completed_start, completed_start + 60, 1);
    runTemporaryMp4CleanupPass(manager);
    runTemporaryMp4CleanupPass(manager);
    require(File::fileExist(path), "Dry-run deleted a temporary MP4");
    require(File::fileExist(completed_path), "Dry-run deleted a completed orphan MP4");

    manager.stop();
    return 0;
}

int scenarioTemporaryMp4CleanupRetention(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    auto hot_pools = manager.listPools(tierTypeToString(HotTier));
    require(!hot_pools.empty(), "Default HOT pool was not created");

    const std::string camera_limit = "camera-cleanup-camera-limit";
    StoragePolicy long_policy = makeHotOnlyPolicy(
        hot_pools.front().id, "cleanup-long-hot-retention", 3650);
    require(manager.createPolicy(long_policy) == long_policy.id,
            "Cannot create long HOT-retention cleanup policy");
    require(manager.assignPolicyToCamera(camera_limit, long_policy.id, "cleanup retention test"),
            "Cannot assign long HOT-retention cleanup policy");
    auto camera_recorder = StatisticRecorder::Instance().getRecorder(camera_limit, true);
    require(camera_recorder != nullptr, "Cannot create camera statistics for cleanup test");
    CameraOption camera_option;
    camera_option.keepArchivedMaxForAuto = false;
    camera_option.keepArchivedMaxFor = 1;
    camera_recorder->setCameraOption(camera_option);

    const std::string policy_limit = "camera-cleanup-policy-limit";
    StoragePolicy short_policy = makeHotOnlyPolicy(
        hot_pools.front().id, "cleanup-short-hot-retention", 30);
    require(manager.createPolicy(short_policy) == short_policy.id,
            "Cannot create short HOT-retention cleanup policy");
    require(manager.assignPolicyToCamera(policy_limit, short_policy.id, "cleanup retention test"),
            "Cannot assign short HOT-retention cleanup policy");
    auto policy_recorder = StatisticRecorder::Instance().getRecorder(policy_limit, true);
    require(policy_recorder != nullptr, "Cannot create policy-limit camera statistics");
    CameraOption long_camera_option;
    long_camera_option.keepArchivedMaxForAuto = false;
    long_camera_option.keepArchivedMaxFor = 3650ULL * 86400ULL;
    policy_recorder->setCameraOption(long_camera_option);

    const std::string camera_limited_path = root + "/hot/record/" + camera_limit +
        "/main/2020-01-02/.2020-01-02-17-00-00-0.mp4";
    const std::string cutoff_date = getTimeStr("%Y-%m-%d");
    const std::string cutoff_segment = getTimeStr("%Y-%m-%d-%H-%M-%S");
    const std::string boundary_path = root + "/hot/record/" + camera_limit +
        "/main/" + cutoff_date + "/." + cutoff_segment + "-0.mp4";
    const std::string policy_limited_path = root + "/hot/record/" + policy_limit +
        "/main/2020-01-02/.2020-01-02-17-01-00-0.mp4";
    writeFile(camera_limited_path, "camera-retention-wins");
    writeFile(boundary_path, "cutoff-date-is-not-scanned");
    writeFile(policy_limited_path, "policy-retention-wins");

    runTemporaryMp4CleanupPass(manager);
    require(File::fileExist(camera_limited_path) && File::fileExist(policy_limited_path),
            "Retention cleanup deleted a temporary MP4 on first observation");
    runTemporaryMp4CleanupPass(manager);
    require(!File::fileExist(camera_limited_path),
            "Camera keepArchivedMaxFor was not used as the smaller retention");
    require(!File::fileExist(policy_limited_path),
            "HOT retain_until_days was not used as the smaller retention");
    require(File::fileExist(boundary_path),
            "Temporary MP4 in the cutoff date folder was scanned and deleted");

    manager.stop();
    return 0;
}

int scenarioTemporaryMp4CleanupRestartPrepare(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string path = root +
        "/hot/record/camera-restart/main/2020-01-02/.2020-01-02-16-00-00-0.mp4";
    writeFile(path, "restart-temporary");
    setFileModifiedTime(path, ::time(nullptr) - 7200);
    runTemporaryMp4CleanupPass(manager);
    require(File::fileExist(path), "First process deleted temporary MP4 after one observation");

    manager.stop();
    return 0;
}

int scenarioTemporaryMp4CleanupRestartVerify(const std::string &root) {
    migrateDatabase(root);
    configureTemporaryMp4Cleanup(false);
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const std::string path = root +
        "/hot/record/camera-restart/main/2020-01-02/.2020-01-02-16-00-00-0.mp4";
    require(File::fileExist(path), "Restart fixture is missing");
    runTemporaryMp4CleanupPass(manager);
    require(File::fileExist(path), "Fresh process reused an observation from the previous process");
    runTemporaryMp4CleanupPass(manager);
    require(!File::fileExist(path), "Fresh process did not delete after its second observation");

    manager.stop();
    return 0;
}

std::string motionPathForDay(const std::string &root,
                             const std::string &camera,
                             int64_t day_start,
                             const std::string &extension) {
    return root + "/hot/record/" + camera + "/archive/motion/" +
        getTimeStr("%Y-%m-%d", static_cast<time_t>(day_start)) + extension;
}

void addMetadataCleanupRange(const std::string &range_id,
                             const std::string &camera,
                             const std::string &stream,
                             int64_t start_time,
                             int64_t end_time,
                             const std::string &job_camera = "") {
    const int64_t now = static_cast<int64_t>(::time(nullptr));
    SegmentTierRange range;
    range.range_id = range_id;
    range.camera_id = camera;
    range.stream_id = stream;
    range.tier = tierTypeToString(HotTier);
    range.pool_id = "hot-pool";
    range.status = segmentStatusToString(SegmentStatus::DELETED);
    range.start_time = start_time;
    range.end_time = end_time;
    range.segment_count = 1;
    range.created_at = now;
    range.updated_at = now;
    require(SegmentTierRangeImp().add(range),
            "Cannot seed deleted tier range");

    TierMetadataCleanupJob job;
    job.range_id = range_id;
    job.camera_id = job_camera.empty() ? camera : job_camera;
    job.status = "PENDING";
    job.created_at = now;
    job.updated_at = now;
    require(TierMetadataCleanupJobRepository().add(job),
            "Cannot seed metadata cleanup job");
}

bool metadataCleanupPending(const std::string &range_id,
                            std::string *error = nullptr) {
    auto jobs = TierMetadataCleanupJobRepository().queryPending(1000);
    for (const auto &job : jobs) {
        if (job.range_id != range_id)
            continue;
        if (error)
            *error = job.error_message.value_or("");
        return true;
    }
    return false;
}

void addBookmark(const std::string &guid,
                 const std::string &camera,
                 int64_t start_time,
                 int64_t end_time) {
    Bookmark bookmark;
    bookmark.guid = guid;
    bookmark.camera_guid = camera;
    bookmark.start_time = start_time;
    bookmark.end_time = end_time;
    bookmark.duration = static_cast<int>(end_time - start_time);
    bookmark.created = static_cast<int64_t>(::time(nullptr));
    BookmarkImp().add(bookmark, "");
}

bool bookmarkExists(const std::string &guid) {
    return !BookmarkImp().findById(guid).empty();
}

bool bookmarkIndexExists(const std::string &guid) {
    return !BookmarkIndexImp().findByBookmarkGuid(guid).empty();
}

int scenarioMotionCleanupSafety(const std::string &root) {
    migrateDatabase(root);
    migrateEscDatabase(root);
    MiscDataImp().initMiscData();
    mINI::Instance()[Record::kArchiveName] = "archive";
    TierStorageManager &manager = TierStorageManager::Instance();
    manager.start();

    const int64_t day = static_cast<int64_t>(
        StampUtils::getStartOfDay(static_cast<uint64_t>(::time(nullptr) - 10 * 86400)));
    const int64_t next_day = static_cast<int64_t>(
        StampUtils::getStartOfDay(static_cast<uint64_t>(day + 36 * 3600)));

    const std::string retained_camera = "camera-motion-retained";
    const std::string retained_range = "range-motion-retained";
    const std::string retained_block = motionPathForDay(root, retained_camera, day, ".mblk");
    const std::string retained_index = motionPathForDay(root, retained_camera, day, ".idx");
    writeFile(retained_block, "motion");
    writeFile(retained_index, "index");
    writeTimeBlock(root, retained_camera, "secondary", "retained.mp4",
                   day + 3600, day + 3660, 1);
    addMetadataCleanupRange(retained_range, retained_camera, "main",
                            day + 3500, day + 3700);

    const std::string empty_camera = "camera-motion-empty";
    const std::string empty_range = "range-motion-empty";
    const std::string empty_block = motionPathForDay(root, empty_camera, day, ".mblk");
    const std::string empty_index = motionPathForDay(root, empty_camera, day, ".idx");
    writeFile(empty_block, "motion");
    writeFile(empty_index, "index");
    writeTimeBlock(root, empty_camera, "main", "next-day.mp4",
                   next_day + 3600, next_day + 3660, 1);
    addMetadataCleanupRange(empty_range, empty_camera, "main",
                            day + 3500, day + 3700);
    const std::string deleted_bookmark = "bookmark-range-deleted";
    const std::string retained_bookmark = "bookmark-range-retained";
    const std::string other_camera_bookmark = "bookmark-other-camera";
    addBookmark(deleted_bookmark, empty_camera, day + 3600, day + 3660);
    addBookmark(retained_bookmark, empty_camera, day + 3400, day + 3450);
    addBookmark(other_camera_bookmark, "camera-bookmark-other",
                day + 3600, day + 3660);

    const std::string no_timefile_camera = "camera-motion-no-timefile";
    const std::string no_timefile_range = "range-motion-no-timefile";
    const std::string no_timefile_block =
        motionPathForDay(root, no_timefile_camera, day, ".mblk");
    const std::string no_timefile_index =
        motionPathForDay(root, no_timefile_camera, day, ".idx");
    writeFile(no_timefile_block, "motion");
    writeFile(no_timefile_index, "index");
    addMetadataCleanupRange(no_timefile_range, no_timefile_camera, "main",
                            day + 3500, day + 3700);

    const std::string sequential_camera = "camera-motion-sequential";
    const std::string sequential_range = "range-motion-sequential";
    const std::string sequential_block =
        motionPathForDay(root, sequential_camera, day, ".mblk");
    const std::string sequential_index =
        motionPathForDay(root, sequential_camera, day, ".idx");
    const std::string date = getTimeStr("%Y-%m-%d", static_cast<time_t>(day));
    const std::string sequential_segment = root + "/hot/record/" +
        sequential_camera + "/main/" + date + "/" + date + "-02-00-00-0.mp4";
    writeFile(sequential_block, "motion");
    writeFile(sequential_index, "index");
    writeTimeBlock(root, sequential_camera, "main", sequential_segment,
                   day + 7200, day + 7260, 1);
    addMetadataCleanupRange(sequential_range, sequential_camera, "main",
                            day + 7100, day + 7300);
    const std::string sequential_bookmark = "bookmark-range-sequential";
    addBookmark(sequential_bookmark, sequential_camera,
                day + 7150, day + 7200);

    TierMetadataCleanupJob missing_range_job;
    missing_range_job.range_id = "range-motion-missing";
    missing_range_job.camera_id = "camera-motion-missing";
    missing_range_job.status = "PENDING";
    missing_range_job.created_at = static_cast<int64_t>(::time(nullptr));
    missing_range_job.updated_at = missing_range_job.created_at;
    require(TierMetadataCleanupJobRepository().add(missing_range_job),
            "Cannot seed metadata job without a range");

    const std::string mismatch_camera = "camera-motion-mismatch";
    const std::string mismatch_range = "range-motion-mismatch";
    const std::string mismatch_block =
        motionPathForDay(root, mismatch_camera, day, ".mblk");
    const std::string mismatch_index =
        motionPathForDay(root, mismatch_camera, day, ".idx");
    writeFile(mismatch_block, "motion");
    writeFile(mismatch_index, "index");
    addMetadataCleanupRange(mismatch_range, mismatch_camera, "main",
                            day + 3500, day + 3700,
                            "camera-motion-wrong-owner");

    manager.runTieringCycle();
    drainWorkThreadPool();

    require(File::fileExist(retained_block) && File::fileExist(retained_index),
            "Motion was deleted while another stream still had a TimeBlock");
    require(!File::fileExist(empty_block) && !File::fileExist(empty_index),
            "Motion was not deleted for an empty camera day");
    require(!File::fileExist(no_timefile_block) && !File::fileExist(no_timefile_index),
            "Motion was not deleted for a camera without any TimeFile");
    require(!metadataCleanupPending(retained_range),
            "Retained-day metadata job did not complete");
    require(!metadataCleanupPending(empty_range),
            "Empty-day metadata job did not complete");
    require(!metadataCleanupPending(no_timefile_range),
            "No-TimeFile metadata job did not complete");
    require(!bookmarkExists(deleted_bookmark) &&
                !bookmarkIndexExists(deleted_bookmark),
            "Bookmark or its cluster index inside the deleted range was retained");
    require(bookmarkExists(retained_bookmark) &&
                bookmarkIndexExists(retained_bookmark),
            "Bookmark or its cluster index outside the deleted range was removed");
    require(bookmarkExists(other_camera_bookmark) &&
                bookmarkIndexExists(other_camera_bookmark),
            "Bookmark or its cluster index belonging to another camera was removed");
    std::string sequential_error;
    require(metadataCleanupPending(sequential_range, &sequential_error) &&
                sequential_error.find("Awaiting timefile swap verification") == 0,
            "Metadata job did not wait for TimeFile swap verification");
    require(File::fileExist(sequential_block) && File::fileExist(sequential_index),
            "Motion was deleted in the same pass that scheduled TimeFile cleanup");
    require(bookmarkExists(sequential_bookmark) &&
                bookmarkIndexExists(sequential_bookmark),
            "Bookmark or its cluster index was deleted before TimeFile cleanup completed");
    std::string missing_range_error;
    require(metadataCleanupPending(missing_range_job.range_id, &missing_range_error) &&
                missing_range_error.find("Metadata missing") == 0,
            "Missing range metadata job was incorrectly completed");
    std::string mismatch_error;
    require(metadataCleanupPending(mismatch_range, &mismatch_error) &&
                mismatch_error.find("Camera ID mismatch") == 0 &&
                File::fileExist(mismatch_block) && File::fileExist(mismatch_index),
            "Camera mismatch did not fail closed");

    manager.runTieringCycle();
    drainWorkThreadPool();
    require(!metadataCleanupPending(sequential_range),
            "Metadata job did not complete after TimeFile cleanup");
    require(!File::fileExist(sequential_block) && !File::fileExist(sequential_index),
            "Motion was not deleted after TimeFile cleanup completed");
    require(!bookmarkExists(sequential_bookmark) &&
                !bookmarkIndexExists(sequential_bookmark),
            "Bookmark or its cluster index was not deleted after TimeFile cleanup completed");

    manager.stop();
    return 0;
}

int runScenario(const std::string &name, const std::string &root) {
    try {
        if (name == "register-idempotent") return scenarioRegisterIdempotent(root);
        if (name == "hot-to-nas") return scenarioHotToNas(root);
        if (name == "overlap-start-ownership") return scenarioOverlappingSegmentsUseStartOwnership(root);
        if (name == "reuse-existing-nas-target") return scenarioReusesExistingNasTarget(root);
        if (name == "backoff-missing-segment-job") return scenarioBacksOffMissingSegmentJob(root);
        if (name == "drop-missing-segment") return scenarioDropsMissingSegmentAfterMaxAttempts(root);
        if (name == "hot-to-minio") return scenarioHotToMinio(root);
        if (name == "restore-from-minio") return scenarioRestoreFromMinio(root);
        if (name == "duplicate-restore") return scenarioDuplicateRestore(root);
        if (name == "restore-window-all-streams") return scenarioRestoreWindowIncludesAllStreams(root);
        if (name == "restore-status-preserves-total") return scenarioRestoreStatusPreservesDeclaredTotals(root);
        if (name == "reuse-equivalent-restore-windows") return scenarioReusesEquivalentRestoreWindows(root);
        if (name == "retention-approval") return scenarioRetentionApproval(root);
        if (name == "protected-retention") return scenarioProtectedRetention(root);
        if (name == "delete-waits-active-transfer") return scenarioDeletionWaitsForActiveTransfer(root);
        if (name == "restore-concurrency-limit") return scenarioRestoreConcurrencyLimit(root);
        if (name == "search-tier-metadata") return scenarioSearchEngineReflectsTierMetadata(root);
        if (name == "load-saved-camera-root") return scenarioLoadsSavedCameraRecordRoot(root);
        if (name == "policy-lifecycle-storage") return scenarioPolicyLifecycleChangesCameraStorage(root);
        if (name == "resolve-local-playback-paths") return scenarioResolveLocalPlaybackPaths(root);
        if (name == "reject-unreadable-playback-path") return scenarioRejectUnreadablePlaybackPath(root);
        if (name == "missing-restore-cache") return scenarioMissingRestoreCacheRequiresRestore(root);
        if (name == "validate-restore-window-cache") return scenarioValidatesRestoreWindowCache(root);
        if (name == "job-scoped-restore-cache-cleanup") return scenarioCleansJobScopedRestoreCache(root);
        if (name == "pool-health-playback") return scenarioTracksPoolHealthAndBlocksPlayback(root);
        if (name == "pool-health-tier-move") return scenarioPoolHealthDefersTierMove(root);
        if (name == "policy-reassignment-cancels-job") return scenarioPolicyReassignmentCancelsPendingJob(root);
        if (name == "policy-retention-cancels-job") return scenarioPolicyRetentionChangeCancelsPendingJob(root);
        if (name == "pool-health-tier-delete") return scenarioPoolHealthGuardsDeletion(root);
        if (name == "backfill-atomic-idempotent") return scenarioBackfillAtomicIdempotent(root);
        if (name == "backfill-local-day") return scenarioBackfillUsesLocalDayBuckets(root);
        if (name == "compact-range-repository") return scenarioCompactRangeRepository(root);
        if (name == "backfill-orphan-retry") return scenarioBackfillOrphanRetriesWithoutAdvancing(root);
        if (name == "backfill-missing-timeline") return scenarioBackfillMissingTimelineCompletes(root);
        if (name == "recovery-prepare") return scenarioRecoveryPrepare(root);
        if (name == "recovery-verify") return scenarioRecoveryVerify(root);
        if (name == "tmp-cleanup-stable-stale") return scenarioTemporaryMp4CleanupStableStale(root);
        if (name == "tmp-cleanup-preserves-unsafe") return scenarioTemporaryMp4CleanupPreservesUnsafe(root);
        if (name == "completed-cleanup") return scenarioCompletedMp4Cleanup(root);
        if (name == "tmp-cleanup-dry-run") return scenarioTemporaryMp4CleanupDryRun(root);
        if (name == "tmp-cleanup-retention") return scenarioTemporaryMp4CleanupRetention(root);
        if (name == "tmp-cleanup-restart-prepare") return scenarioTemporaryMp4CleanupRestartPrepare(root);
        if (name == "tmp-cleanup-restart-verify") return scenarioTemporaryMp4CleanupRestartVerify(root);
        if (name == "motion-cleanup-safety") return scenarioMotionCleanupSafety(root);
        throw std::runtime_error("Unknown scenario: " + name);
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "TierStorageManager scenario '%s' failed: %s\n", name.c_str(), ex.what());
        return 1;
    }
}

int runChild(const std::string &scenario, const std::string &root) {
    pid_t pid = ::fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        ::execl(g_executable.c_str(), g_executable.c_str(),
                "--tier-scenario", scenario.c_str(), "--tier-root", root.c_str(),
                static_cast<char *>(nullptr));
        std::_Exit(127);
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "waitpid failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

class TempRoot {
public:
    TempRoot() : path(makeTempRoot()) {}
    ~TempRoot() { removeTree(path); }
    std::string path;
};

} // namespace

TEST(StreamVariantTest, NormalizesRelayAndTranscodeSuffixPayloads) {
    EXPECT_EQ("main", StrMediaUtils::originStream("main.transcode.fixture"));
    EXPECT_EQ("main", StrMediaUtils::originStream("main.relay.fixture"));
    EXPECT_EQ("main", StrMediaUtils::originStream("main"));
    EXPECT_FALSE(StrMediaUtils::isTranscodeStream("main.transcoder.fixture"));
    EXPECT_FALSE(StrMediaUtils::isRelayStream("main.relayed.fixture"));
}

TEST(TierStorageManagerIntegrationTest, MapsAppDirectoryBindMountToLogicalPoolRoot) {
    DiskPartition direct;
    direct.device = "/dev/sdb1";
    direct.mount_point = "/mnt/storage";
    direct.filesystem_type = "ext4";
    direct.total_bytes = 1000;
    direct.free_bytes = 600;
    direct.used_bytes = 400;
    direct.usage_pct = 40.0f;

    const StorageMountCandidate direct_candidate =
        StorageMountCandidate::fromDiskPartition(direct, "LOCAL_DISK", "record");
    EXPECT_EQ("/mnt/storage", direct_candidate.mount_path);
    EXPECT_EQ("/mnt/storage", direct_candidate.actual_mount_path);
    EXPECT_EQ("/mnt/storage/record", direct_candidate.data_path);
    EXPECT_EQ("DIRECT_ROOT", direct_candidate.layout);
    EXPECT_EQ(600U, direct_candidate.toJson()["free"].asUInt64());

    DiskPartition app_mount = direct;
    app_mount.mount_point = "/opt/media/bin/www/record/";
    const StorageMountCandidate bind_candidate =
        StorageMountCandidate::fromDiskPartition(app_mount, "LOCAL_DISK", "record");
    EXPECT_EQ("/opt/media/bin/www", bind_candidate.mount_path);
    EXPECT_EQ("/opt/media/bin/www/record", bind_candidate.actual_mount_path);
    EXPECT_EQ("/opt/media/bin/www/record", bind_candidate.data_path);
    EXPECT_EQ("APP_DIRECTORY_MOUNT", bind_candidate.layout);
    EXPECT_EQ("/opt/media/bin/www", bind_candidate.toJson()["mount"].asString());

    StoragePool local_pool;
    local_pool.mount_path = Optional<std::string>(bind_candidate.mount_path);
    EXPECT_EQ("/opt/media/bin/www", TierStorageManager::resolveFilePoolMountRoot(local_pool));
    EXPECT_EQ("/opt/media/bin/www/record",
              TierStorageManager::resolveFilePoolDataRoot(local_pool, "record"));

    StoragePool nas_pool;
    nas_pool.network_path = Optional<std::string>("/mnt/nas/");
    EXPECT_EQ("/mnt/nas", TierStorageManager::resolveFilePoolMountRoot(nas_pool));
    EXPECT_EQ("/mnt/nas/capture",
              TierStorageManager::resolveFilePoolDataRoot(nas_pool, "capture"));
}

TEST(TierStorageManagerIntegrationTest, RegistersHotSegmentIdempotently) {
    TempRoot root;
    EXPECT_EQ(0, runChild("register-idempotent", root.path));
}

TEST(TierStorageManagerIntegrationTest, CleansRangeMetadataBeforeCompletingJob) {
    TempRoot root;
    EXPECT_EQ(0, runChild("motion-cleanup-safety", root.path));
}

TEST(TierStorageManagerIntegrationTest, RunsCycleAndMovesHotSegmentToNas) {
    TempRoot root;
    EXPECT_EQ(0, runChild("hot-to-nas", root.path));
}

TEST(TierStorageManagerIntegrationTest, MovesOnlyStartOwnedSegmentsAcrossOverlappingBoundary) {
    TempRoot root;
    EXPECT_EQ(0, runChild("overlap-start-ownership", root.path));
}

TEST(TierStorageManagerIntegrationTest, ReusesExistingNasTargetWhenSourceIsMissing) {
    TempRoot root;
    EXPECT_EQ(0, runChild("reuse-existing-nas-target", root.path));
}

TEST(TierStorageManagerIntegrationTest, BacksOffMissingSegmentJob) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backoff-missing-segment-job", root.path));
}

TEST(TierStorageManagerIntegrationTest, DropsMissingSegmentAfterMaxAttempts) {
    TempRoot root;
    EXPECT_EQ(0, runChild("drop-missing-segment", root.path));
}

TEST(TierStorageManagerIntegrationTest, RunsCycleAndMovesHotSegmentToMinio) {
    if (!hasMinioEnvironment())
        GTEST_SKIP() << "TIER_TEST_S3_* environment is required";
    TempRoot root;
    EXPECT_EQ(0, runChild("hot-to-minio", root.path));
}

TEST(TierStorageManagerIntegrationTest, RestoresSegmentFromMinio) {
    if (!hasMinioEnvironment())
        GTEST_SKIP() << "TIER_TEST_S3_* environment is required";
    TempRoot root;
    EXPECT_EQ(0, runChild("restore-from-minio", root.path));
}

TEST(TierStorageManagerIntegrationTest, ReusesActiveRestoreForDuplicateRequest) {
    TempRoot root;
    EXPECT_EQ(0, runChild("duplicate-restore", root.path));
}

TEST(TierStorageManagerIntegrationTest, RestoresAllCameraStreamsWhenStreamIdIsEmpty) {
    TempRoot root;
    EXPECT_EQ(0, runChild("restore-window-all-streams", root.path));
}

TEST(TierStorageManagerIntegrationTest, RestoreStatusPreservesTimeBlockTotalBytes) {
    TempRoot root;
    EXPECT_EQ(0, runChild("restore-status-preserves-total", root.path));
}

TEST(TierStorageManagerIntegrationTest, ReusesEquivalentRestoreWindowJobsAndExtendsTtl) {
    TempRoot root;
    EXPECT_EQ(0, runChild("reuse-equivalent-restore-windows", root.path));
}

TEST(TierStorageManagerIntegrationTest, MarksExpiredAndDeletesAfterApproval) {
    TempRoot root;
    EXPECT_EQ(0, runChild("retention-approval", root.path));
}

TEST(TierStorageManagerIntegrationTest, ProtectsRetentionUntilGuardsExpire) {
    TempRoot root;
    EXPECT_EQ(0, runChild("protected-retention", root.path));
}

TEST(TierStorageManagerIntegrationTest, DefersDeletionWhileTransferIsActive) {
    TempRoot root;
    EXPECT_EQ(0, runChild("delete-waits-active-transfer", root.path));
}

TEST(TierStorageManagerIntegrationTest, EnforcesRestoreConcurrencyLimit) {
    TempRoot root;
    EXPECT_EQ(0, runChild("restore-concurrency-limit", root.path));
}

TEST(TierStorageManagerIntegrationTest, SearchEngineReflectsTierMetadata) {
    TempRoot root;
    EXPECT_EQ(0, runChild("search-tier-metadata", root.path));
}

TEST(TierStorageManagerIntegrationTest, LoadsAndRefreshesSavedCameraRecordRoot) {
    TempRoot root;
    EXPECT_EQ(0, runChild("load-saved-camera-root", root.path));
}

TEST(TierStorageManagerIntegrationTest, PolicyLifecycleChangesCameraStorage) {
    TempRoot root;
    EXPECT_EQ(0, runChild("policy-lifecycle-storage", root.path));
}

TEST(TierStorageManagerIntegrationTest, ResolvesExistingEmptyAndSymlinkPlaybackFiles) {
    TempRoot root;
    EXPECT_EQ(0, runChild("resolve-local-playback-paths", root.path));
}

TEST(TierStorageManagerIntegrationTest, RejectsUnreadablePlaybackFile) {
    TempRoot root;
    const int status = runChild("reject-unreadable-playback-path", root.path);
    if (status == 77)
        GTEST_SKIP() << "Current user namespace cannot drop root privileges";
    EXPECT_EQ(0, status);
}

TEST(TierStorageManagerIntegrationTest, RequiresRestoreWhenObjectCacheIsMissing) {
    TempRoot root;
    EXPECT_EQ(0, runChild("missing-restore-cache", root.path));
}

TEST(TierStorageManagerIntegrationTest, ValidatesRestoreWindowCacheByStreamCoverageAndTtl) {
    TempRoot root;
    EXPECT_EQ(0, runChild("validate-restore-window-cache", root.path));
}

TEST(TierStorageManagerIntegrationTest, CleansOnlyExpiredJobScopedRestoreCache) {
    TempRoot root;
    EXPECT_EQ(0, runChild("job-scoped-restore-cache-cleanup", root.path));
}

TEST(TierStorageManagerIntegrationTest, TracksPoolHealthAndBlocksPlaybackWhenOffline) {
    TempRoot root;
    EXPECT_EQ(0, runChild("pool-health-playback", root.path));
}

TEST(TierStorageManagerIntegrationTest, DefersTierMoveUntilDestinationPoolRecovers) {
    TempRoot root;
    EXPECT_EQ(0, runChild("pool-health-tier-move", root.path));
}

TEST(TierStorageManagerIntegrationTest, CancelsPendingJobAfterPolicyReassignment) {
    TempRoot root;
    EXPECT_EQ(0, runChild("policy-reassignment-cancels-job", root.path));
}

TEST(TierStorageManagerIntegrationTest, CancelsPendingJobAfterRetentionIncrease) {
    TempRoot root;
    EXPECT_EQ(0, runChild("policy-retention-cancels-job", root.path));
}

TEST(TierStorageManagerIntegrationTest, GuardsRangeAndOrphanDeletionUntilPoolRecovers) {
    TempRoot root;
    EXPECT_EQ(0, runChild("pool-health-tier-delete", root.path));
}

TEST(TierStorageManagerIntegrationTest, BackfillsAtomicallyAndIdempotently) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-atomic-idempotent", root.path));
}

TEST(TierStorageManagerIntegrationTest, BackfillsAcrossLocalMidnightIntoSeparateRanges) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-local-day", root.path));
}

TEST(TierStorageManagerIntegrationTest, BoundsAndCompactsTierMetadata) {
    TempRoot root;
    EXPECT_EQ(0, runChild("compact-range-repository", root.path));
}

TEST(TierStorageManagerIntegrationTest, RetriesOrphanChunkWithoutAdvancingCursor) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-orphan-retry", root.path));
}

TEST(TierStorageManagerIntegrationTest, CompletesEmptyBackfillWhenCameraHasNoTimeFile) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-missing-timeline", root.path));
}

TEST(TierStorageManagerIntegrationTest, RecoversRunningWorkAcrossFreshProcesses) {
    TempRoot root;
    ASSERT_EQ(0, runChild("recovery-prepare", root.path));
    EXPECT_EQ(0, runChild("recovery-verify", root.path));
}

TEST(TierStorageManagerIntegrationTest, DeletesStableStaleTemporaryMp4FromHotPools) {
    TempRoot root;
    EXPECT_EQ(0, runChild("tmp-cleanup-stable-stale", root.path));
}

TEST(TierStorageManagerIntegrationTest, PreservesUnsafeTemporaryMp4Candidates) {
    TempRoot root;
    EXPECT_EQ(0, runChild("tmp-cleanup-preserves-unsafe", root.path));
}

TEST(TierStorageManagerIntegrationTest, DeletesCompletedMp4OnlyWhenMatchingTimeBlockIsMissing) {
    TempRoot root;
    EXPECT_EQ(0, runChild("completed-cleanup", root.path));
}

TEST(TierStorageManagerIntegrationTest, ReportsTemporaryMp4InDryRunWithoutDeleting) {
    TempRoot root;
    EXPECT_EQ(0, runChild("tmp-cleanup-dry-run", root.path));
}

TEST(TierStorageManagerIntegrationTest, UsesSmallerHotOrCameraRetentionForCleanup) {
    TempRoot root;
    EXPECT_EQ(0, runChild("tmp-cleanup-retention", root.path));
}

TEST(TierStorageManagerIntegrationTest, DoesNotDeleteTemporaryMp4ImmediatelyAfterRestart) {
    TempRoot root;
    ASSERT_EQ(0, runChild("tmp-cleanup-restart-prepare", root.path));
    EXPECT_EQ(0, runChild("tmp-cleanup-restart-verify", root.path));
}

int main(int argc, char **argv) {
    g_executable = argv[0];
    std::string scenario;
    std::string root;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--tier-scenario") scenario = argv[++i];
        else if (std::string(argv[i]) == "--tier-root") root = argv[++i];
    }
    if (!scenario.empty()) {
        if (root.empty()) return 2;
        return runScenario(scenario, root);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
