#include <gtest/gtest.h>

#include "Local/TierStorageBase.h"
#include "Common/config.h"
#include "Util/NoticeCenter.h"
#include "WebApi.h"
#include "WebApiErrCode.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

TEST(WebApiResponseTest, MapsRecordedThumbnailTimeoutToGatewayTimeout) {
    EXPECT_EQ(504, getStatusCode(ApiErrCode::CODE_EXTRACT_THUMBNAIL_TIMEOUT));
}

TEST(MediaSeekEventTest, EmitsBooleanResolveFlagWithoutTypeMismatch) {
    const std::string event = "test.media.seeked2.boolean.flag";
    int listener_tag = 0;
    int callback_count = 0;
    bool last_resolve_flag = false;
    NoticeCenter::Instance().addListener(&listener_tag, event,
        [&](BroadcastMediaSeeked2Args) {
            ++callback_count;
            last_resolve_flag = resolve_recorded_block_path;
            invoker(max_duration, {});
        });

    MediaTuple tuple = {DEFAULT_VHOST, "camera", "stream", ""};
    uint64_t callback_duration = 0;
    Broadcast::Seek2Invoker invoker = [&](const uint64_t &duration,
                                          const std::map<std::string, std::map<uint64_t, std::string>> &) {
        callback_duration = duration;
    };

    EXPECT_NO_THROW({
        EXPECT_EQ(1, NOTICE_EMIT(BroadcastMediaSeeked2Args, event,
                                 tuple, uint64_t(100), uint64_t(300), true, invoker));
    });
    EXPECT_EQ(1, callback_count);
    EXPECT_TRUE(last_resolve_flag);
    EXPECT_EQ(300U, callback_duration);

    EXPECT_NO_THROW({
        EXPECT_EQ(1, NOTICE_EMIT(BroadcastMediaSeeked2Args, event,
                                 tuple, uint64_t(100), uint64_t(300), false, invoker));
    });
    EXPECT_EQ(2, callback_count);
    EXPECT_FALSE(last_resolve_flag);
    NoticeCenter::Instance().delListener(&listener_tag, event);
}

TEST(WebApiResponseTest, AcceptsLocalVariableNamedMessage) {
    Json::Value val;
    HttpSession::KeyValue headerOut;
    int response_status = 0;
    std::string response_body;
    HttpSession::HttpResponseInvoker invoker(
        [&](int status, const StrCaseMap &, const std::string &body) {
            response_status = status;
            response_body = body;
        });

    std::string message = "HeadBucket failed";
    RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_CONN_FAILED, message);

    Json::Value response;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(response_body, response));
    EXPECT_EQ(404, response_status);
    EXPECT_EQ(914005, response["code"].asInt());
    EXPECT_EQ(message, response["msg"].asString());
}

#ifdef ENABLE_TIER_STORAGE
TEST(StoragePoolApiTest, MergesChangedConnectionFieldsAndPreservesMaskedCredentials) {
    StoragePool pool;
    pool.id = "pool-1";
    pool.name = "Old pool";
    pool.type = "MINIO";
    pool.tier = "COLD";
    pool.endpoint = Optional<std::string>("http://old:9000");
    pool.bucket = Optional<std::string>("old-bucket");
    pool.base_path = Optional<std::string>("old-path");
    pool.access_key = Optional<std::string>("old-access");
    pool.secret_key_enc = Optional<std::string>("old-secret");

    Json::Value body;
    body["id"] = pool.id;
    body["endpoint"] = "http://new:9000";
    body["bucket"] = "new-bucket";
    body["base_path"] = "new-path";
    body["access_key"] = "***********";
    body["secret_key"] = "";

    TierStorageBase::applyPoolPatch(pool, body);

    EXPECT_EQ("pool-1", pool.id);
    EXPECT_EQ("http://new:9000", pool.endpoint.value());
    EXPECT_EQ("new-bucket", pool.bucket.value());
    EXPECT_EQ("new-path", pool.base_path.value());
    EXPECT_EQ("old-access", pool.access_key.value());
    EXPECT_EQ("old-secret", pool.secret_key_enc.value());
}

