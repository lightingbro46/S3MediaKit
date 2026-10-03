#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Extension/Factory.h"
#include "Poller/EventPoller.h"
#include "Rtsp/Rtsp.h"
#include "ext-codec/JPEG.h"
#include "ext-codec/JPEGRtp.h"

#if defined(ENABLE_FFMPEG)
#include "Codec/Transcode.h"
#include "Transcode/MjpegIngestTranscoder.h"
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
}
#endif

using namespace mediakit;
using namespace toolkit;

namespace {

class ConfigGuard {
public:
    ConfigGuard(const std::string &key, const std::string &value) : _key(key) {
        auto &ini = mINI::Instance();
        auto it = ini.find(key);
        _old = it == ini.end() ? "" : it->second;
        ini[key] = value;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    }
    ~ConfigGuard() {
        mINI::Instance()[_key] = _old;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    }

private:
    std::string _key;
    std::string _old;
};

Frame::Ptr makeJpegFrame(const std::string &jpeg, uint64_t dts) {
    return Factory::getFrameFromBuffer(CodecJPEG, std::make_shared<BufferString>(jpeg), dts, dts);
}

#if defined(ENABLE_FFMPEG)
// Baseline JPEG of a moving gradient, encoded with FFmpeg's MJPEG encoder (yuvj420p, standard Huffman tables).
std::string encodeJpeg(int width, int height, int index) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        return "";
    }
    std::shared_ptr<AVCodecContext> ctx(avcodec_alloc_context3(codec), [](AVCodecContext *p) { avcodec_free_context(&p); });
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base = AVRational { 1, 25 };
    // RTP/JPEG (RFC 2435) only carries the standard Huffman tables, like camera output.
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "huffman", "default", 0);
    int ret = avcodec_open2(ctx.get(), codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        return "";
    }
    std::shared_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame *p) { av_frame_free(&p); });
    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_YUVJ420P;
    av_frame_get_buffer(frame.get(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            frame->data[0][y * frame->linesize[0] + x] = (uint8_t)(x + y + index * 4);
        }
    }
    for (int y = 0; y < height / 2; ++y) {
        memset(frame->data[1] + y * frame->linesize[1], 128, width / 2);
        memset(frame->data[2] + y * frame->linesize[2], 128, width / 2);
    }
    frame->pts = index;
    std::string out;
    if (avcodec_send_frame(ctx.get(), frame.get()) < 0) {
        return "";
    }
    std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });
    if (avcodec_receive_packet(ctx.get(), pkt.get()) == 0) {
        out.assign((const char *)pkt->data, pkt->size);
    }
    return out;
}
#endif

class RtpCollector : public RingDelegate<RtpPacket::Ptr> {
public:
    void onWrite(RtpPacket::Ptr rtp, bool) override { packets.emplace_back(std::move(rtp)); }
    std::vector<RtpPacket::Ptr> packets;
};

} // namespace

TEST(MjpegIngest, TrackKeepsOriginCodecAcrossClone) {
    auto track = Factory::getTrackByCodecId(CodecH264);
    ASSERT_TRUE(track);
    EXPECT_EQ(track->getOriginCodecId(), CodecH264);
    EXPECT_FALSE(track->transcoded());

    track->setOriginCodecId(CodecJPEG);
    auto copy = track->clone();
    EXPECT_EQ(copy->getCodecId(), CodecH264);
    EXPECT_EQ(copy->getOriginCodecId(), CodecJPEG);
    EXPECT_EQ(copy->getOriginCodecName(), "JPEG");
    EXPECT_TRUE(copy->transcoded());
}

#if defined(ENABLE_HLS) || defined(ENABLE_RTPPROXY)
#include "mpeg-proto.h"
TEST(MjpegIngest, JpegIsNotMappedToMpegTsJpeg2000) {
    // MPEG-TS has no MJPEG stream type; 0x21 is JPEG 2000 and made players decode garbage.
    EXPECT_EQ(getMpegIdByCodec(CodecJPEG), PSI_STREAM_RESERVED);
}
#endif

#if defined(ENABLE_FFMPEG)

TEST(MjpegIngest, JpegTrackReadsResolutionAndFollowsChanges) {
    auto first = encodeJpeg(320, 240, 0);
    auto resized = encodeJpeg(640, 480, 1);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(resized.empty());

    JPEGTrack track;
    // Not ready until the size and a frame interval are known.
    track.inputFrame(makeJpegFrame(first, 1000));
    track.inputFrame(makeJpegFrame(first, 1040));
    track.inputFrame(makeJpegFrame(first, 1080));
    ASSERT_TRUE(track.ready());
    EXPECT_EQ(track.getVideoWidth(), 320);
    EXPECT_EQ(track.getVideoHeight(), 240);
    EXPECT_NEAR(track.getVideoFps(), 25.0, 0.5);

    track.inputFrame(makeJpegFrame(resized, 1120));
    EXPECT_EQ(track.getVideoWidth(), 640);
    EXPECT_EQ(track.getVideoHeight(), 480);
}

TEST(MjpegIngest, JpegTrackIgnoresNonIncreasingStamps) {
    auto jpeg = encodeJpeg(160, 120, 0);
    ASSERT_FALSE(jpeg.empty());
    JPEGTrack track;
    track.inputFrame(makeJpegFrame(jpeg, 500));
    track.inputFrame(makeJpegFrame(jpeg, 500));
    track.inputFrame(makeJpegFrame(jpeg, 500));
    EXPECT_FALSE(track.ready());
}

