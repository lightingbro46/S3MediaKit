#include <gtest/gtest.h>

#include <cmath>

#include "Server/HddMonitor.h"
#include "Server/NetworkMonitor.h"
#include "Server/ReaderMonitor.h"
#include "Server/ResourceMonitor.h"

using namespace managerkit;

TEST(ResourceMonitorHelpersTest, FormatsNumbersBytesAndDurations) {
    EXPECT_EQ("12.35", format_double_2f(12.345));
    EXPECT_EQ("1.50", format_float_2f(1.5F));
    EXPECT_EQ("0.00 B", format_bytes_human_readable(0));
    EXPECT_EQ("1023.00 B", format_bytes_human_readable(1023));
    EXPECT_EQ("1.00 KB", format_bytes_human_readable(1024));
    EXPECT_EQ("1.50 MB", format_bytes_human_readable(1572864));
    EXPECT_EQ("1.00 GB", format_bytes_human_readable(1073741824ULL));
    EXPECT_EQ("0ms", format_duration_verbose(0));
    EXPECT_EQ("1s 1ms", format_duration_verbose(1001));
    EXPECT_EQ("1m 1s 1ms", format_duration_verbose(61001));
    EXPECT_EQ("1h 1m 1s 1ms", format_duration_verbose(3661001));
}

TEST(ResourceMonitorHelpersTest, SanitizesNonFiniteAndNegativeJsonNumbers) {
    EXPECT_EQ("1.25", sanitize_for_json(1.25));
    EXPECT_EQ("2.50", sanitize_for_json(2.5F));
    EXPECT_EQ("0.00", sanitize_for_json(-1.0));
    EXPECT_EQ("0.00", sanitize_for_json(-0.0F));
    EXPECT_EQ("0.00", sanitize_for_json(std::numeric_limits<double>::infinity()));
    EXPECT_EQ("0.00", sanitize_for_json(std::numeric_limits<float>::quiet_NaN()));
}

TEST(ResourceMonitorHelpersTest, ConvertsResourceTypes) {
    EXPECT_EQ("CPU", getResourceTypeString(ResourceType::CPU));
    EXPECT_EQ("MEMORY", getResourceTypeString(ResourceType::MEMORY));
    EXPECT_EQ("NETWORK", getResourceTypeString(ResourceType::NETWORK));
    EXPECT_EQ("HDD", getResourceTypeString(ResourceType::HDD));
    EXPECT_EQ("READER", getResourceTypeString(ResourceType::READER));
    EXPECT_EQ("unknown", getResourceTypeString(static_cast<ResourceType>(99)));
}

TEST(ResourceMonitorModelsTest, SerializesDiskAndNetworkSnapshots) {
    DiskPartition disk;
    disk.device = "/dev/sda1";
    disk.mount_point = "/data";
    disk.filesystem_type = "EXT4";
    disk.used_bytes = 25;
    disk.total_bytes = 100;
    disk.usage_pct = 25.0F;
    disk.read_only = true;
    disk.last_seen_at = 1234;
    auto disk_json = disk.toJson();
    EXPECT_EQ("/dev/sda1", disk_json["name"].asString());
    EXPECT_EQ("/data", disk_json["mount"].asString());
    EXPECT_EQ("EXT4", disk_json["filesystem_type"].asString());
    EXPECT_TRUE(disk_json["read_only"].asBool());
    EXPECT_EQ(1234, disk_json["last_seen_at"].asInt64());
    EXPECT_EQ(25U, disk_json["used"].asUInt64());
    EXPECT_EQ("25.00", disk_json["used_pct"].asString());
    EXPECT_FALSE(disk.isNetworkFileSystem());
    disk.filesystem_type = "NfS4";
    EXPECT_TRUE(disk.isNetworkFileSystem());

    NetInterfaceInfo net;
    net.name = "eth0";
    net.ipv4 = "192.0.2.1";
    net.ipv6 = "2001:db8::1";
    net.mac_address = "00:11:22:33:44:55";
    net.speed_mbps = 1000;
    net.rx_mbps = 12.5F;
    net.tx_mbps = std::numeric_limits<float>::infinity();
    auto net_json = net.toJson();
    EXPECT_EQ("eth0", net_json["name"].asString());
    EXPECT_EQ("12.50", net_json["rx_mbps"].asString());
    EXPECT_EQ("0.00", net_json["tx_mbps"].asString());
    EXPECT_FLOAT_EQ(1000, net_json["speed_mbps"].asFloat());
}

TEST(HddMonitorTest, RefreshReplacesMountInventoryAndPreservesLastGoodSnapshotOnFailure) {
    int phase = 0;
    int provider_calls = 0;
    HddMonitor::PartitionProvider provider = [&](std::vector<DiskPartition> &partitions) {
        ++provider_calls;
        if (phase == 2)
            return false;
        DiskPartition disk;
        disk.device = phase == 0 ? "nas-a" : "nas-b";
        disk.mount_point = phase == 0 ? "/mnt/a" : "/mnt/b";
        disk.filesystem_type = "nfs4";
        disk.total_bytes = 100;
        disk.used_bytes = 25;
        disk.free_bytes = 75;
        disk.usage_pct = 25.0F;
        partitions.push_back(disk);
        return true;
    };

    HddMonitor monitor(toolkit::EventPollerPool::Instance().getPoller(), 3600.0F, provider);
    ASSERT_TRUE(monitor.refreshNow());
    DiskSnapshot first = monitor.getSnapshot();
    ASSERT_TRUE(first.valid);
    ASSERT_EQ(1U, first.partitions.size());
    EXPECT_EQ("/mnt/a", first.partitions[0].mount_point);
    EXPECT_GT(first.partitions[0].last_seen_at, 0);

    phase = 1;
    ASSERT_TRUE(monitor.refreshNow());
    DiskSnapshot second = monitor.getSnapshot();
    ASSERT_EQ(1U, second.partitions.size());
    EXPECT_EQ("/mnt/b", second.partitions[0].mount_point);
    EXPECT_GT(second.generation, first.generation);

    phase = 2;
    EXPECT_FALSE(monitor.refreshNow());
    DiskSnapshot after_failure = monitor.getSnapshot();
    ASSERT_EQ(1U, after_failure.partitions.size());
    EXPECT_EQ("/mnt/b", after_failure.partitions[0].mount_point);
    EXPECT_EQ(second.generation, after_failure.generation);
    EXPECT_EQ(3, provider_calls);
}

