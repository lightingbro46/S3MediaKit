#include <gtest/gtest.h>

#include "Rtmp/RtmpDemuxer.h"

using namespace mediakit;

namespace {

class CapturingTrackListener : public TrackListener {
public:
    std::vector<Track::Ptr> tracks;
    size_t completed = 0;
    size_t reset = 0;

    bool addTrack(const Track::Ptr &track) override {
        tracks.emplace_back(track);
        return true;
    }
    void addTrackCompleted() override { ++completed; }
    void resetTracks() override { ++reset; }
};

RtmpPacket::Ptr packet(uint8_t type, const std::string &body, uint32_t stamp = 0) {
    auto value = RtmpPacket::create();
    value->type_id = type;
    value->time_stamp = stamp;
    value->buffer = body;
    value->body_size = body.size();
    return value;
}

} // namespace

TEST(RtmpDemuxerTest, CountsTracksDeclaredByMetadata) {
    AMFValue metadata(AMF_OBJECT);
    EXPECT_EQ(0U, RtmpDemuxer::trackCount(metadata));
    metadata.set("videocodecid", AMFValue(7));
    EXPECT_EQ(1U, RtmpDemuxer::trackCount(metadata));
    metadata.set("audiocodecid", AMFValue(10));
    metadata.set("duration", AMFValue(12.5));
    EXPECT_EQ(2U, RtmpDemuxer::trackCount(metadata));
    EXPECT_THROW(RtmpDemuxer::trackCount(AMFValue(1)), std::runtime_error);
}

TEST(RtmpDemuxerTest, CreatesAudioVideoTracksFromMetadata) {
    RtmpDemuxer demuxer;
    CapturingTrackListener listener;
    demuxer.setTrackListener(&listener, false);

    AMFValue metadata(AMF_OBJECT);
    metadata.set("duration", AMFValue(42.25));
    metadata.set("videocodecid", AMFValue(7));
    metadata.set("videodatarate", AMFValue(1500));
    metadata.set("audiocodecid", AMFValue(10));
    metadata.set("audiosamplerate", AMFValue(44100));
    metadata.set("audiosamplesize", AMFValue(16));
    metadata.set("stereo", AMFValue(true));
    metadata.set("audiodatarate", AMFValue(128));

    EXPECT_TRUE(demuxer.loadMetaData(metadata));
    EXPECT_FLOAT_EQ(42.25F, demuxer.getDuration());
    ASSERT_EQ(2U, listener.tracks.size());
    EXPECT_EQ(1U, listener.completed);
    auto video = std::dynamic_pointer_cast<VideoTrack>(demuxer.getTrack(TrackVideo, false));
    auto audio = std::dynamic_pointer_cast<AudioTrack>(demuxer.getTrack(TrackAudio, false));
    ASSERT_TRUE(video);
    ASSERT_TRUE(audio);
    EXPECT_EQ(1500 * 1024, video->getBitRate());
    EXPECT_EQ(128 * 1024, audio->getBitRate());
    // AACTrack derives rate/channel from AudioSpecificConfig; metadata alone
    // creates the track but leaves these fields unknown until the config packet.
    EXPECT_EQ(0, audio->getAudioSampleRate());
    EXPECT_EQ(0, audio->getAudioChannel());
    EXPECT_EQ(16, audio->getAudioSampleBit());

    // Loading the same metadata is idempotent: decoders/tracks are not duplicated.
    EXPECT_TRUE(demuxer.loadMetaData(metadata));
    EXPECT_EQ(2U, listener.tracks.size());
    EXPECT_EQ(2U, listener.completed);
}

TEST(RtmpDemuxerTest, HandlesMissingMalformedAndUnsupportedMetadata) {
    RtmpDemuxer demuxer;
    CapturingTrackListener listener;
    demuxer.setTrackListener(&listener, false);
    EXPECT_FALSE(demuxer.loadMetaData(AMFValue(AMF_OBJECT)));
    EXPECT_FALSE(demuxer.loadMetaData(AMFValue(1)));

    AMFValue unsupported(AMF_OBJECT);
    unsupported.set("videocodecid", AMFValue(999));
    unsupported.set("audiocodecid", AMFValue(999));
    EXPECT_TRUE(demuxer.loadMetaData(unsupported));
    EXPECT_TRUE(listener.tracks.empty());
    EXPECT_EQ(1U, listener.completed);
}

TEST(RtmpDemuxerTest, DetectsTracksFromIncomingPackets) {
    RtmpDemuxer demuxer;
    CapturingTrackListener listener;
    demuxer.setTrackListener(&listener, false);

    // AVC sequence header: classic H264 flags plus a minimal AVCDecoderConfigurationRecord.
    std::string avc("\x17\x00\x00\x00\x00", 5);
    avc.append("\x01\x64\x00\x1F\xFF\xE1\x00\x04\x67\x64\x00\x1F\x01\x00\x02\x68\xEE", 17);
    demuxer.inputRtmp(packet(MSG_VIDEO, avc));

    std::string aac("\xAF\x00\x12\x10", 4);
    demuxer.inputRtmp(packet(MSG_AUDIO, aac));
    demuxer.inputRtmp(packet(MSG_ACK, "ignored"));

    EXPECT_TRUE(demuxer.getTrack(TrackVideo, false));
    EXPECT_TRUE(demuxer.getTrack(TrackAudio, false));
    EXPECT_EQ(2U, listener.tracks.size());
}
