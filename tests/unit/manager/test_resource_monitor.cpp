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
    auto disk_json = disk.toJson();
    EXPECT_EQ("/dev/sda1", disk_json["name"].asString());
    EXPECT_EQ("/data", disk_json["mount"].asString());
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

TEST(ReaderMonitorTest, TracksLiveAndRecordedReaders) {
    auto poller = toolkit::EventPollerPool::Instance().getPoller();
    ReaderMonitor monitor(poller);
    EXPECT_EQ(0, monitor.totalReaderCount());
    EXPECT_EQ(0, monitor.totalReaderCount("missing"));
    EXPECT_TRUE(monitor.getCurrentUsage().empty());

    monitor.setStreamReaderCount("camera-a", 3, false);
    monitor.setStreamReaderCount("camera-a", 2, true);
    monitor.setStreamReaderCount("camera-b", 4, false);
    EXPECT_EQ(9, monitor.totalReaderCount());
    EXPECT_EQ(5, monitor.totalReaderCount("camera-a"));
    EXPECT_EQ(4, monitor.totalReaderCount("camera-b"));
    auto usage = monitor.getCurrentUsage();
    ASSERT_EQ(2U, usage.size());
    EXPECT_EQ(3, usage["camera-a"].first);
    EXPECT_EQ(2, usage["camera-a"].second);

    monitor.setStreamReaderCount("camera-a", 1, false);
    EXPECT_EQ(7, monitor.totalReaderCount());
    EXPECT_EQ(3, monitor.totalReaderCount("camera-a"));
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

    monitor.setStreamReaderCount("camera", 2);
    EXPECT_FALSE(monitor.isReaderCountAvailable("camera"));
    EXPECT_FALSE(monitor.isReaderCountLimit("camera"));
    monitor.setStreamReaderCount("camera", 3);
    EXPECT_TRUE(monitor.isReaderCountLimit("camera"));

    monitor.setStreamReaderCount("other", 3);
    EXPECT_FALSE(monitor.isReaderCountAvailable());
    monitor.setStreamReaderCount("third", 1);
    EXPECT_TRUE(monitor.isReaderCountLimit("third"));
}