TEST(StoragePoolApiTest, ReplacesCredentialsWhenNewValuesAreProvided) {
    StoragePool pool;
    pool.access_key = Optional<std::string>("old-access");
    pool.secret_key_enc = Optional<std::string>("old-secret");

    Json::Value body;
    body["access_key"] = "new-access";
    body["secret_key"] = "new-secret";
    TierStorageBase::applyPoolPatch(pool, body);

    EXPECT_EQ("new-access", pool.access_key.value());
    EXPECT_EQ("new-secret", pool.secret_key_enc.value());
}

// Without the AWS SDK an object pool is refused up front with a clear message, so the
// object-pool checks after this point only apply to builds that have object storage.
#ifdef ENABLE_AWS_SDK
#define SKIP_UNLESS_OBJECT_STORAGE(pool)
#else
#define SKIP_UNLESS_OBJECT_STORAGE(pool)                                                   \
    do {                                                                                   \
        std::string refused;                                                               \
        EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, refused));                  \
        EXPECT_NE(std::string::npos, refused.find("ENABLE_AWS_SDK=OFF")) << refused; \
        GTEST_SKIP() << "object storage is not built in (ENABLE_AWS_SDK=OFF)";              \
    } while (false)
#endif

TEST(StoragePoolApiTest, ParsesAdHocObjectPoolAndValidatesRequiredFields) {
    Json::Value body;
    body["name"] = "Test pool";
    body["type"] = "MINIO";
    body["tier"] = "COLD";
    body["endpoint"] = "http://minio:9000";
    body["bucket"] = "recordings";
    body["base_path"] = "archive";
    body["access_key"] = "access";
    body["secret_key"] = "secret";

    StoragePool pool = TierStorageBase::poolFromJson(body);
    SKIP_UNLESS_OBJECT_STORAGE(pool);
    std::string error;
    EXPECT_TRUE(TierStorageBase::validatePoolByType(pool, error)) << error;

    body.removeMember("bucket");
    pool = TierStorageBase::poolFromJson(body);
    EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, error));
}

TEST(StoragePoolApiTest, ValidatesQuotaCapacitySource) {
    Json::Value body;
    body["name"] = "Quota pool";
    body["type"] = "MINIO";
    body["tier"] = "COLD";
    body["endpoint"] = "http://minio:9000";
    body["bucket"] = "recordings";
    body["base_path"] = "archive";
    body["access_key"] = "access";
    body["secret_key"] = "secret";

    StoragePool pool = TierStorageBase::poolFromJson(body);
    SKIP_UNLESS_OBJECT_STORAGE(pool);
    std::string error;
    EXPECT_EQ("METRICS", pool.capacity_source);
    EXPECT_FALSE(pool.capacity_quota_bytes.has_value());
    EXPECT_TRUE(TierStorageBase::validatePoolByType(pool, error)) << error;

    body["capacity_source"] = "quota";
    pool = TierStorageBase::poolFromJson(body);
    EXPECT_EQ("QUOTA", pool.capacity_source);
    EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, error));
    EXPECT_NE(std::string::npos, error.find("capacity_quota_bytes is required")) << error;

    body["capacity_quota_bytes"] = "5000000000000";
    pool = TierStorageBase::poolFromJson(body);
    ASSERT_TRUE(pool.capacity_quota_bytes.has_value());
    EXPECT_EQ(5000000000000LL, pool.capacity_quota_bytes.value());
    EXPECT_TRUE(TierStorageBase::validatePoolByType(pool, error)) << error;

    for (const char *bad : {"0", "-5", "12abc", "1.5"}) {
        body["capacity_quota_bytes"] = bad;
        pool = TierStorageBase::poolFromJson(body);
        EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, error)) << bad;
    }
    body["capacity_quota_bytes"] = 1.5;
    pool = TierStorageBase::poolFromJson(body);
    EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, error));

    body["capacity_quota_bytes"] = Json::Int64(1000);
    body["capacity_source"] = "BOTH";
    pool = TierStorageBase::poolFromJson(body);
    EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, error));
    EXPECT_NE(std::string::npos, error.find("METRICS or QUOTA")) << error;

    StoragePool local;
    local.type = "LOCAL_DISK";
    local.tier = "HOT";
    local.mount_path = Optional<std::string>("/data");
    local.capacity_source = "QUOTA";
    local.capacity_quota_bytes = Optional<int64_t>(1000);
    EXPECT_FALSE(TierStorageBase::validatePoolByType(local, error));
    EXPECT_NE(std::string::npos, error.find("only supported for object storage")) << error;
}