TEST(MjpegIngest, RtpRoundTripUsesMillisecondStamp) {
    auto jpeg = encodeJpeg(320, 240, 0);
    ASSERT_FALSE(jpeg.empty());

    JPEGRtpEncoder encoder;
    encoder.setRtpInfo(0x1234, 1400, 90000, 26);
    auto ring = std::make_shared<RtpRing::RingType>();
    auto collector = std::make_shared<RtpCollector>();
    ring->setDelegate(collector);
    encoder.setRtpRing(ring);
    ASSERT_TRUE(encoder.inputFrame(makeJpegFrame(jpeg, 123456)));
    ASSERT_FALSE(collector->packets.empty());

    JPEGRtpDecoder decoder;
    std::vector<Frame::Ptr> frames;
    decoder.addDelegate([&frames](const Frame::Ptr &frame) {
        frames.emplace_back(Frame::getCacheAbleFrame(frame));
        return true;
    });
    ASSERT_GT(collector->packets.size(), 1u);
    for (size_t i = 0; i < collector->packets.size(); ++i) {
        // Only the first packet of the image is a key position for the RTSP GOP cache.
        EXPECT_EQ(decoder.inputRtp(collector->packets[i]), i == 0) << "packet " << i;
    }
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0]->dts(), 123456u);

    JPEGTrack track;
    track.inputFrame(frames[0]);
    track.inputFrame(frames[0]);
    EXPECT_EQ(track.getVideoWidth(), 320);
    EXPECT_EQ(track.getVideoHeight(), 240);
}

TEST(MjpegIngest, CreateFollowsConfig) {
    auto poller = EventPollerPool::Instance().getPoller(false);
    auto jpeg_track = std::make_shared<JPEGTrack>();
    {
        ConfigGuard off(Mjpeg::kIngestTranscode, "0");
        EXPECT_FALSE(MjpegIngestTranscoder::enabled());
        EXPECT_FALSE(MjpegIngestTranscoder::create(jpeg_track, poller));
    }
    ConfigGuard on(Mjpeg::kIngestTranscode, "1");
    EXPECT_TRUE(MjpegIngestTranscoder::enabled());
    EXPECT_FALSE(MjpegIngestTranscoder::create(Factory::getTrackByCodecId(CodecH264), poller));

    ConfigGuard limit(Mjpeg::kIngestMaxSessions, "1");
    auto first = MjpegIngestTranscoder::create(jpeg_track, poller);
    ASSERT_TRUE(first);
    EXPECT_EQ(MjpegIngestTranscoder::activeCount(), 1u);
    EXPECT_FALSE(MjpegIngestTranscoder::create(jpeg_track, poller));
    first = nullptr;
    EXPECT_EQ(MjpegIngestTranscoder::activeCount(), 0u);
}

TEST(MjpegIngest, TranscodesJpegToH264OnPoller) {
    ConfigGuard on(Mjpeg::kIngestTranscode, "1");
    ConfigGuard codec(Mjpeg::kIngestCodec, "H264");
    auto poller = EventPollerPool::Instance().getPoller(false);

    std::vector<std::string> jpegs;
    for (int i = 0; i < 10; ++i) {
        jpegs.emplace_back(encodeJpeg(320, 240, i));
        ASSERT_FALSE(jpegs.back().empty());
    }

    auto jpeg_track = std::make_shared<JPEGTrack>();
    jpeg_track->setIndex(3);
    auto transcoder = MjpegIngestTranscoder::create(jpeg_track, poller);
    ASSERT_TRUE(transcoder);
    auto output = transcoder->outputTrack();
    EXPECT_EQ(output->getCodecId(), CodecH264);
    EXPECT_EQ(output->getOriginCodecId(), CodecJPEG);
    EXPECT_EQ(output->getIndex(), 3);

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<Frame::Ptr> out;
    bool wrong_thread = false;
    output->addDelegate([&](const Frame::Ptr &frame) {
        std::lock_guard<std::mutex> lck(mtx);
        wrong_thread = wrong_thread || !poller->isCurrentThread();
        out.emplace_back(Frame::getCacheAbleFrame(frame));
        cv.notify_all();
        return true;
    });
    jpeg_track->addDelegate(transcoder);

    // Feed on the player poller, 25 fps.
    for (size_t i = 0; i < jpegs.size(); ++i) {
        auto frame = makeJpegFrame(jpegs[i], 1000 + i * 40);
        poller->sync([&]() { jpeg_track->inputFrame(frame); });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::unique_lock<std::mutex> lck(mtx);
        cv.wait_for(lck, std::chrono::seconds(5), [&]() {
            for (auto &frame : out) {
                if (frame->keyFrame()) {
                    return out.size() >= 3;
                }
            }
            return false;
        });
        ASSERT_FALSE(out.empty());
        EXPECT_FALSE(wrong_thread);
        bool have_key = false;
        uint64_t last_dts = 0;
        for (auto &frame : out) {
            EXPECT_EQ(frame->getCodecId(), CodecH264);
            EXPECT_EQ(frame->getIndex(), 3);
            have_key = have_key || frame->keyFrame();
            EXPECT_GE(frame->dts(), last_dts);
            last_dts = frame->dts();
        }
        EXPECT_TRUE(have_key);
        EXPECT_GE(out.front()->dts(), 1000u);
    }
    poller->sync([&]() {
        EXPECT_TRUE(output->ready());
        auto video = std::dynamic_pointer_cast<VideoTrack>(output);
        ASSERT_TRUE(video);
        EXPECT_EQ(video->getVideoWidth(), 320);
        EXPECT_EQ(video->getVideoHeight(), 240);
    });

    jpeg_track->delDelegate(transcoder.get());
    transcoder = nullptr;
    EXPECT_EQ(MjpegIngestTranscoder::activeCount(), 0u);
}

#endif // ENABLE_FFMPEG
