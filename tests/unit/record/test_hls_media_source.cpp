#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Http/HttpSession.h"
#include "Network/Socket.h"
#include "Record/HlsMediaSource.h"
#include "Poller/EventPoller.h"

using namespace mediakit;
using namespace toolkit;

namespace {

HlsMediaSource::Ptr makeHlsSource(const std::string &schema, const std::string &stream) {
    auto source = std::make_shared<HlsMediaSource>(schema, MediaTuple(DEFAULT_VHOST, "hls-unit", stream));
    source->setIndexFile("#EXTM3U\n");
    return source;
}

bool waitForReaderCount(const HlsMediaSource::Ptr &source, int expected) {
    for (size_t i = 0; i < 100 && source->readerCount() != expected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return source->readerCount() == expected;
}

} // namespace

TEST(HlsCookieDataTest, CountsOneReaderPerSessionAndDetachesOnDestruction) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto session = std::make_shared<HttpSession>(Socket::createSocket(poller));
    auto source = makeHlsSource(HLS_SCHEMA, "single-session");
    MediaInfo info("http://localhost/hls-unit/single-session/hls.m3u8");

    auto viewer = std::make_shared<HlsCookieData>(info, session, "viewer-1");
    viewer->setMediaSource(source);
    viewer->addByteUsage(1);
    EXPECT_TRUE(waitForReaderCount(source, 1));
    EXPECT_EQ("viewer-1", viewer->getSessionId());

    viewer.reset();
    EXPECT_TRUE(waitForReaderCount(source, 0));
}

TEST(HlsCookieDataTest, ConcurrentRefreshDoesNotAttachDuplicateReaders) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto session = std::make_shared<HttpSession>(Socket::createSocket(poller));
    auto source = makeHlsSource(HLS_SCHEMA, "concurrent-session");
    MediaInfo info("http://localhost/hls-unit/concurrent-session/hls.m3u8");
    auto viewer = std::make_shared<HlsCookieData>(info, session, "viewer-concurrent");

    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) {
        threads.emplace_back([viewer, source]() {
            for (size_t j = 0; j < 100; ++j) {
                viewer->setMediaSource(source);
                viewer->addByteUsage(1);
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(waitForReaderCount(source, 1));
    viewer.reset();
    EXPECT_TRUE(waitForReaderCount(source, 0));
}

TEST(HlsCookieDataTest, CountsDistinctPlaybackSessionsSeparately) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto session = std::make_shared<HttpSession>(Socket::createSocket(poller));
    auto source = makeHlsSource(HLS_SCHEMA, "distinct-sessions");
    MediaInfo info("http://localhost/hls-unit/distinct-sessions/hls.m3u8");
    auto first = std::make_shared<HlsCookieData>(info, session, "viewer-1");
    auto second = std::make_shared<HlsCookieData>(info, session, "viewer-2");

    first->setMediaSource(source);
    second->setMediaSource(source);
    EXPECT_TRUE(waitForReaderCount(source, 2));

    first.reset();
    EXPECT_TRUE(waitForReaderCount(source, 1));
    second.reset();
    EXPECT_TRUE(waitForReaderCount(source, 0));
}

TEST(HlsCookieDataTest, SwitchingRenditionMovesTheReader) {
    auto poller = EventPollerPool::Instance().getPoller();
    auto session = std::make_shared<HttpSession>(Socket::createSocket(poller));
    auto high = makeHlsSource(HLS_SCHEMA, "high");
    auto low = makeHlsSource(HLS_SCHEMA, "low");
    MediaInfo info("http://localhost/hls-unit/high/hls.m3u8");
    auto viewer = std::make_shared<HlsCookieData>(info, session, "viewer-switch");

    viewer->setMediaSource(high);
    ASSERT_TRUE(waitForReaderCount(high, 1));
    viewer->setMediaSource(low);

    EXPECT_TRUE(waitForReaderCount(high, 0));
    EXPECT_TRUE(waitForReaderCount(low, 1));
    viewer.reset();
    EXPECT_TRUE(waitForReaderCount(low, 0));
}
