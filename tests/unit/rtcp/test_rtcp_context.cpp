#include <gtest/gtest.h>

#include "Rtcp/RtcpContext.h"

using namespace mediakit;

TEST(RtcpRecvContextTest, CountsExpectedAndLostPackets) {
    RtcpContextForRecv context;
    context.onRtp(100, 9000, 1000, 90000, 100);
    context.onRtp(101, 12000, 1033, 90000, 120);
    context.onRtp(103, 15000, 1066, 90000, 140);
    EXPECT_EQ(4U, context.getExpectedPackets());
    EXPECT_EQ(1U, context.getLost());
    EXPECT_EQ(4U, context.getExpectedPacketsInterval());
    EXPECT_EQ(1U, context.getLostInterval());
    EXPECT_EQ(0U, context.getExpectedPacketsInterval());
    EXPECT_EQ(0U, context.getLostInterval());
}

TEST(RtcpRecvContextTest, HandlesSequenceWrapAndCreatesReceiverReport) {
    RtcpContextForRecv context;
    context.onRtp(65534, 100, 1000, 90000, 10);
    context.onRtp(65535, 200, 1001, 90000, 10);
    context.onRtp(0, 300, 1002, 90000, 10);
    context.onRtp(1, 400, 1003, 90000, 10);
    EXPECT_EQ(4U, context.getExpectedPackets());
    toolkit::Buffer::Ptr report = context.createRtcpRR(10, 20);
    ASSERT_TRUE(report);
    EXPECT_GT(report->size(), sizeof(RtcpHeader));
}

TEST(RtcpSendContextTest, CreatesSenderReportAndDefaultsRtt) {
    RtcpContextForSend context;
    context.onRtp(1, 90000, 10000, 90000, 1200);
    context.onRtp(2, 93000, 10033, 90000, 800);
    toolkit::Buffer::Ptr report = context.createRtcpSR(1234);
    ASSERT_TRUE(report);
    EXPECT_GT(report->size(), sizeof(RtcpHeader));
    EXPECT_EQ(0U, context.getRtt(999));
}
