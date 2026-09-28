#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <set>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>

#include "Common/MediaSource.h"
#include "Common/config.h"
#include "FMP4/FMP4MediaSource.h"
#include "Extension/Factory.h"
#include "Record/MP4Demuxer.h"
#include "Record/MP4Muxer.h"
#include "Record/MergedMP4Reader.h"
#include "Record/MP4Recorder.h"
#include "Util/File.h"
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Poller/EventPoller.h"
#include "Network/Session.h"

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

// The recorder's next file of a stream overlaps the previous one by up to a
// second (whole-second index). The next clip starts inside that file: it must
// be switched at the file's first keyframe, not at the clip start, where the
// frames up to the next keyframe (a GOP) would be dropped.
TEST_F(MergedReplayOpenTest, NextFileOfSameStreamPlaysWithoutGap) {
    const string first = exeDir() + "merged_replay_file_a.mp4";
    const string second = exeDir() + "merged_replay_file_b.mp4";
    writeRecording(first, 6000, 2000);
    writeRecording(second, 6000, 2000);

    // File B starts 1 s before file A ends; the planner cuts A at 6 s and
    // starts B at 1 s into its file. B's keyframes map to 5, 7 and 9 s.
    const uint64_t start = 1790000200;
    provideTimeline({clipOf(0, first, start, start, 6), clipOf(0, second, start + 5, start + 6, 5)});

    GET_CONFIG(string, record_app, Record::kAppName);
    MediaTuple tuple = {DEFAULT_VHOST, record_app, "cam/vod/" + to_string(start) + ".replay.files",
                        "quality=hi&prefered=hi&duration=0"};
    ProtocolOption option;
    option.enable_mp4 = option.enable_rtsp = option.enable_rtmp = false;
    option.enable_hls = option.enable_hls_fmp4 = option.enable_ts = false;
    option.enable_fmp4 = true;
    option.fmp4_demand = false;
    option.max_track = 16;
    option.modify_stamp = ProtocolOption::kModifyStampOff; // keep the timeline's own timestamps
    _reader = make_shared<MergedMP4Reader>(tuple, option);
    ASSERT_TRUE(_reader->openForReplay());
    ASSERT_GE(waitRegistered(tuple, 5000), 0);
    auto src = dynamic_pointer_cast<FMP4MediaSource>(MediaSource::find(FMP4_SCHEMA, tuple.vhost, tuple.app, tuple.stream));
    ASSERT_TRUE(src);

    auto poller = EventPollerPool::Instance().getPoller();
    auto stamps = make_shared<vector<uint64_t>>();
    auto mtx = make_shared<mutex>();
    FMP4MediaSource::RingType::RingReader::Ptr reader;
    poller->sync([&]() {
        reader = src->getRing()->attach(poller);
        reader->setReadCB([stamps, mtx](const FMP4MediaSource::RingDataType &list) {
            lock_guard<mutex> lck(*mtx);
            list->for_each([&](const FMP4Packet::Ptr &packet) { stamps->push_back(packet->time_stamp); });
        });
    });
    src->speed(10.0f);
    for (int i = 0; i < 100; ++i) {
        {
            lock_guard<mutex> lck(*mtx);
            if (!stamps->empty() && stamps->back() >= 10500) break;
        }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    poller->sync([&]() { reader.reset(); });

    lock_guard<mutex> lck(*mtx);
    ASSERT_GE(stamps->size(), 2u);
    uint64_t max_gap = 0, gap_at = 0;
    for (size_t i = 1; i < stamps->size(); ++i) {
        if ((*stamps)[i] > (*stamps)[i - 1] && (*stamps)[i] - (*stamps)[i - 1] > max_gap) {
            max_gap = (*stamps)[i] - (*stamps)[i - 1];
            gap_at = (*stamps)[i - 1];
        }
    }
    EXPECT_GE(stamps->back(), 10000u) << "replay did not reach the second file";
    EXPECT_LE(max_gap, 200u) << "gap of " << max_gap << " ms after " << gap_at << " ms";
    File::delete_file(first);
    File::delete_file(second);
}

// Per-stream replay (".../vod/<stamp>") opened with ?duration=N stops at the end
// of [stamp, stamp + N] instead of reading on into later recordings.
class PerStreamReplayDurationTest : public ::testing::Test {
protected:
    void SetUp() override {
        _first = exeDir() + "per_stream_duration_a.mp4";
        _second = exeDir() + "per_stream_duration_b.mp4";
        writeRecording(_first, 10000, 2000);
        writeRecording(_second, 10000, 2000);
        const auto first = _first, second = _second;
        const uint64_t file_start = kFileStart;
        NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastMediaSeeked2,
            [first, second, file_start](BroadcastMediaSeeked2Args) {
                map<string, map<uint64_t, string>> files;
                files["main"][file_start] = first;
                files["main"][file_start + 10] = second;
                invoker(20 - (stamp - file_start), files);
            });
    }
    void TearDown() override {
        NoticeCenter::Instance().delListener(this);
        File::delete_file(_first);
        File::delete_file(_second);
    }

    // Reads the whole replay; returns the last video dts, -1 when nothing was read.
    int64_t lastVideoDts(uint64_t duration_s) {
        MultiMP4Demuxer demuxer;
        demuxer.setMaxDuration(duration_s);
        demuxer.openMP4("/rec/record/cam/main/vod/" + to_string(kFileStart + 4));
        int64_t last = -1;
        bool key = false, eof = false;
        for (int i = 0; i < 5000 && !eof; ++i) {
            auto frame = demuxer.readFrame(key, eof);
            if (frame && frame->getTrackType() == TrackVideo) last = frame->dts();
        }
        EXPECT_TRUE(eof);
        return last;
    }

    static const uint64_t kFileStart = 1790000300;
    string _first, _second;
};

