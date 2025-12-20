#ifndef SRC_RTSP_RTSPMEDIASOURCE_H_
#define SRC_RTSP_RTSPMEDIASOURCE_H_

#include <mutex>
#include <string>
#include <memory>
#include <functional>
#include "Common/MediaSource.h"
#include "Common/PacketCache.h"
#include "Util/RingBuffer.h"

#define RTP_GOP_SIZE 512

namespace mediakit {

/**
 * Data abstraction of rtsp media source
 * Rtsp has two key elements, sdp and rtp packets
 * As long as these two elements are generated, it is very simple to implement rtsp push stream and rtsp server
 * In the rtsp push and pull stream protocol, sdp is transmitted first, then the transmission method (tcp/udp/multicast) is negotiated, and finally rtp is continuously transmitted
 */
class RtspMediaSource : public MediaSource, public toolkit::RingDelegate<RtpPacket::Ptr>, private PacketCache<RtpPacket> {
public:
    using Ptr = std::shared_ptr<RtspMediaSource>;
    using RingDataType = std::shared_ptr<toolkit::List<RtpPacket::Ptr> >;
    using RingType = toolkit::RingBuffer<RingDataType>;

    /**
     * Constructor
     * @param vhost Virtual host name
     * @param app Application name
     * @param stream_id Stream id
     * @param ring_size You can set a fixed ring buffer size, 0 is adaptive
     */
    RtspMediaSource(const MediaTuple& tuple, int ring_size = RTP_GOP_SIZE): MediaSource(RTSP_SCHEMA, tuple), _ring_size(ring_size) {}

    ~RtspMediaSource() override {
        try {
            flush();
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }

    /**
     * Get the ring buffer of the media source
     */
    const RingType::Ptr &getRing() const {
        return _ring;
    }

    void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                       const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) override {
        assert(_ring);
        _ring->getInfoList(cb, on_change);
    }

    bool broadcastMessage(const toolkit::Any &data) override {
        assert(_ring);
        _ring->sendMessage(data);
        return true;
    }

    /**
     * Get the number of players
     */
    int readerCount() override {
        return _ring ? _ring->readerCount() : 0;
    }

    /**
     * Get the sdp of this source
     */
    const std::string &getSdp() const {
        return _sdp;
    }

    virtual RtspMediaSource::Ptr clone(const std::string& stream) {
        return nullptr;
    }

    /**
     * Get the ssrc of the corresponding track
     */
    virtual uint32_t getSsrc(TrackType trackType) {
        assert(trackType >= 0 && trackType < TrackMax);
        auto &track = _tracks[trackType];
        if (!track) {
            return 0;
        }
        return track->_ssrc;
    }

    /**
     * Get the sequence of the corresponding track
     */
    virtual uint16_t getSequence(TrackType trackType) {
        assert(trackType >= 0 && trackType < TrackMax);
        auto &track = _tracks[trackType];
        if (!track) {
            return 0;
        }
        return track->_seq;
    }

    /**
     * Get the timestamp of the corresponding track, in milliseconds
     */
    uint32_t getTimeStamp(TrackType trackType) override;

    /**
     * Update timestamp
     */
    void setTimeStamp(uint32_t stamp) override;

    /**
     * Set sdp
     */
    virtual void setSdp(const std::string &sdp);

    /**
     * Input rtp
     * @param rtp rtp packet
     * @param keyPos Whether this packet is the first packet of a key frame
     */
    void onWrite(RtpPacket::Ptr rtp, bool keyPos) override;

    void clearCache() override{
        PacketCache<RtpPacket>::clearCache();
        _ring->clearCache();
    }

private:
    /**
     * Trigger this function when flushing rtp packets in batches
     * @param rtp_list rtp packet list
     * @param key_pos Whether it contains a key frame
     */
    void onFlush(std::shared_ptr<toolkit::List<RtpPacket::Ptr> > rtp_list, bool key_pos) override {
        // If there is no video, then there is no point in having a GOP cache, so is_key is always true to ensure that the GOP cache is always cleared
        _ring->write(std::move(rtp_list), _have_video ? key_pos : true);
    }

private:
    bool _have_video = false;
    int _ring_size;
    std::string _sdp;
    RingType::Ptr _ring;
    SdpTrack::Ptr _tracks[TrackMax];
};

} /* namespace mediakit */

#endif /* SRC_RTSP_RTSPMEDIASOURCE_H_ */
