#include <gtest/gtest.h>

#if defined(ENABLE_FFMPEG)
#include "Processor/MultiMediaSourceProcessor.h"

using namespace mediakit;

TEST(AutoProfileOverlayTest, KeepsDerivedTranscodeAcrossTrackReset) {
    ProtocolOption option;
    option.enable_rtsp = false;
    option.enable_rtmp = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    option.enable_ts = false;
    option.enable_fmp4 = true;
    option.enable_mp4 = false;
    option.enable_transcode = false;

    auto processor = std::make_shared<MultiMediaSourceProcessor>(
        MediaTuple(DEFAULT_VHOST, "record", "camera.replay.test", ""), option);

    TranscodeProcessor::Config config;
    config.output_schema = FMP4_SCHEMA;
    config.stream_suffix = std::string(TRANSCODE_SUFFIX) + "overlay-test";
    config.demand = true;

    processor->ensureTranscode(config);
    EXPECT_FALSE(processor->canClose());

    processor->resetTracks();
    EXPECT_FALSE(processor->canClose());

    processor->close();
}
#endif
