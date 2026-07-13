#include <gtest/gtest.h>

#include "Rtmp/Rtmp.h"
#include "Common/PacketCache.h"
#include "Common/MediaSource.h"

using namespace mediakit;
using namespace toolkit;

namespace {

struct DummyPacket {
    explicit DummyPacket(int value) : value(value) {}
    int value;
};

template <typename Packet>
class CapturingPacketCache : public PacketCache<Packet> {
public:
    typedef toolkit::List<std::shared_ptr<Packet> > PacketList;
    std::vector<size_t> flush_sizes;
    std::vector<bool> key_positions;

    void onFlush(std::shared_ptr<PacketList> packets, bool key_pos) override {
        flush_sizes.emplace_back(packets->size());
        key_positions.emplace_back(key_pos);
    }
};

class ConfigGuard {
public:
    ConfigGuard(const std::string &key, const std::string &value) : _key(key) {
        auto &ini = toolkit::mINI::Instance();
        auto it = ini.find(key);
        _old = it == ini.end() ? "" : it->second;
        ini[key] = value;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    }
    ~ConfigGuard() {
        toolkit::mINI::Instance()[_key] = _old;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    }

private:
    std::string _key;
    std::string _old;
};

} // namespace

TEST(FlushPolicyTest, FlushesOnTimestampChangeRollbackKeyframeAndOverflow) {
    ConfigGuard merge(General::kMergeWriteMS, "0");
    FlushPolicy policy;
    EXPECT_FALSE(policy.isFlushAble(false, false, 0, 0));
    EXPECT_TRUE(policy.isFlushAble(false, false, 10, 1));
    EXPECT_FALSE(policy.isFlushAble(false, false, 10, 2));
    EXPECT_TRUE(policy.isFlushAble(false, false, 11, 2));
    EXPECT_TRUE(policy.isFlushAble(false, false, 1000, 1));
    EXPECT_TRUE(policy.isFlushAble(false, false, 100, 1));
    EXPECT_TRUE(policy.isFlushAble(false, false, 100, 1024));
    EXPECT_TRUE(policy.isFlushAble(true, true, 100, 0));
}

TEST(FlushPolicyTest, HonorsMergeWindow) {
    ConfigGuard merge(General::kMergeWriteMS, "100");
    FlushPolicy policy;
    EXPECT_FALSE(policy.isFlushAble(false, false, 10, 0));
    EXPECT_FALSE(policy.isFlushAble(false, false, 100, 2));
    EXPECT_TRUE(policy.isFlushAble(false, false, 101, 2));
    EXPECT_FALSE(policy.isFlushAble(false, false, 150, 2));
    EXPECT_TRUE(policy.isFlushAble(false, false, 1200, 2));
    EXPECT_TRUE(policy.isFlushAble(false, false, 100, 2));
    EXPECT_TRUE(policy.isFlushAble(true, false, 0, 1024));
}

TEST(PacketCacheTest, FlushesImmediatelyWhenMergeIsDisabled) {
    ConfigGuard merge(General::kMergeWriteMS, "0");
    CapturingPacketCache<DummyPacket> cache;
    cache.inputPacket(0, false, std::make_shared<DummyPacket>(1), false);
    cache.inputPacket(1, false, std::make_shared<DummyPacket>(2), true);
    ASSERT_EQ(2U, cache.flush_sizes.size());
    EXPECT_EQ(1U, cache.flush_sizes[0]);
    EXPECT_FALSE(cache.key_positions[0]);
    EXPECT_TRUE(cache.key_positions[1]);
    cache.flush();
    EXPECT_EQ(2U, cache.flush_sizes.size());
}

TEST(PacketCacheTest, BatchesWithinMergeWindowAndCanClear) {
    ConfigGuard merge(General::kMergeWriteMS, "100");
    CapturingPacketCache<DummyPacket> cache;
    cache.inputPacket(0, false, std::make_shared<DummyPacket>(1), false);
    cache.inputPacket(50, false, std::make_shared<DummyPacket>(2), false);
    cache.inputPacket(101, false, std::make_shared<DummyPacket>(3), false);
    ASSERT_EQ(1U, cache.flush_sizes.size());
    EXPECT_EQ(2U, cache.flush_sizes[0]);
    cache.clearCache();
    cache.flush();
    EXPECT_EQ(1U, cache.flush_sizes.size());

    cache.inputPacket(200, true, std::make_shared<DummyPacket>(4), true);
    cache.flush();
    ASSERT_EQ(2U, cache.flush_sizes.size());
    EXPECT_TRUE(cache.key_positions.back());
}

TEST(PacketCacheTest, RtpLowLatencyFlushesEachPacket) {
    ConfigGuard merge(General::kMergeWriteMS, "100");
    ConfigGuard low_latency(Rtsp::kLowLatency, "1");
    CapturingPacketCache<RtpPacket> cache;
    cache.inputPacket(0, true, RtpPacket::create(), false);
    cache.inputPacket(0, true, RtpPacket::create(), false);
    ASSERT_EQ(2U, cache.flush_sizes.size());
    EXPECT_EQ(1U, cache.flush_sizes[0]);
    EXPECT_EQ(1U, cache.flush_sizes[1]);
}
