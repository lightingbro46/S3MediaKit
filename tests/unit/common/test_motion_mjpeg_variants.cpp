#include <gtest/gtest.h>

#ifdef ENABLE_MOTION

#include "Motion/MotionMjpegMediaSource.h"
#include "Poller/EventPoller.h"

using namespace mediakit;
using namespace toolkit;

namespace {

using Reader = MotionMjpegMediaSource::RingType::RingReader::Ptr;

MotionMjpegMediaSource::Ptr makeSource() {
    MediaTuple tuple;
    tuple.vhost = DEFAULT_VHOST;
    tuple.app = "cam";
    tuple.stream = "variant_test";
    return std::make_shared<MotionMjpegMediaSource>(tuple);
}

Reader attach(const MotionMjpegMediaSource::Ptr &src, const EventPoller::Ptr &poller,
              const std::string &tag, size_t max_variants = 8) {
    Reader reader;
    OverlayBuildOptions options;
    options.username = tag;
    poller->sync([&]() {
        reader = src->attachOverlayVariant(tag, std::vector<OverlayComponent>(), options, poller, max_variants);
    });
    return reader;
}

// Readers are released on their poller; wait until the source saw it.
void release(Reader &reader, const EventPoller::Ptr &poller) {
    reader = nullptr;
    poller->sync([]() {});
    poller->sync([]() {});
}

} // namespace

TEST(MotionMjpegVariantTest, SameTagSharesVariantAndDifferentTagsSplit) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto src = makeSource();

    auto a1 = attach(src, poller, "tag-a");
    auto a2 = attach(src, poller, "tag-a");
    auto b1 = attach(src, poller, "tag-b");
    ASSERT_TRUE(a1 && a2 && b1);

    auto variants = src->overlayVariants();
    ASSERT_EQ(2U, variants.size());
    EXPECT_EQ(2, variants["tag-a"]->ring->readerCount());
    EXPECT_EQ(1, variants["tag-b"]->ring->readerCount());
    EXPECT_EQ("tag-a", variants["tag-a"]->options.username);
    EXPECT_EQ(3, src->readerCount());
}

TEST(MotionMjpegVariantTest, VariantIsDroppedWithItsLastReader) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto src = makeSource();

    auto a1 = attach(src, poller, "tag-a");
    auto a2 = attach(src, poller, "tag-a");
    release(a1, poller);
    EXPECT_EQ(1U, src->overlayVariants().size());
    EXPECT_EQ(1, src->readerCount());

    release(a2, poller);
    EXPECT_TRUE(src->overlayVariants().empty());
    EXPECT_EQ(0, src->readerCount());
}

TEST(MotionMjpegVariantTest, NewVariantBeyondLimitIsRefused) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto src = makeSource();

    auto a = attach(src, poller, "tag-a", 2);
    auto b = attach(src, poller, "tag-b", 2);
    ASSERT_TRUE(a && b);
    EXPECT_FALSE(attach(src, poller, "tag-c", 2));
    // An existing variant still accepts viewers at the limit.
    auto a2 = attach(src, poller, "tag-a", 2);
    EXPECT_TRUE(a2);

    release(b, poller);
    EXPECT_TRUE(attach(src, poller, "tag-c", 2));
}

TEST(MotionMjpegVariantTest, ReaderGetsOnlyItsVariantFrames) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto src = makeSource();

    auto a = attach(src, poller, "tag-a");
    auto b = attach(src, poller, "tag-b");
    std::vector<uint64_t> got_a, got_b;
    poller->sync([&]() {
        a->setReadCB([&](const MotionJpegFrame::Ptr &pkt) { got_a.push_back(pkt->stamp_ms); });
        b->setReadCB([&](const MotionJpegFrame::Ptr &pkt) { got_b.push_back(pkt->stamp_ms); });
    });

    auto variants = src->overlayVariants();
    for (uint64_t stamp = 1; stamp <= 3; ++stamp) {
        auto pkt = std::make_shared<MotionJpegFrame>();
        pkt->stamp_ms = stamp;
        variants["tag-a"]->ring->write(pkt, true);
    }
    auto pkt = std::make_shared<MotionJpegFrame>();
    pkt->stamp_ms = 100;
    variants["tag-b"]->ring->write(pkt, true);
    poller->sync([]() {});

    EXPECT_EQ((std::vector<uint64_t>{1, 2, 3}), got_a);
    EXPECT_EQ((std::vector<uint64_t>{100}), got_b);
    release(a, poller);
    release(b, poller);
}

#endif // ENABLE_MOTION
