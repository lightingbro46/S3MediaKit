#include <gtest/gtest.h>

#include <map>

#include "Extension/Factory.h"
#include "Record/MP4Muxer.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

const uint8_t kSps[] = {0x67, 0x42, 0xc0, 0x0b, 0xd9, 0x02, 0x8d, 0xb0, 0x11, 0x00, 0x00, 0x03,
                        0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x32, 0x0f, 0x14, 0x2a, 0x48}; // 160x96
const uint8_t kPps[] = {0x68, 0xcb, 0x83, 0xcb, 0x20};
const uint8_t kAacConfig[] = {0x15, 0x88}; // AAC-LC 8 kHz mono

Frame::Ptr nalFrame(CodecId codec, const vector<uint8_t> &payload, uint64_t dts, int index) {
    string data;
    if (codec == CodecH264) data.assign("\x00\x00\x00\x01", 4);
    data.append(reinterpret_cast<const char *>(payload.data()), payload.size());
    auto frame = Frame::getCacheAbleFrame(Factory::getFrameFromPtr(codec, data.data(), data.size(), dts, dts));
    frame->setIndex(index);
    return frame;
}

uint32_t be32(const string &s, size_t pos) {
    auto p = reinterpret_cast<const uint8_t *>(s.data() + pos);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t be64(const string &s, size_t pos) {
    return (uint64_t(be32(s, pos)) << 32) | be32(s, pos + 4);
}

struct Fragment {
    map<uint32_t, uint64_t> tfdt; // track_ID -> baseMediaDecodeTime
    map<uint32_t, uint64_t> sidx; // track_ID -> earliest_presentation_time
};

// Parses the sidx and moof/traf/{tfhd,tfdt} boxes of one fMP4 media segment.
void parseBoxes(const string &s, size_t begin, size_t end, Fragment &out, uint32_t *track_id) {
    while (begin + 8 <= end) {
        size_t size = be32(s, begin);
        auto type = s.substr(begin + 4, 4);
        if (size < 8 || begin + size > end) return;
        if (type == "moof" || type == "traf") {
            uint32_t traf_track = 0;
            parseBoxes(s, begin + 8, begin + size, out, &traf_track);
        } else if (type == "sidx") {
            out.sidx[be32(s, begin + 12)] = be64(s, begin + 20); // version 1
        } else if (type == "tfhd" && track_id) {
            *track_id = be32(s, begin + 12);
        } else if (type == "tfdt" && track_id) {
            out.tfdt[*track_id] = s[begin + 8] == 1 ? be64(s, begin + 12) : be32(s, begin + 12);
        }
        begin += size;
    }
}

// Drives MP4MuxerMemory through the same reset sequence as FMP4MediaSourceMuxer.
class SegmentCapture : public MP4MuxerMemory {
public:
    void open() {
        _audio = Factory::getTrackByCodecId(CodecAAC, 8000, 1, 16);
        _audio->setExtraData(kAacConfig, sizeof(kAacConfig));
        _video = Factory::getTrackByCodecId(CodecH264);
        _video->inputFrame(nalFrame(CodecH264, vector<uint8_t>(kSps, kSps + sizeof(kSps)), 0, _video->getIndex()));
        _video->inputFrame(nalFrame(CodecH264, vector<uint8_t>(kPps, kPps + sizeof(kPps)), 0, _video->getIndex()));
        addTrack(_audio);
        addTrack(_video);
        addTrackCompleted();
        getInitSegment();
    }

    // FMP4MediaSourceMuxer::resetTracks() + addTrackCompleted()
    void switchSource() {
        flush();
        auto offset = static_cast<int64_t>(getNextDts());
        resetTracks();
        open();
        seedStampOffsets(offset);
    }

    // 25 fps with a keyframe every second, AAC every 128 ms, from input dts `begin`.
    void feed(uint64_t begin, uint32_t duration_ms) {
        uint64_t next_audio = begin;
        for (uint64_t dts = begin; dts < begin + duration_ms; dts += 40) {
            for (; next_audio <= dts; next_audio += 128) {
                inputFrame(nalFrame(CodecAAC, vector<uint8_t>(32, 0x21), next_audio, _audio->getIndex()));
            }
            if ((dts - begin) % 1000 == 0) {
                inputFrame(nalFrame(CodecH264, vector<uint8_t>(kSps, kSps + sizeof(kSps)), dts, _video->getIndex()));
                inputFrame(nalFrame(CodecH264, vector<uint8_t>(kPps, kPps + sizeof(kPps)), dts, _video->getIndex()));
                inputFrame(nalFrame(CodecH264, {0x65, 0x88, 0x84, 0x00, 0x33, 0xff}, dts, _video->getIndex()));
            } else {
                inputFrame(nalFrame(CodecH264, {0x41, 0x9a, 0x02, 0x04, 0x11}, dts, _video->getIndex()));
            }
        }
    }

    vector<Fragment> fragments;

protected:
    void onSegmentData(string segment, uint64_t, bool) override {
        Fragment fragment;
        parseBoxes(segment, 0, segment.size(), fragment, nullptr);
        if (!fragment.tfdt.empty()) fragments.push_back(fragment);
    }

private:
    Track::Ptr _audio;
    Track::Ptr _video;
};

} // namespace

// After a Main<->Sub switch the new writer's fragments continue the timeline:
// tfdt must not restart at 0 nor repeat the last sample's time, and must agree
// with the absolute sidx time.
TEST(FMP4DecodeTimeTest, TfdtContinuesAcrossTrackReset) {
    SegmentCapture muxer;
    muxer.open();
    // Input dts 0 is avoided: DeltaStamp treats the frame after a 0 stamp as
    // the first one again, giving two samples the same time.
    muxer.feed(1000, 3000);
    muxer.flush();
    const size_t before_switch = muxer.fragments.size();
    ASSERT_GT(before_switch, 0u);
    const uint64_t switch_at = muxer.getNextDts();
    EXPECT_EQ(switch_at, 3000u); // last video sample at 2960 + 40 ms

    muxer.switchSource();
    muxer.feed(50000, 3000); // the next clip's own timestamps
    muxer.flush();
    ASSERT_GT(muxer.fragments.size(), before_switch);

    const uint32_t kVideoTrack = 2; // audio is added first
    const auto &first_after = muxer.fragments[before_switch];
    ASSERT_TRUE(first_after.tfdt.count(kVideoTrack));
    EXPECT_EQ(first_after.tfdt.at(kVideoTrack), switch_at);

    map<uint32_t, uint64_t> last;
    for (size_t i = 0; i < muxer.fragments.size(); ++i) {
        for (auto &pr : muxer.fragments[i].tfdt) {
            if (last.count(pr.first)) {
                EXPECT_GT(pr.second, last[pr.first]) << "fragment " << i << " track " << pr.first;
            }
            last[pr.first] = pr.second;
        }
        auto &fragment = muxer.fragments[i];
        if (fragment.tfdt.count(kVideoTrack) && fragment.sidx.count(kVideoTrack)) {
            // no B-frames: the first sample's pts equals its dts
            EXPECT_EQ(fragment.tfdt.at(kVideoTrack), fragment.sidx.at(kVideoTrack)) << "fragment " << i;
        }
    }
}
