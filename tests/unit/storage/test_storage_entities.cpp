#include <gtest/gtest.h>

#include "Local/StorageTier.h"
#include "Storage/AuditLog.h"
#include "Storage/Bookmark.h"
#include "Storage/StoragePolicy.h"
#include "Storage/TransactionPeerAckLog.h"
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"

using namespace managerkit;

TEST(StorageTierTest, ConvertsEnumsAndReportsCapabilities) {
    EXPECT_EQ("HOT", tierTypeToString(HotTier));
    EXPECT_EQ(WarmTier, tierTypeFromString("WARM"));
    EXPECT_EQ(TierMax, tierTypeFromString("bad"));
    EXPECT_EQ("MINIO", poolTypeToString(StoragePoolType::MINIO));
    EXPECT_EQ(StoragePoolType::S3, poolTypeFromString("S3"));
    EXPECT_TRUE(poolTypeIsObjectStorage("ARCHIVE"));
    EXPECT_FALSE(poolTypeIsObjectStorage("NAS"));
    EXPECT_TRUE(poolTypeSupport(HotTier)["LOCAL_DISK"].asBool());
    EXPECT_TRUE(poolTypeSupport(ColdTier)["MINIO"].asBool());
    EXPECT_EQ("CRITICAL", healthToString(StorageHealth::CRITICAL));
    EXPECT_EQ("SNAPSHOT_ONLY", tierDataModeToString(TierDataMode::SNAPSHOT_ONLY));
    EXPECT_EQ("DELETE_OLDEST", overflowActionToString(OverflowAction::DELETE_OLDEST));
    EXPECT_EQ(JobStatus::FAILED, jobStatusFromString("FAILED"));
    EXPECT_EQ("PROJECT", policySourceToString(PolicySource::PROJECT));
    EXPECT_EQ("MISSING", segmentStatusToString(SegmentStatus::MISSING));
}

TEST(StoragePolicyTest, RoundTripsNestedConfiguration) {
    PolicyTierConfig tier;
    tier.tier = "COLD";
    tier.enabled = true;
    tier.pool_id = "pool-1";
    tier.retain_until_days = 90;
    const Json::Value tier_json = tier.toJson();
    PolicyTierConfig tier_copy = PolicyTierConfig::fromJson(tier_json);
    EXPECT_EQ("COLD", tier_copy.tier);
    EXPECT_EQ(90, tier_copy.retain_until_days);

    StoragePolicy policy;
    policy.id = "p1";
    policy.name = "Default";
    policy.description = Optional<std::string>("description");
    policy.tiers_json = StoragePolicy::tiersToJson({tier});
    policy.delete_policy_json = "{\"delete_after_days\":120}";
    policy.advanced_rules_json = "{}";
    ASSERT_EQ(1U, policy.parsedTiers().size());
    EXPECT_EQ(120, policy.parsedDeletePolicy().delete_after_days);
    Json::Value json = policy.toJson();
    EXPECT_EQ("p1", json["id"].asString());
    EXPECT_EQ(1U, json["tiers"].size());
}

TEST(BookmarkTest, RoundTripsOptionalJsonAndEntityRows) {
    Bookmark bookmark;
    bookmark.guid = "b1";
    bookmark.camera_guid = "c1";
    bookmark.start_time = 100;
    bookmark.duration = 30;
    bookmark.end_time = Optional<int64_t>(130);
    bookmark.name = Optional<std::string>("event");
    Json::Value json = bookmark.toJson();
    Bookmark restored = Bookmark::fromJson(json);
    EXPECT_EQ("b1", restored.guid);
    EXPECT_EQ(130, restored.end_time.value());
    EXPECT_EQ("event", restored.name.value());
    EXPECT_EQ(10U, EntityTraits<Bookmark>::getColumns().size());
    EXPECT_EQ("b1", EntityTraits<Bookmark>::getPrimaryKeyValue(bookmark)[0]);
}

TEST(StorageJsonTest, RoundTripsAuditResourceStatusAndAckLog) {
    AuditLog audit = {1, 10, 20, 30, 4, "r", "p", "s"};
    AuditLog audit_copy = AuditLog::fromJson(audit.toJson());
    EXPECT_EQ(4, audit_copy.event_type);
    EXPECT_EQ("r", audit_copy.resources);

    VmsResource resource = {"1", "g1", "parent", "name", "url", "xtype"};
    VmsResource resource_copy = VmsResource::fromJson(resource.toJson());
    EXPECT_EQ("g1", resource_copy.guid);
    EXPECT_EQ("url", resource_copy.url);

    VmsResourceStatus status = {"g1", 2};
    EXPECT_EQ(2, VmsResourceStatus::fromJson(status.toJson()).status);

    PeerAckLog ack = {"peer", "db", "source", "source-db", 99, 1234};
    PeerAckLog ack_copy = PeerAckLog::fromJson(ack.toJson());
    EXPECT_EQ(99, ack_copy.acked_seq);
    EXPECT_EQ((std::vector<std::string>{"peer_guid", "src_peer_guid"}),
              EntityTraits<PeerAckLog>::getPrimaryKey());
    EXPECT_EQ((std::vector<std::string>{"peer", "source"}),
              EntityTraits<PeerAckLog>::getPrimaryKeyValue(ack));
}
