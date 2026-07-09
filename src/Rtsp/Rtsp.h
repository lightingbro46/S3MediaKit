#ifndef RTSP_RTSP_H_
#define RTSP_RTSP_H_

#include <string.h>
#include <string>
#include <memory>
#include <unordered_map>
#include "Network/Socket.h"
#include "Common/macros.h"
#include "Extension/Frame.h"

namespace mediakit {

class Track;

namespace Rtsp {
typedef enum {
    RTP_Invalid = -1,
    RTP_TCP = 0,
    RTP_UDP = 1,
    RTP_MULTICAST = 2,
} eRtpType;

typedef enum {
    Live = 0,
    Replay = 1
} eRtpMode;

#define RTP_PT_MAP(XX)                                                                                                                                         \
    XX(PCMU, TrackAudio, 0, 8000, 1, CodecG711U)                                                                                                               \
    XX(GSM, TrackAudio, 3, 8000, 1, CodecInvalid)                                                                                                              \
    XX(G723, TrackAudio, 4, 8000, 1, CodecG723)                                                                                                             \
    XX(DVI4_8000, TrackAudio, 5, 8000, 1, CodecInvalid)                                                                                                        \
    XX(DVI4_16000, TrackAudio, 6, 16000, 1, CodecInvalid)                                                                                                      \
    XX(LPC, TrackAudio, 7, 8000, 1, CodecInvalid)                                                                                                              \
    XX(PCMA, TrackAudio, 8, 8000, 1, CodecG711A)                                                                                                               \
    XX(G722, TrackAudio, 9, 16000, 1, CodecG722)                                                                                                             \
    XX(L16_Stereo, TrackAudio, 10, 44100, 2, CodecInvalid)                                                                                                     \
    XX(L16_Mono, TrackAudio, 11, 44100, 1, CodecInvalid)                                                                                                       \
    XX(QCELP, TrackAudio, 12, 8000, 1, CodecInvalid)                                                                                                           \
    XX(CN, TrackAudio, 13, 8000, 1, CodecInvalid)                                                                                                              \
    XX(MP3, TrackAudio, 14, 44100, 2, CodecMP3)                                                                                                            \
    XX(G728, TrackAudio, 15, 8000, 1, CodecG728)                                                                                                            \
    XX(DVI4_11025, TrackAudio, 16, 11025, 1, CodecInvalid)                                                                                                     \
    XX(DVI4_22050, TrackAudio, 17, 22050, 1, CodecInvalid)                                                                                                     \
    XX(G729, TrackAudio, 18, 8000, 1, CodecG729)                                                                                                            \
    XX(CelB, TrackVideo, 25, 90000, 1, CodecInvalid)                                                                                                           \
    XX(JPEG, TrackVideo, 26, 90000, 1, CodecJPEG)                                                                                                              \
    XX(nv, TrackVideo, 28, 90000, 1, CodecInvalid)                                                                                                             \
    XX(H261, TrackVideo, 31, 90000, 1, CodecInvalid)                                                                                                           \
    XX(MPV, TrackVideo, 32, 90000, 1, CodecInvalid)                                                                                                            \
    XX(MP2T, TrackVideo, 33, 90000, 1, CodecTS)                                                                                                           \
    XX(H263, TrackVideo, 34, 90000, 1, CodecInvalid)

typedef enum {
#define ENUM_DEF(name, type, value, clock_rate, channel, codec_id) PT_##name = value,
    RTP_PT_MAP(ENUM_DEF)
#undef ENUM_DEF
        PT_MAX
    = 128
} PayloadType;

}; // namespace Rtsp

#pragma pack(push, 1)

class RtpHeader {
public:
#if __BYTE_ORDER == __BIG_ENDIAN
    // Version number, fixed to 2
    uint32_t version : 2;
    // padding
    uint32_t padding : 1;
    // Extension
    uint32_t ext : 1;
    // csrc
    uint32_t csrc : 4;
    // mark
    uint32_t mark : 1;
    // Payload type
    uint32_t pt : 7;
#else
    // csrc
    uint32_t csrc : 4;
    // Extension
    uint32_t ext : 1;
    // padding
    uint32_t padding : 1;
    // Version number, fixed to 2
    uint32_t version : 2;
    // Payload type
    uint32_t pt : 7;
    // mark
    uint32_t mark : 1;
#endif
    // Sequence number
    uint32_t seq : 16;
    // Timestamp
    uint32_t stamp;
    // ssrc
    uint32_t ssrc;
    // Payload, if csrc and ext exist, the front is 4 * csrc + (4 + 4 * ext_len)
    uint8_t payload;

public:
    // Return the byte length of the csrc field
    size_t getCsrcSize() const;
    // Return the starting address of the csrc field, return nullptr if it does not exist
    uint8_t *getCsrcData();

    // Return the byte length of the ext field
    size_t getExtSize() const;
    // Return the ext reserved value
    uint16_t getExtReserved() const;
    // Return the starting address of the ext segment, return nullptr if it does not exist
    uint8_t *getExtData();

    // Return the valid payload pointer, skip csrc, ext
    uint8_t *getPayloadData();
    // Return the total length of the valid payload, excluding csrc, ext, padding
    ssize_t getPayloadSize(size_t rtp_size) const;
    // Print debug information
    std::string dumpString(size_t rtp_size) const;

private:
    // Return the valid payload offset
    size_t getPayloadOffset() const;
    // Return the padding length
    size_t getPaddingSize(size_t rtp_size) const;
};

#pragma pack(pop)

// This rtp is in the form of rtp over tcp, the first 4 bytes need to be ignored
class RtpPacket : public toolkit::BufferRaw {
public:
    using Ptr = std::shared_ptr<RtpPacket>;
    enum { kRtpVersion = 2, kRtpHeaderSize = 12, kRtpTcpHeaderSize = 4 };