TEST(HddMonitorTest, FindsLongestMountUsingPathBoundaryWithoutRefreshing) {
    DiskSnapshot snapshot;
    snapshot.valid = true;
    DiskPartition root;
    root.device = "root";
    root.mount_point = "/";
    DiskPartition nas;
    nas.device = "nas";
    nas.mount_point = "/mnt/nas";
    DiskPartition nas2;
    nas2.device = "nas2";
    nas2.mount_point = "/mnt/nas2";
    snapshot.partitions.push_back(root);
    snapshot.partitions.push_back(nas);
    snapshot.partitions.push_back(nas2);

    DiskPartition found;
    ASSERT_TRUE(HddMonitor::findMountPoint(snapshot, "/mnt/nas/camera/file.mp4", found));
    EXPECT_EQ("nas", found.device);
    ASSERT_TRUE(HddMonitor::findMountPoint(snapshot, "/mnt/nas2/camera/file.mp4", found));
    EXPECT_EQ("nas2", found.device);
    ASSERT_TRUE(HddMonitor::findMountPoint(snapshot, "/mnt/nas-other/file.mp4", found));
    EXPECT_EQ("root", found.device);

    for (int i = 0; i < 600; ++i) {
        ASSERT_TRUE(HddMonitor::findMountPoint(snapshot, "/mnt/nas/camera/file.mp4", found));
    }
}

TEST(ReaderMonitorTest, TracksLiveAndRecordedReaders) {
    auto poller = toolkit::EventPollerPool::Instance().getPoller();
    ReaderMonitor monitor(poller);
    EXPECT_EQ(0, monitor.totalReaderCount());
    EXPECT_EQ(0, monitor.totalReaderCount("missing"));
    EXPECT_TRUE(monitor.getCurrentUsage().empty());

    monitor.setStreamReaderCount("camera-a", "camera-a|live:rtsp", 3);
    monitor.setStreamReaderCount("camera-a", "camera-a|record:fmp4", 2);
    monitor.setStreamReaderCount("camera-b", "camera-b|live:rtsp", 4);
    EXPECT_EQ(9, monitor.totalReaderCount());
    EXPECT_EQ(5, monitor.totalReaderCount("camera-a"));
    EXPECT_EQ(4, monitor.totalReaderCount("camera-b"));
    auto usage = monitor.getCurrentUsage();
    ASSERT_EQ(2U, usage.size());
    EXPECT_EQ(3, usage["camera-a"].first);
    EXPECT_EQ(2, usage["camera-a"].second);

    // Repeated schema events are normalized to the same source key by
    // GlobalMonitor. The snapshot must replace the old value instead of being
    // added again.
    monitor.setStreamReaderCount("camera-a", "camera-a|live:rtsp", 1);
    EXPECT_EQ(7, monitor.totalReaderCount());
    EXPECT_EQ(3, monitor.totalReaderCount("camera-a"));

    // Different replay timestamps are different source instances and should
    // both contribute to the camera's recorded reader count.
    monitor.setStreamReaderCount("camera-a", "camera-a|record:camera/vod/100", 2);
    monitor.setStreamReaderCount("camera-a", "camera-a|record:camera/vod/200", 1);
    EXPECT_EQ(10, monitor.totalReaderCount());
    EXPECT_EQ(6, monitor.totalReaderCount("camera-a"));

    monitor.setStreamReaderCount("camera-a", "camera-a|record:camera/vod/100", 0);
    EXPECT_EQ(8, monitor.totalReaderCount());
    EXPECT_EQ(4, monitor.totalReaderCount("camera-a"));
}

TEST(ReaderMonitorTest, AppliesGlobalAndPerStreamThresholds) {
    auto poller = toolkit::EventPollerPool::Instance().getPoller();
    ReaderMonitor monitor(poller);
    monitor.setThreshold(5, 7);
    EXPECT_EQ(std::make_pair(5.0F, 7.0F), monitor.getThreshold());
    monitor.setStreamReaderThreshold(3, 4);

    EXPECT_TRUE(monitor.isReaderCountAvailable());
    EXPECT_TRUE(monitor.isReaderCountAvailable("camera"));
    EXPECT_FALSE(monitor.isReaderCountLimit("camera"));

    monitor.setStreamReaderCount("camera", "camera|live:rtsp", 2);
    EXPECT_FALSE(monitor.isReaderCountAvailable("camera"));
    EXPECT_FALSE(monitor.isReaderCountLimit("camera"));
    monitor.setStreamReaderCount("camera", "camera|live:rtsp", 3);
    EXPECT_TRUE(monitor.isReaderCountLimit("camera"));

    monitor.setStreamReaderCount("other", "other|live:rtsp", 3);
    EXPECT_FALSE(monitor.isReaderCountAvailable());
    monitor.setStreamReaderCount("third", "third|live:rtsp", 1);
    EXPECT_TRUE(monitor.isReaderCountLimit("third"));
}
