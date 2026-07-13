#include <gtest/gtest.h>

#include "ext-codec/AAC.h"
#include "ext-codec/G711.h"
#include "ext-codec/L16.h"
#include "ext-codec/MP3.h"
#include "ext-codec/Opus.h"

namespace mediakit {
int getAacFrameLength(const uint8_t *data, size_t bytes);
std::string makeAacConfig(const uint8_t *hex, size_t length);
int dumpAacConfig(const std::string &config, size_t length, uint8_t *out, size_t out_size);
bool parseAacConfig(const std::string &config, int &samplerate, int &channels);
}

using namespace mediakit;

TEST(AacHelpersTest, ParsesAndDumpsAudioSpecificConfig) {
    const uint8_t adts[] = {0xFF, 0xF1, 0x50, 0x80, 0x01, 0x7F, 0xFC, 0x00, 0x00, 0x00, 0x00};
    const std::string config = makeAacConfig(adts, sizeof(adts));
    EXPECT_FALSE(config.empty());
    int sample_rate = 0;
    int channels = 0;
    EXPECT_TRUE(parseAacConfig(config, sample_rate, channels));
    EXPECT_EQ(44100, sample_rate);
    EXPECT_EQ(2, channels);

    uint8_t output[16] = {0};
    EXPECT_GT(dumpAacConfig(config, 100, output, sizeof(output)), 0);
}

TEST(AacHelpersTest, DetectsAdtsHeaderAndFrameLength) {
    const uint8_t adts[] = {0xFF, 0xF1, 0x50, 0x80, 0x01, 0x7F, 0xFC, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(11, getAacFrameLength(adts, sizeof(adts)));
    EXPECT_EQ(-1, getAacFrameLength(adts, 3));
}

TEST(AudioTracksTest, ExposeExpectedMetadata) {
    AACTrack aac(std::string("\x12\x10", 2));
    EXPECT_TRUE(aac.ready());
    EXPECT_EQ(CodecAAC, aac.getCodecId());
    EXPECT_EQ(44100, aac.getAudioSampleRate());
    EXPECT_EQ(2, aac.getAudioChannel());
    EXPECT_EQ(16, aac.getAudioSampleBit());
    EXPECT_TRUE(aac.getExtraData());

    MP3Track mp3(44100, 2);
    EXPECT_EQ(CodecMP3, mp3.getCodecId());
    EXPECT_EQ(44100, mp3.getAudioSampleRate());

    L16Track l16(16000, 1);
    EXPECT_EQ(CodecL16, l16.getCodecId());
    EXPECT_EQ(16000, l16.getAudioSampleRate());

    OpusTrack opus;
    EXPECT_EQ(CodecOpus, opus.getCodecId());
    EXPECT_EQ(48000, opus.getAudioSampleRate());
    EXPECT_EQ(2, opus.getAudioChannel());

    G711Track g711(CodecG711A, 8000, 1, 16);
    EXPECT_EQ(CodecG711A, g711.getCodecId());
    EXPECT_EQ(8000, g711.getAudioSampleRate());
}
