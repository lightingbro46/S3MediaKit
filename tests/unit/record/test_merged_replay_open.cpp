#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "Common/MediaSource.h"
#include "Common/config.h"
#include "FMP4/FMP4MediaSource.h"
#include "Extension/Factory.h"
#include "Record/MP4Demuxer.h"
#include "Record/MP4Muxer.h"
#include "Record/MergedMP4Reader.h"
#include "Util/File.h"
#include "Util/NoticeCenter.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

const uint8_t kSps[] = {0x67, 0x42, 0xc0, 0x0b, 0xd9, 0x02, 0x8d, 0xb0, 0x11, 0x00, 0x00, 0x03,
                        0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x32, 0x0f, 0x14, 0x2a, 0x48}; // 160x96
const uint8_t kPps[] = {0x68, 0xcb, 0x83, 0xcb, 0x20};
const uint8_t kAacConfig[] = {0x15, 0x88}; // AAC-LC 8 kHz mono, like the camera's sub stream

Frame::Ptr nalFrame(CodecId codec, const vector<uint8_t> &payload, uint64_t dts, int index) {
    string data;
    if (codec == CodecH264) data.assign("\x00\x00\x00\x01", 4);
    data.append(reinterpret_cast<const char *>(payload.data()), payload.size());
    auto frame = Frame::getCacheAbleFrame(Factory::getFrameFromPtr(codec, data.data(), data.size(), dts, dts));
    frame->setIndex(index);
    return frame;
}

/**
 * Writes a recording the way the recorder does (MP4Muxer, audio as the first
 * trak): 25 fps H.264 with a keyframe every `gop_ms`, SPS/PPS in front of each
 * IDR, AAC 8 kHz. Slice payloads are dummies: replay never decodes them.
 */
void writeRecording(const string &path, uint32_t duration_ms, uint32_t gop_ms) {
    auto audio = Factory::getTrackByCodecId(CodecAAC, 8000, 1, 16);
    audio->setExtraData(kAacConfig, sizeof(kAacConfig));
    auto video = Factory::getTrackByCodecId(CodecH264);
    video->inputFrame(nalFrame(CodecH264, vector<uint8_t>(kSps, kSps + sizeof(kSps)), 0, video->getIndex()));
    video->inputFrame(nalFrame(CodecH264, vector<uint8_t>(kPps, kPps + sizeof(kPps)), 0, video->getIndex()));
    ASSERT_TRUE(audio->ready());
    ASSERT_TRUE(video->ready());

    MP4Muxer muxer;
    muxer.openMP4(path);
    ASSERT_TRUE(muxer.addTrack(audio));
    ASSERT_TRUE(muxer.addTrack(video));
    muxer.addTrackCompleted();

    uint64_t next_audio = 0;
    for (uint64_t dts = 0; dts < duration_ms; dts += 40) {
        for (; next_audio <= dts; next_audio += 128) { // 1024 samples at 8 kHz
            muxer.inputFrame(nalFrame(CodecAAC, vector<uint8_t>(32, 0x21), next_audio, audio->getIndex()));
        }
        if (dts % gop_ms == 0) {
            muxer.inputFrame(nalFrame(CodecH264, vector<uint8_t>(kSps, kSps + sizeof(kSps)), dts, video->getIndex()));
            muxer.inputFrame(nalFrame(CodecH264, vector<uint8_t>(kPps, kPps + sizeof(kPps)), dts, video->getIndex()));
            muxer.inputFrame(nalFrame(CodecH264, {0x65, 0x88, 0x84, 0x00, 0x33, 0xff}, dts, video->getIndex()));
        } else {
            muxer.inputFrame(nalFrame(CodecH264, {0x41, 0x9a, 0x02, 0x04, 0x11}, dts, video->getIndex()));
        }
    }
    muxer.closeMP4();
}

class MergedReplayOpenTest : public ::testing::Test {
protected:
    void TearDown() override {
        NoticeCenter::Instance().delListener(this);
        if (_reader) _reader->stopReplay();
        _reader.reset();
    }