TEST(StoragePoolApiTest, PatchesAndSerializesQuotaFields) {
    StoragePool pool;
    pool.type = "MINIO";
    pool.tier = "COLD";
    pool.capacity_source = "QUOTA";
    pool.capacity_quota_bytes = Optional<int64_t>(1000);

    Json::Value unrelated;
    unrelated["name"] = "Renamed";
    TierStorageBase::applyPoolPatch(pool, unrelated);
    EXPECT_EQ("QUOTA", pool.capacity_source);
    ASSERT_TRUE(pool.capacity_quota_bytes.has_value());
    EXPECT_EQ(1000, pool.capacity_quota_bytes.value());

    Json::Value detail = TierStorageBase::poolDetailToJson(pool);
    EXPECT_EQ("QUOTA", detail["capacity_source"].asString());
    EXPECT_EQ(1000, detail["capacity_quota_bytes"].asInt64());
    EXPECT_EQ(1000, pool.toJson()["capacity_quota_bytes"].asInt64());

    Json::Value body;
    body["capacity_quota_bytes"] = Json::Int64(2000);
    TierStorageBase::applyPoolPatch(pool, body);
    EXPECT_EQ(2000, pool.capacity_quota_bytes.value());

    body = Json::Value();
    body["capacity_source"] = "METRICS";
    body["capacity_quota_bytes"] = Json::nullValue;
    TierStorageBase::applyPoolPatch(pool, body);
    EXPECT_EQ("METRICS", pool.capacity_source);
    EXPECT_FALSE(pool.capacity_quota_bytes.has_value());
    EXPECT_TRUE(TierStorageBase::poolDetailToJson(pool)["capacity_quota_bytes"].isNull());
}

TEST(StoragePoolApiTest, SerializesPoolDetailWithoutExposingCredentials) {
    StoragePool pool;
    pool.id = "pool-1";
    pool.name = "Archive";
    pool.type = "MINIO";
    pool.tier = "COLD";
    pool.access_key = Optional<std::string>("access-value");
    pool.secret_key_enc = Optional<std::string>("secret-value");
    pool.capacity_available = false;
    pool.capacity_status = "UNAVAILABLE";
    pool.capacity_message = "HTTP 403";

    const Json::Value value = TierStorageBase::poolDetailToJson(pool);

    EXPECT_EQ("***********", value["access_key"].asString());
    EXPECT_EQ("***********", value["secret_key"].asString());
    EXPECT_FALSE(value["capacity_available"].asBool());
    EXPECT_EQ("UNAVAILABLE", value["capacity_status"].asString());
    EXPECT_EQ("HTTP 403", value["capacity_message"].asString());
}

TEST(StoragePoolApiTest, ValidatesPolicyAgainstProvidedPoolMap) {
    PolicyTierConfig hot;
    hot.tier = "HOT";
    hot.enabled = true;
    hot.pool_id = "hot-pool";
    hot.retain_until_days = 7;
    hot.overflow_action = "DELETE_OLDEST";

    PolicyDeleteConfig deletion;
    deletion.delete_after_days = 7;

    StoragePolicy policy;
    policy.tiers_json = StoragePolicy::tiersToJson({hot});
    Json::FastWriter writer;
    policy.delete_policy_json = writer.write(deletion.toJson());

    StoragePool pool;
    pool.id = "hot-pool";
    pool.type = "LOCAL_DISK";
    pool.tier = "HOT";
    pool.enabled = 1;
    TierStorageBase::StoragePoolMap pools;
    pools[pool.id] = pool;

    std::string error;
    EXPECT_TRUE(TierStorageBase::validateStoragePolicy(policy, pools, error)) << error;

    pools[pool.id].enabled = 0;
    EXPECT_FALSE(TierStorageBase::validateStoragePolicy(policy, pools, error));
    EXPECT_NE(std::string::npos, error.find("must exist and be enabled"));
}