TEST_F(PerStreamReplayDurationTest, StopsAtStampPlusDuration) {
    // Timestamps count from the first file's start: the window [4 s, 10 s).
    const auto last = lastVideoDts(6);
    EXPECT_GE(last, 9000);
    EXPECT_LT(last, 10000);
}

TEST_F(PerStreamReplayDurationTest, WithoutDurationReadsOnIntoTheNextFile) {
    EXPECT_GE(lastVideoDts(0), 19000);
}

namespace {
class IdleSession : public Session {
public:
    using Session::Session;
    void onRecv(const Buffer::Ptr &) override {}
    void onError(const SockException &) override {}
    void onManager() override {}
};
} // namespace

// A derived (overlay/transcode) replay stream is published by TranscodeProcessor, not opened from
// MP4: findAsync must keep waiting for its registration instead of answering "not found" at once.
TEST(PerStreamReplayOpenTest, DerivedStreamWaitsForRegistration) {
    auto &wait_ms = mINI::Instance()[General::kMaxStreamWaitTimeMS];
    const string saved = wait_ms;
    wait_ms = "1000";
    GET_CONFIG(string, record_app, Record::kAppName);
    MediaInfo info;
    info.schema = FMP4_SCHEMA;
    info.vhost = DEFAULT_VHOST;
    info.app = record_app;
    info.stream = string("cam-derived/vod/1790000400") + TRANSCODE_SUFFIX + ".overlay.test";

    auto poller = EventPollerPool::Instance().getPoller();
    auto session = make_shared<IdleSession>(Socket::createSocket(poller, false));
    auto answered_at = make_shared<atomic<int64_t>>(-1);
    auto begin = chrono::steady_clock::now();
    poller->sync([&]() {
        MediaSource::findAsync(info, session, [answered_at, begin](const MediaSource::Ptr &) {
            *answered_at = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - begin).count();
        });
    });
    for (int i = 0; i < 100 && *answered_at < 0; ++i) this_thread::sleep_for(chrono::milliseconds(30));
    wait_ms = saved;
    EXPECT_GE(answered_at->load(), 900) << "answered before the stream could register";
    poller->sync([&]() { session.reset(); });
}

