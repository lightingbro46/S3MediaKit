#include <gtest/gtest.h>

#include "Storage/BookmarkIndex.h"
#include "Storage/SyncRowVersion.h"
#include "Storage/SystemMetrics.h"
#include "Storage/TransactionLog.h"
#include "Storage/TransactionSequence.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResourceAssignment.h"
#if defined(ENABLE_TIER_STORAGE)
#include "Storage/StoragePool.h"
#include "Storage/StorageTierExtra.h"
#include "Storage/TieringJob.h"
#include "Storage/TierQualityJob.h"
#endif

using namespace managerkit;

namespace {

template <typename T>
void expectEntitySchema(const char *table) {
    EXPECT_EQ(table, EntityTraits<T>::tableName());
    EXPECT_FALSE(EntityTraits<T>::getColumns().empty());
}

} // namespace

TEST(StorageModelsTest, RoundTripsJsonModels) {
    BookmarkIndex index;
    index.bookmark_guid = "idx";
    index.camera_guid = "camera";
    index.start_time = 100;
    index.end_time = 200;
    index.tags_csv = "door,alarm";
    auto index_copy = BookmarkIndex::fromJson(index.toJson());
    EXPECT_EQ(index.bookmark_guid, index_copy.bookmark_guid);
    EXPECT_EQ(index.tags_csv, index_copy.tags_csv);

    TransactionLog log;
    log.sequence = 42;
    log.peer_guid = "node-a";
    log.tran_guid = "transaction";
    log.tran_data = "{\"id\":1}";
    auto log_copy = TransactionLog::fromJson(log.toJson());
    EXPECT_EQ(log.sequence, log_copy.sequence);
    EXPECT_EQ(log.tran_data, log_copy.tran_data);

    TransactionSequence sequence;
    sequence.peer_guid = "node-b";
    sequence.sequence = 99;
    auto sequence_copy = TransactionSequence::fromJson(sequence.toJson());
    EXPECT_EQ(sequence.peer_guid, sequence_copy.peer_guid);
    EXPECT_EQ(sequence.sequence, sequence_copy.sequence);

    VmsKvPair pair;
    pair.id = "kv";
    pair.resource_guid = "resource";
    pair.name = "retention";
    pair.value = "30";
    auto pair_copy = VmsKvPair::fromJson(pair.toJson());
    EXPECT_EQ(pair.id, pair_copy.id);
    EXPECT_EQ(pair.value, pair_copy.value);

    VmsResourceAssignment assignment;
    assignment.assignment_guid = "assignment";
    assignment.resource_guid = "resource";
    assignment.owner_peer_id = "server";
    auto assignment_copy = VmsResourceAssignment::fromJson(assignment.toJson());
    EXPECT_EQ(assignment.assignment_guid, assignment_copy.assignment_guid);
    EXPECT_EQ(assignment.owner_peer_id, assignment_copy.owner_peer_id);
}

TEST(StorageModelsTest, SyncRowVersionRoundTripAndOrdering) {
    SyncRowVersion v;
    v.table_name = "vms_kvpair";
    v.row_key = "cam:enableActive";
    v.group_key = "cam";
    v.ts_ms = 1789617436049LL;
    v.peer_guid = "node-b";
    v.deleted = 1;
    auto copy = SyncRowVersion::fromJson(v.toJson());
    EXPECT_EQ(v.table_name, copy.table_name);
    EXPECT_EQ(v.row_key, copy.row_key);
    EXPECT_EQ(v.group_key, copy.group_key);
    EXPECT_EQ(v.ts_ms, copy.ts_ms);
    EXPECT_EQ(v.peer_guid, copy.peer_guid);
    EXPECT_EQ(v.deleted, copy.deleted);
    expectEntitySchema<SyncRowVersion>("sync_row_version");
    EXPECT_EQ(2u, EntityTraits<SyncRowVersion>::getPrimaryKey().size());

    EXPECT_TRUE(SyncRowVersionImp::isNewer(2, "a", 1, "z"));
    EXPECT_FALSE(SyncRowVersionImp::isNewer(1, "z", 2, "a"));
    EXPECT_TRUE(SyncRowVersionImp::isNewer(5, "node-b", 5, "node-a"));
    EXPECT_FALSE(SyncRowVersionImp::isNewer(5, "node-a", 5, "node-a"));
    EXPECT_EQ("#group:cam", SyncRowVersionImp::groupRowKey("cam"));
}