    void provideTimeline(const vector<Broadcast::ReplayClip> &clips) {
        NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastGetStreamQuality,
            [](BroadcastGetStreamQualityArgs) { invoker({{0, "main"}, {1, "sub"}}); });
        NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastGetReplayTimeline,
            [clips](BroadcastGetReplayTimelineArgs) {
                uint64_t total = 0;
                for (const auto &clip : clips) total += clip.duration;
                invoker(clips, total);
            });
    }

    // Waits for the FMP4 source to register; returns milliseconds or -1 on timeout.
    int64_t waitRegistered(const MediaTuple &tuple, int timeout_ms) {
        auto begin = chrono::steady_clock::now();
        for (;;) {
            auto elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - begin).count();
            if (MediaSource::find(FMP4_SCHEMA, tuple.vhost, tuple.app, tuple.stream)) return elapsed;
            if (elapsed > timeout_ms) return -1;
            this_thread::sleep_for(chrono::milliseconds(20));
        }
    }

    MergedMP4Reader::Ptr _reader;
};

Broadcast::ReplayClip clipOf(int source, const string &path, uint64_t file_start, uint64_t start, uint32_t duration) {
    Broadcast::ReplayClip clip;
    clip.source = source;
    clip.stream_id = source == 0 ? "main" : "sub";
    clip.file_path = path;
    clip.file_start_time = file_start;
    clip.start_time = start;
    clip.duration = duration;
    return clip;
}

} // namespace

// The first clip must feed the muxer: the FMP4 source registers as soon as its
// tracks are ready, not when the replay switches to the next clip.
TEST_F(MergedReplayOpenTest, RegistersSourceWhileFirstClipPlays) {
    const string sub = exeDir() + "merged_replay_open_sub.mp4";
    writeRecording(sub, 10000, 2000);
    ASSERT_GT(File::fileSize(sub), 0u);

    // The stamp lies mid-GOP: the seek lands on the 4 s keyframe, frames up to
    // 5 s are dropped and playback starts at the 6 s keyframe.
    const uint64_t file_start = 1790000000;
    const uint64_t stamp = file_start + 5;
    provideTimeline({clipOf(1, sub, file_start, stamp, 5)});

    GET_CONFIG(string, record_app, Record::kAppName);
    MediaTuple tuple = {DEFAULT_VHOST, record_app, "cam/vod/" + to_string(stamp) + ".replay.test",
                        "quality=lo&prefered=hi&duration=0"};
    ProtocolOption option;
    option.enable_mp4 = option.enable_rtsp = option.enable_rtmp = false;
    option.enable_hls = option.enable_hls_fmp4 = option.enable_ts = false;
    option.enable_fmp4 = true;
    option.fmp4_demand = false;
    option.max_track = 16;
    _reader = make_shared<MergedMP4Reader>(tuple, option);
    _reader->openForReplay();

    const auto registered_ms = waitRegistered(tuple, 5000);
    EXPECT_GE(registered_ms, 0) << "FMP4 source not registered within 5 s of openForReplay";
    // Frames before the 6 s keyframe are skipped without pacing: no wait of a GOP.
    EXPECT_LE(registered_ms, 500);
    File::delete_file(sub);
}

// The first keyframe keeps the SPS/PPS that precede it: after a track reset
// (Main<->Sub switch) they carry the new resolution in-band for players that
// read fMP4 progressively and ignore the second init segment.
TEST(MP4MuxerParameterSetsTest, FirstKeyframeSampleCarriesSpsAndPps) {
    const string path = exeDir() + "mp4_muxer_parameter_sets.mp4";
    writeRecording(path, 3000, 2000);

    MP4Demuxer demuxer;
    demuxer.openMP4(path);
    Frame::Ptr first_video;
    bool key = false, eof = false;
    while (!first_video && !eof) {
        auto frame = demuxer.readFrame(key, eof);
        if (frame && frame->getTrackType() == TrackVideo) first_video = frame;
    }
    ASSERT_TRUE(first_video);
    EXPECT_TRUE(key);
    EXPECT_EQ(first_video->data()[first_video->prefixSize()] & 0x1f, 7) << "first video sample does not start with SPS";
    demuxer.closeMP4();
    File::delete_file(path);
}
