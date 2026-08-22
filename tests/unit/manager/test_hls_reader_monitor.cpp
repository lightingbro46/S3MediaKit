#include <gtest/gtest.h>

#include "Server/ReaderMonitor.h"

using namespace managerkit;

TEST(HlsReaderMonitorTest, UsesLatestSourceSnapshotInsteadOfAccumulatingRequests) {
    auto poller = toolkit::EventPollerPool::Instance().getPoller();
    ReaderMonitor monitor(poller);
    const std::string source_id = "camera-hls|live:camera-hls/main";

    monitor.setStreamReaderCount("camera-hls", source_id, 1);
    monitor.setStreamReaderCount("camera-hls", source_id, 1);
    EXPECT_EQ(1, monitor.totalReaderCount("camera-hls"));

    monitor.setStreamReaderCount("camera-hls", source_id, 2);
    EXPECT_EQ(2, monitor.totalReaderCount());

    monitor.setStreamReaderCount("camera-hls", source_id, 0);
    EXPECT_EQ(0, monitor.totalReaderCount());
    EXPECT_TRUE(monitor.getCurrentUsage().empty());
}