TEST(StorageModelsTest, SerializesTieringAndMetricsModels) {
#if defined(ENABLE_TIER_STORAGE)
    TieringJob job;
    job.job_id = "job";
    job.camera_id = "camera";
    job.source_tier = "HOT";
    job.target_tier = "COLD";
    job.bytes_total = 1024;
    job.error_message = std::string("retry");
    job.job_trigger = tieringJobTriggerToString(TieringJobTrigger::PRESSURE);
    job.policy_id = "policy";
    job.policy_updated_at = 123;
    EXPECT_EQ("job", job.toJson()["job_id"].asString());
    EXPECT_EQ(1024, job.toJson()["bytes_total"].asInt64());
    EXPECT_EQ("retry", job.toJson()["error_message"].asString());
    EXPECT_EQ("PRESSURE", job.toJson()["job_trigger"].asString());
    EXPECT_EQ("policy", job.toJson()["policy_id"].asString());
    EXPECT_EQ(123, job.toJson()["policy_updated_at"].asInt64());
    EXPECT_EQ("COPY_MOVE", job.toJson()["operation"].asString());
    EXPECT_EQ(TieringJobTrigger::PRESSURE,
              tieringJobTriggerFromString(job.toJson()["job_trigger"].asString()));
    EXPECT_EQ(TieringJobTrigger::LEGACY, tieringJobTriggerFromString("UNKNOWN"));

    PolicyDeleteConfig deletion;
    deletion.external_pool_id = "archive-pool";
    EXPECT_EQ("archive-pool", deletion.toJson()["external_pool_id"].asString());

    SegmentTierRange range;
    range.range_id = "range";
    range.segment_count = 8;
    range.size_bytes = 4096;
    EXPECT_EQ(8, range.toJson()["segment_count"].asInt64());

    RestoreJob restore;
    restore.job_id = "restore";
    restore.processed_bytes = 512;
    EXPECT_EQ("restore", restore.toJson()["job_id"].asString());
    EXPECT_EQ(512, restore.toJson()["processed_bytes"].asInt64());

    StorageAlert alert;
    alert.id = "alert";
    alert.acknowledged = 1;
    EXPECT_EQ("alert", alert.toJson()["id"].asString());
    EXPECT_TRUE(alert.toJson()["acknowledged"].asBool());

    ProtectedVideo video;
    video.protected_id = "protected";
    video.reason = Optional<std::string>("evidence");
    EXPECT_EQ("protected", video.toJson()["protected_id"].asString());
    EXPECT_EQ("evidence", video.toJson()["reason"].asString());

    StoragePool pool;
    pool.capacity_available = true;
    pool.capacity_status = "AVAILABLE";
    pool.capacity_message = "capacity collected";
    const Json::Value pool_json = pool.toJson();
    EXPECT_TRUE(pool_json["capacity_available"].asBool());
    EXPECT_EQ("AVAILABLE", pool_json["capacity_status"].asString());
    EXPECT_EQ("capacity collected", pool_json["capacity_message"].asString());
#endif

    SystemMetric metric;
    metric.id = 12;
    metric.cpu_usage_pct = 12.5;
    metric.nets_json = "[{\"name\":\"eth0\"}]";
    metric.disks_json = "invalid";
    auto metric_json = metric.toJson();
    EXPECT_EQ(12, metric_json["id"].asInt64());
    EXPECT_DOUBLE_EQ(12.5, metric_json["cpu_usage_pct"].asDouble());
    EXPECT_EQ(1U, metric_json["nets"].size());
    EXPECT_TRUE(metric_json["disks"].isArray());
}

#if defined(ENABLE_TIER_STORAGE)
TEST(StorageModelsTest, RoundTripsEveryPolicyAdvancedRule) {
    PolicyAdvancedRules rules;
    rules.enable_tier_quality_reduction = true;
    rules.enable_early_move_when_pool_high = false;
    rules.prefer_move_no_event_video_first = false;
    rules.prefer_keep_event_video_longer = false;
    rules.skip_move_if_pool_offline = false;
    rules.alert_when_pool_critical = false;
    rules.min_segment_age_minutes_before_move = 7;

    const PolicyAdvancedRules parsed = PolicyAdvancedRules::fromJson(rules.toJson());
    EXPECT_TRUE(parsed.enable_tier_quality_reduction);
    EXPECT_FALSE(parsed.enable_early_move_when_pool_high);
    EXPECT_FALSE(parsed.prefer_move_no_event_video_first);
    EXPECT_FALSE(parsed.prefer_keep_event_video_longer);
    EXPECT_FALSE(parsed.skip_move_if_pool_offline);
    EXPECT_FALSE(parsed.alert_when_pool_critical);
    EXPECT_EQ(7, parsed.min_segment_age_minutes_before_move);
    EXPECT_FALSE(PolicyAdvancedRules::fromJson(Json::Value(Json::objectValue))
                     .enable_tier_quality_reduction);
}
#endif

TEST(StorageModelsTest, ExposesAllStorageSchemas) {
    expectEntitySchema<BookmarkIndex>("bookmark_index");
#if defined(ENABLE_TIER_STORAGE)
    expectEntitySchema<TieringJob>("tiering_jobs");
    expectEntitySchema<TieringJobSegment>("tiering_job_segments");
    expectEntitySchema<TierTimefileSizeUpdate>("tier_timefile_size_updates");
    const std::vector<std::string> quality_segment_key = {
        "job_id", "stream_id", "segment_path"};
    EXPECT_EQ(quality_segment_key,
              EntityTraits<TieringJobSegment>::getPrimaryKey());
    expectEntitySchema<SegmentTierRange>("segment_tier_ranges");
    expectEntitySchema<PoolMetrics>("pool_metrics");
    expectEntitySchema<RestoreJob>("restore_jobs");
    expectEntitySchema<RestoreJobSegment>("restore_job_segments");
    const std::vector<std::string> restore_segment_key = {
        "job_id", "stream_id", "segment_path"};
    EXPECT_EQ(restore_segment_key,
              EntityTraits<RestoreJobSegment>::getPrimaryKey());
    expectEntitySchema<TierMetadataCleanupJob>("tier_metadata_cleanup_jobs");
    expectEntitySchema<SegmentRetentionOverride>("segment_retention_overrides");
    expectEntitySchema<StorageAlert>("storage_alerts");
    expectEntitySchema<ProtectedVideo>("protected_videos");
    expectEntitySchema<TransactionLog>("transaction_log");
    expectEntitySchema<TransactionSequence>("transaction_sequence");
    expectEntitySchema<VmsKvPair>("vms_kvpair");
    expectEntitySchema<VmsResourceAssignment>("vms_resource_assignment");
    expectEntitySchema<StoragePool>("storage_pools");
#endif
    expectEntitySchema<SystemMetric>("system_metrics");
}