TEST(StoragePoolApiTest, RejectsPoliciesThatWouldNeverDeleteOrDeleteImmediately) {
    StoragePool pool;
    pool.id = "hot-pool";
    pool.type = "LOCAL_DISK";
    pool.tier = "HOT";
    pool.enabled = 1;
    TierStorageBase::StoragePoolMap pools;
    pools[pool.id] = pool;

    PolicyTierConfig hot;
    hot.tier = "HOT";
    hot.enabled = true;
    hot.pool_id = pool.id;
    hot.retain_until_days = 7;
    hot.overflow_action = "MOVE_TO_NEXT_TIER";

    PolicyDeleteConfig deletion;
    deletion.delete_after_days = 7;
    Json::FastWriter writer;

    StoragePolicy policy;
    policy.tiers_json = StoragePolicy::tiersToJson({hot});
    policy.delete_policy_json = writer.write(deletion.toJson());

    std::string error;
    EXPECT_FALSE(TierStorageBase::validateStoragePolicy(policy, pools, error));
    EXPECT_NE(std::string::npos, error.find("last enabled tier")) << error;

    hot.overflow_action = "DELETE_OLDEST";
    hot.retain_until_days = 0;
    deletion.delete_after_days = 0;
    policy.tiers_json = StoragePolicy::tiersToJson({hot});
    policy.delete_policy_json = writer.write(deletion.toJson());
    EXPECT_FALSE(TierStorageBase::validateStoragePolicy(policy, pools, error));
    EXPECT_NE(std::string::npos, error.find("greater than 0 for DELETE_OLDEST")) << error;

    hot.retain_until_days = 1;
    deletion.delete_after_days = 1;
    policy.tiers_json = StoragePolicy::tiersToJson({hot});
    policy.delete_policy_json = writer.write(deletion.toJson());
    EXPECT_TRUE(TierStorageBase::validateStoragePolicy(policy, pools, error)) << error;
}

TEST(StoragePoolApiTest, SerializesJobViewsWithSharedRules) {
    RestoreJob restore;
    restore.job_id = "restore-1";
    restore.camera_id = "camera-1";
    restore.status = "RUNNING";
    restore.total_bytes = 100;
    restore.processed_bytes = 25;
    const Json::Value restore_value = TierStorageBase::restoreJobToJson(
        restore, "Front door", true);
    EXPECT_EQ(25, restore_value["progress_percent"].asInt());
    EXPECT_EQ("Front door", restore_value["camera_name"].asString());
    EXPECT_FALSE(restore_value.isMember("processed_bytes"));

    RestoreJobSnapshot snapshot;
    snapshot.job = restore;
    snapshot.playback_status = "RESTORING";
    snapshot.total_segment_count = 4;
    snapshot.restored_segment_count = 1;
    snapshot.remaining_segment_count = 3;
    snapshot.segment_progress_percent = 25;
    const Json::Value snapshot_value = TierStorageBase::restoreJobSnapshotToJson(snapshot);
    EXPECT_EQ("RESTORING", snapshot_value["playback_status"].asString());
    EXPECT_FALSE(snapshot_value["playback_ready"].asBool());
    EXPECT_EQ(4, snapshot_value["total_segment_count"].asInt());
    EXPECT_EQ(3, snapshot_value["remaining_segment_count"].asInt());
    EXPECT_EQ(25, snapshot_value["segment_progress_percent"].asInt());
    EXPECT_FALSE(snapshot_value.isMember("estimated_restore_seconds"));

    TieringJob tiering;
    tiering.job_id = "tier-1";
    tiering.camera_id = "camera-1";
    tiering.status = "CANCELLED";
    tiering.error_message = Optional<std::string>("POLICY_CHANGED: policy updated");
    const Json::Value tiering_value = TierStorageBase::tieringJobToJson(
        tiering, "", true);
    EXPECT_EQ("TIERING_JOB_POLICY_CHANGED", tiering_value["error_code"].asString());
    EXPECT_FALSE(tiering_value.isMember("camera_name"));
}
#endif
