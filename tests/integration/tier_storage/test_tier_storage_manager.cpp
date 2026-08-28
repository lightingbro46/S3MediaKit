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
#include <unistd.h>
#include <ftw.h>

#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Camera/CameraStatistic.h"
#include "Local/SearchEngine.h"
#include "Local/StatisticRecorder.h"
#include "Local/TierStorageManager.h"
#include "Local/TimeMuxer.h"
#include "Storage/DbStorage.h"
#include "Storage/MigrationHistory.h"
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
    mINI::Instance()[General::kMediaServerId] = "tier-manager-test";
    mINI::Instance()[Protocol::kMP4SavePath] = root + "/hot";
    mINI::Instance()[Record::kAppName] = "record";
    mINI::Instance()[Storage::kRestoreSavePath] = root + "/restore";
    mINI::Instance()[Storage::kTierRangeBackfillEnabled] = 0;
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
                                const std::string &policy_id) {
    PolicyTierConfig hot;
    hot.tier = tierTypeToString(HotTier);
    hot.enabled = true;
    hot.pool_id = hot_pool_id;
    hot.retain_until_days = 3650;
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

    SegmentTierImp segment_imp;
    SegmentTierRangeImp range_imp;
    require(segment_imp.findByCamera(camera).size() == 1, "Duplicate segment row was created");
    require(range_imp.queryByCamera(camera, now - 180, now).size() == 1,
            "Duplicate range row was created");
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
    require(File::loadFile(destination) == payload, "NAS destination content mismatch");
    auto ranges = SegmentTierRangeImp().queryByCamera(camera, now - 240, now);
    require(ranges.size() == 1 && ranges.front().tier == tierTypeToString(WarmTier) &&
            ranges.front().pool_id == nas.id, "Tier range was not updated to NAS/WARM");
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

    auto request = manager.requestRestoreSegment(
        fixture.camera, fixture.stream, fixture.start, fixture.source_path, "integration test");
    require(request.handled && !request.job_id.empty(), "Restore request was not accepted");
    require(waitUntil([&]() {
        auto jobs = manager.getRestoreJob(request.job_id);
        return !jobs.empty() && (jobs.front().status == "DONE" || jobs.front().status == "FAILED");
    }, 15000), "Restore job did not reach a terminal state");
    auto jobs = manager.getRestoreJob(request.job_id);
    require(jobs.size() == 1 && jobs.front().status == "DONE",
            jobs.empty() ? "Restore job disappeared"
                         : "Restore failed: " + jobs.front().error_message.value_or(""));
    const std::string restored = manager.getRestoreSegmentPath(
        fixture.camera, fixture.stream, fixture.segment_key);
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
    require(waitUntil([&]() {
        return PoolMetricsImp().latestForPool(hot_pools.front().id).timestamp > 0;
    }, 5000), "Protected retention cycle did not complete health snapshot");
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
            tuple, fixture.start, fixture.end, 0, 0, false, "", true,
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
    require(!File::fileExist(manager.getRestoreSegmentPath(camera, stream, segment_path)),
            "Restore cache unexpectedly exists before resolution");

    const PlaybackPathResolveResult result =
        manager.resolvePlaybackSegmentPath(camera, stream, now, original);
    require(!result.ready && result.restore_required && !result.fallback &&
                result.read_path.empty() && result.tier == tierTypeToString(ColdTier) &&
                result.pool_id == cold_pool.id && result.range_id == range.range_id,
            "Missing object restore cache did not request restore");

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
    const int64_t start = static_cast<int64_t>(::time(nullptr)) - 3600;
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

    auto records = SegmentTierRepository().findByCamera(camera);
    auto ranges = SegmentTierRangeImp().queryByCamera(camera, start - 1, end + 1);
    const int64_t expected_size = static_cast<int64_t>(first_payload.size() +
                                                        second_payload.size());
    require(records.size() == 2 && records.front().segment_path == first_key &&
                records.back().segment_path == second_key,
            "Backfill did not preserve and complete partial segment metadata");
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

    records = SegmentTierRepository().findByCamera(camera);
    ranges = SegmentTierRangeImp().queryByCamera(camera, start - 1, end + 1);
    require(records.size() == 2, "Backfill replay duplicated segment records");
    require(ranges.size() == 1 && ranges.front().segment_count == 2 &&
                ranges.front().size_bytes == expected_size,
            "Backfill replay inflated normalized range counters");

    manager.stop();
    drainWorkThreadPool();
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
    require(SegmentTierRepository().findByCamera(camera).empty(),
            "Orphan chunk partially committed segment metadata");

    manager.stop();
    drainWorkThreadPool();
    return 0;
}

