#include <gtest/gtest.h>

#include "Storage/BookmarkIndex.h"
#include "Storage/StoragePool.h"
#include "Storage/StorageTierExtra.h"
#include "Storage/SystemMetrics.h"
#include "Storage/TieringJob.h"
#include "Storage/TransactionLog.h"
#include "Storage/TransactionSequence.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResourceAssignment.h"

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

TEST(StorageModelsTest, SerializesTieringAndMetricsModels) {
    TieringJob job;
    job.job_id = "job";
    job.camera_id = "camera";
    job.source_tier = "HOT";
    job.target_tier = "COLD";
    job.bytes_total = 1024;
    job.error_message = std::string("retry");
    EXPECT_EQ("job", job.toJson()["job_id"].asString());
    EXPECT_EQ(1024, job.toJson()["bytes_total"].asInt64());
    EXPECT_EQ("retry", job.toJson()["error_message"].asString());

    SegmentTierRecord record;
    record.camera_id = "camera";
    record.segment_path = "2026/segment.mp4";
    record.file_size = 2048;
    EXPECT_EQ(2048, record.toJson()["file_size"].asInt64());

    SegmentTierRange range;
    range.range_id = "range";
    range.segment_count = 8;
    range.size_bytes = 4096;
    EXPECT_EQ(8, range.toJson()["segment_count"].asInt64());

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

TEST(StorageModelsTest, ExposesAllStorageSchemas) {
    expectEntitySchema<BookmarkIndex>("bookmark_index");
    expectEntitySchema<TieringJob>("tiering_jobs");
    expectEntitySchema<SegmentTierRecord>("segment_tier_records");
    expectEntitySchema<SegmentTierRange>("segment_tier_ranges");
    expectEntitySchema<PoolMetrics>("pool_metrics");
    expectEntitySchema<RestoreJob>("restore_jobs");
    expectEntitySchema<StorageAlert>("storage_alerts");
    expectEntitySchema<ProtectedVideo>("protected_videos");
    expectEntitySchema<TransactionLog>("transaction_log");
    expectEntitySchema<TransactionSequence>("transaction_sequence");
    expectEntitySchema<VmsKvPair>("vms_kvpair");
    expectEntitySchema<VmsResourceAssignment>("vms_resource_assignment");
    expectEntitySchema<StoragePool>("storage_pools");
    expectEntitySchema<SystemMetric>("system_metrics");
}
