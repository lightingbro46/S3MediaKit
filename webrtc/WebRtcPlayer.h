#ifndef S3MEDIAKIT_WEBRTCPLAYER_H
#define S3MEDIAKIT_WEBRTCPLAYER_H

#include "Rtsp/RtspMediaSource.h"
#include "WebRtcTransport.h"

namespace mediakit {
/**
 * @brief H.264 B frame filter
 * Used to remove B frames from H.264 RTP stream
 */
class H264BFrameFilter {
public:
    /**
     * ISO_IEC_14496-10-AVC-2012
     * Table 7-6 – Name association to slice_type
     */
    enum H264SliceType {
        H264SliceTypeP = 0,
        H264SliceTypeB = 1,
        H264SliceTypeI = 2,
        H264SliceTypeSP = 3,
        H264SliceTypeSI = 4,
        H264SliceTypeP1 = 5,
        H264SliceTypeB1 = 6,
        H264SliceTypeI1 = 7,
        H264SliceTypeSP1 = 8,
        H264SliceTypeSI1 = 9,
    };

    enum H264NALUType {
        NAL_NIDR = 1,
        NAL_PARTITION_A = 2,
        NAL_PARTITION_B = 3,
        NAL_PARTITION_C = 4,
        NAL_IDR = 5,
    };

    H264BFrameFilter();

    ~H264BFrameFilter() = default;

    /**
     * @brief Processing a single RTP packet, removing B-frames
     * @param packet input RTP package
     * @return If it is not a B frame, it returns the original packet, otherwise it returns nullptr
     */
    RtpPacket::Ptr processPacket(const RtpPacket::Ptr &packet);

private:
    /**
     * @brief determines whether the RTP packet contains the B frame of H.264
     * @param packet RTP package
     * @return Return true if it is B frame, otherwise return false
     */
    bool isH264BFrame(const RtpPacket::Ptr &packet) const;

    /**
     * @brief determines whether it is a B-frame based on the NAL type and data
     * @param nal_type NAL unit type
     * @param data NAL unit data (excluding NAL headers)
     * @param size Data size
     * @return Return true if it is B frame, otherwise return false
     */
    bool isBFrameByNalType(uint8_t nal_type, const uint8_t *data, size_t size) const;

    /**
     * @brief parsing index Columbus encoding
     * @param data data buffer
     * @param size Buffer size
     * @param bits_offset bit offset
     * @return parsed value
     */
    int decodeExpGolomb(const uint8_t *data, size_t size, size_t &bitPos) const;

    /**
     * @brief read bits from bitstream
     * @param data data buffer
     * @param size Buffer size
     * @return Read bit value (0 or 1)
     */
    int getBit(const uint8_t *data, size_t size) const;

    /**
     * @brief Extract slice type value
     * @param data data buffer
     * @param size Buffer size
     * @return Slice type value
     */
    uint8_t extractSliceType(const uint8_t *data, size_t size) const;

    /**
     * @brief Processing FU-A sharding
     * @param payload Data buffer
     * @param payload_size buffer size
     * @return Return true if it is B frame, otherwise return false
     */
    bool handleFua(const uint8_t *payload, size_t payload_size) const;

    /**
    * @brief handles STAP-A combination package
    * @param payload Data buffer
    * @param payload_size buffer size
    * @return Return true if it is B frame, otherwise return false
   */
    bool handleStapA(const uint8_t *payload, size_t payload_size) const;


private:
    uint16_t _last_seq; // Maintain the serial number of the output stream
    uint32_t _last_stamp; // Maintain the timestamp of the output stream
    bool _first_packet; // Is it the first package marker
};

class WebRtcPlayer : public WebRtcTransportImp {
public:
    using Ptr = std::shared_ptr<WebRtcPlayer>;
    static Ptr create(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src, const MediaInfo &info);
    MediaInfo getMediaInfo() { return _media_info; }

protected:
    ///////WebRtcTransportImp override///////
    void onStartWebRTC() override;
    void onDestory() override;
    void onRtcConfigure(RtcConfigure &configure) const override;

private:
    WebRtcPlayer(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src, const MediaInfo &info);

    void sendConfigFrames(uint32_t before_seq, uint32_t sample_rate, uint32_t timestamp, uint64_t ntp_timestamp);

private:
    // Media related metadata
    MediaInfo _media_info;
    // Playing rtsp source
    std::weak_ptr<RtspMediaSource> _play_src;

    // In the case of direct RTP forwarding, sps/pps is usually missing. Before forwarding RTP, send the relevant frame information once. In some cases, it can be played.
    bool _send_config_frames_once { false };

    // Reader object for playing rtsp source
    RtspMediaSource::RingType::RingReader::Ptr _reader;

    bool _is_h264 { false };
    bool _bfliter_flag { false };
    std::shared_ptr<H264BFrameFilter> _bfilter;
};

}// namespace mediakit
#endif // S3MEDIAKIT_WEBRTCPLAYER_H