int scenarioBackfillMissingTimelineRetries(const std::string &root) {
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
        return rows.size() == 1 && rows.front().status == "RETRY";
    }, 5000), "Missing timeline did not move backfill task to RETRY");
    auto rows = TierBackfillTaskImp().findByCameraId(camera);
    require(rows.front().cursor_time == start && rows.front().attempt_count == 1 &&
                !rows.front().last_error.empty(),
            "Missing timeline advanced cursor or lost constructor error");

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
    auto jobs = TieringJobImp().findByJobId("recovery-tier-job");
    require(jobs.size() == 1 && jobs.front().status == "PENDING",
            "RUNNING tiering job was not recovered to PENDING");
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

int runScenario(const std::string &name, const std::string &root) {
    try {
        if (name == "register-idempotent") return scenarioRegisterIdempotent(root);
        if (name == "hot-to-nas") return scenarioHotToNas(root);
        if (name == "hot-to-minio") return scenarioHotToMinio(root);
        if (name == "restore-from-minio") return scenarioRestoreFromMinio(root);
        if (name == "duplicate-restore") return scenarioDuplicateRestore(root);
        if (name == "retention-approval") return scenarioRetentionApproval(root);
        if (name == "protected-retention") return scenarioProtectedRetention(root);
        if (name == "restore-concurrency-limit") return scenarioRestoreConcurrencyLimit(root);
        if (name == "search-tier-metadata") return scenarioSearchEngineReflectsTierMetadata(root);
        if (name == "load-saved-camera-root") return scenarioLoadsSavedCameraRecordRoot(root);
        if (name == "policy-lifecycle-storage") return scenarioPolicyLifecycleChangesCameraStorage(root);
        if (name == "resolve-local-playback-paths") return scenarioResolveLocalPlaybackPaths(root);
        if (name == "reject-unreadable-playback-path") return scenarioRejectUnreadablePlaybackPath(root);
        if (name == "missing-restore-cache") return scenarioMissingRestoreCacheRequiresRestore(root);
        if (name == "backfill-atomic-idempotent") return scenarioBackfillAtomicIdempotent(root);
        if (name == "backfill-orphan-retry") return scenarioBackfillOrphanRetriesWithoutAdvancing(root);
        if (name == "backfill-missing-timeline") return scenarioBackfillMissingTimelineRetries(root);
        if (name == "recovery-prepare") return scenarioRecoveryPrepare(root);
        if (name == "recovery-verify") return scenarioRecoveryVerify(root);
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

TEST(TierStorageManagerIntegrationTest, RunsCycleAndMovesHotSegmentToNas) {
    TempRoot root;
    EXPECT_EQ(0, runChild("hot-to-nas", root.path));
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

TEST(TierStorageManagerIntegrationTest, MarksExpiredAndDeletesAfterApproval) {
    TempRoot root;
    EXPECT_EQ(0, runChild("retention-approval", root.path));
}

TEST(TierStorageManagerIntegrationTest, ProtectsRetentionUntilGuardsExpire) {
    TempRoot root;
    EXPECT_EQ(0, runChild("protected-retention", root.path));
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

TEST(TierStorageManagerIntegrationTest, BackfillsAtomicallyAndIdempotently) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-atomic-idempotent", root.path));
}

TEST(TierStorageManagerIntegrationTest, RetriesOrphanChunkWithoutAdvancingCursor) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-orphan-retry", root.path));
}

TEST(TierStorageManagerIntegrationTest, RetriesWhenCameraHasNoTimeFile) {
    TempRoot root;
    EXPECT_EQ(0, runChild("backfill-missing-timeline", root.path));
}

TEST(TierStorageManagerIntegrationTest, RecoversRunningWorkAcrossFreshProcesses) {
    TempRoot root;
    ASSERT_EQ(0, runChild("recovery-prepare", root.path));
    EXPECT_EQ(0, runChild("recovery-verify", root.path));
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