// A per-stream replay used to be opened (recording index, tier lookups, MP4 headers) on the
// session's poller. It now opens on "replay open": the poller stays free while the index is slow,
// the source still registers for the player, and two requests at once open one reader.
TEST_F(PerStreamReplayDurationTest, OpensOffTheSessionPoller) {
    NoticeCenter::Instance().delListener(this, Broadcast::kBroadcastMediaSeeked2);
    auto opens = make_shared<atomic<int>>(0);
    auto on_poller = make_shared<atomic<bool>>(false);
    const auto first = _first, second = _second;
    const uint64_t file_start = kFileStart;
    NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastMediaSeeked2,
        [first, second, file_start, opens, on_poller](BroadcastMediaSeeked2Args) {
            if (!resolve_recorded_block_path) { // findSegmentDuration(): once per open
                ++*opens;
                if (EventPoller::getCurrentPoller()) *on_poller = true;
                this_thread::sleep_for(chrono::milliseconds(400)); // a slow recording index
            }
            map<string, map<uint64_t, string>> files;
            files["main"][file_start] = first;
            files["main"][file_start + 10] = second;
            invoker(20 - (stamp - file_start), files);
        });

    GET_CONFIG(string, record_app, Record::kAppName);
    MediaInfo info;
    info.schema = RTSP_SCHEMA;
    info.vhost = DEFAULT_VHOST;
    info.app = record_app;
    info.stream = "cam-open/main/vod/" + to_string(kFileStart + 4);

    auto poller = EventPollerPool::Instance().getPoller();
    auto session = make_shared<IdleSession>(Socket::createSocket(poller, false));
    auto found = make_shared<promise<int>>();
    auto found_future = found->get_future();
    auto answers = make_shared<atomic<int>>(0);
    poller->sync([&]() {
        for (int i = 0; i < 2; ++i) {
            MediaSource::findAsync(info, session, [found, answers](const MediaSource::Ptr &src) {
                if (++*answers == 2) found->set_value(src ? 1 : 0);
            });
        }
    });
    // While the index lookup sleeps on the replay open thread, the poller answers at once.
    auto begin = chrono::steady_clock::now();
    poller->sync([]() {});
    EXPECT_LT(chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - begin).count(), 100);

    ASSERT_EQ(future_status::ready, found_future.wait_for(chrono::seconds(10)));
    EXPECT_EQ(1, found_future.get()) << "the replay source never registered";
    EXPECT_EQ(1, opens->load()) << "two requests opened two readers";
    EXPECT_FALSE(on_poller->load()) << "the recording index was read on a poller";
    if (auto src = MediaSource::find(RTSP_SCHEMA, info.vhost, info.app, info.stream)) src->close(true);
    poller->sync([&]() { session.reset(); });
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

// Closing a segment (moov write, rename, kBroadcastRecordMP4 listeners that write the timefile and
// SQLite) used to run on a WorkThreadPool poller. It now runs on "rec close N" threads, and the
// segments of one stream close one after another in order.
TEST(MP4RecorderCloseTest, SegmentsOfAStreamCloseInOrderOffPollers) {
    const string key = "__defaultVhost__/record/cam-close/main";
    auto executor = MP4Recorder::closeExecutorFor(key);
    ASSERT_TRUE(executor);
    EXPECT_EQ(executor.get(), MP4Recorder::closeExecutorFor(key).get());

    mutex mtx;
    vector<int> order;
    set<string> threads;
    bool on_poller = false;
    const int count = 20;
    auto done = make_shared<promise<void>>();
    auto finished = done->get_future();
    for (int i = 0; i < count; ++i) {
        executor->async([&, i, done]() {
            // Later tasks must not overtake an earlier, slower one.
            if (i % 5 == 0) this_thread::sleep_for(chrono::milliseconds(5));
            lock_guard<mutex> lck(mtx);
            order.push_back(i);
            threads.insert(getThreadName());
            on_poller = on_poller || static_cast<bool>(EventPoller::getCurrentPoller());
            if (static_cast<int>(order.size()) == count) done->set_value();
        }, false);
    }
    ASSERT_EQ(future_status::ready, finished.wait_for(chrono::seconds(5)));
    for (int i = 0; i < count; ++i) EXPECT_EQ(i, order[i]);
    ASSERT_EQ(1u, threads.size());
    EXPECT_EQ(0u, threads.begin()->find("rec close ")) << *threads.begin();
    EXPECT_FALSE(on_poller);
}