    // Get the rtp header
    RtpHeader *getHeader();
    const RtpHeader *getHeader() const;

    // Print debug information
    std::string dumpString() const;

    // Host byte order seq
    uint16_t getSeq() const;
    uint32_t getStamp() const;
    // Host byte order timestamp, converted to milliseconds
    uint64_t getStampMS(bool ntp = true) const;
    // Host byte order ssrc
    uint32_t getSSRC() const;
    // Valid payload, skip csrc, ext
    uint8_t *getPayload();
    // Valid payload length, excluding csrc, ext, padding
    size_t getPayloadSize() const;

    // Audio and video type
    TrackType type;
    // Audio is the sampling rate, video is generally 90000
    uint32_t sample_rate;
    // ntp timestamp
    uint64_t ntp_stamp;

    int track_index;

    static Ptr create();

private:
    friend class toolkit::ResourcePool_l<RtpPacket>;
    RtpPacket() = default;

private:
    // Object Count Statistics
    toolkit::ObjectStatistic<RtpPacket> _statistic;
};

class RtpPayload {
public:
    static int getClockRate(int pt);
    static int getClockRateByCodec(CodecId codec);
    static TrackType getTrackType(int pt);
    static int getAudioChannel(int pt);
    static const char *getName(int pt);
    static CodecId getCodecId(int pt);
    static int getPayloadType(const Track &track);

private:
    RtpPayload() = delete;
    ~RtpPayload() = delete;
};

class SdpTrack {
public:
    using Ptr = std::shared_ptr<SdpTrack>;
    std::string _t;
    std::string _b;
    uint16_t _port;

    float _duration = 0;
    float _start = 0;
    float _end = 0;

    std::map<char, std::string> _other;
    std::multimap<std::string, std::string> _attr;

    std::string toString(uint16_t port = 0) const;
    std::string getName() const;
    std::string getControlUrl(const std::string &base_url) const;

public:
    int _pt = 0xff;
    int _channel = 0;
    int _samplerate = 0;
    TrackType _type;
    std::string _codec;
    std::string _fmtp;
    std::string _control;

public:
    bool _inited = false;
    uint8_t _interleaved = 0;
    uint16_t _seq = 0;
    uint32_t _ssrc = 0;
    // Timestamp, unit: milliseconds
    uint32_t _time_stamp = 0;
};

class SdpParser {
public:
    using Ptr = std::shared_ptr<SdpParser>;

    SdpParser() = default;
    SdpParser(const std::string &sdp) { load(sdp); }

    void load(const std::string &sdp);
    bool available() const;
    SdpTrack::Ptr getTrack(TrackType type) const;
    std::vector<SdpTrack::Ptr> getAvailableTrack() const;
    std::string toString() const;
    std::string getControlUrl(const std::string &url) const;

private:
    std::vector<SdpTrack::Ptr> _track_vec;
};

/**
 * rtsp sdp base class
 */
class Sdp {
public:
    using Ptr = std::shared_ptr<Sdp>;

    /**
     * Construct sdp
     * @param sample_rate Sampling rate
     * @param payload_type pt type
     */
    Sdp(uint32_t sample_rate, uint8_t payload_type) {
        _sample_rate = sample_rate;
        _payload_type = payload_type;
    }

    virtual ~Sdp() = default;

    /**
     * Get sdp string
     * @return
     */
    virtual std::string getSdp() const = 0;

    /**
     * Get pt
     * @return
     */
    uint8_t getPayloadType() const { return _payload_type; }

    /**
     * Get sampling rate
     * @return
     */
    uint32_t getSampleRate() const { return _sample_rate; }

private:
    uint8_t _payload_type;
    uint32_t _sample_rate;
};

class DefaultSdp : public Sdp {
public:
    DefaultSdp(int payload_type, const Track &track);
    std::string getSdp() const override { return _printer; }

private:
    toolkit::_StrPrinter _printer;
};

/**
 * Other description part in sdp except audio and video
 */
class TitleSdp : public Sdp {
public:
    using Ptr = std::shared_ptr<TitleSdp>;
    /**
     * Construct title type sdp
     * @param dur_sec rtsp on-demand duration, 0 represents live broadcast, unit: seconds
     * @param header Custom sdp description
     * @param version sdp version
     */
    TitleSdp(float dur_sec = 0, const std::map<std::string, std::string> &header = std::map<std::string, std::string>(), int version = 0);

    std::string getSdp() const override { return _printer; }

    float getDuration() const { return _dur_sec; }

private:
    float _dur_sec = 0;
    toolkit::_StrPrinter _printer;
};

// Create 4-byte header for rtp over tcp
toolkit::Buffer::Ptr makeRtpOverTcpPrefix(uint16_t size, uint8_t interleaved);
// Create rtp-rtcp port pair
void makeSockPair(std::pair<toolkit::Socket::Ptr, toolkit::Socket::Ptr> &pair, const std::string &local_ip, bool re_use_port = false, bool is_udp = true);
// Print ssrc in hexadecimal format
std::string printSSRC(uint32_t ui32Ssrc);
bool getSSRC(const char *data, size_t data_len, uint32_t &ssrc);

bool isRtp(const char *buf, size_t size);
bool isRtcp(const char *buf, size_t size);

} // namespace mediakit
#endif // RTSP_RTSP_H_
