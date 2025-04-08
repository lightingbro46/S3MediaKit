#ifndef SRC_RTMP_RTMPMEDIASOURCE_H_
#define SRC_RTMP_RTMPMEDIASOURCE_H_

#include <mutex>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include "amf.h"
#include "Rtmp.h"
#include "Common/MediaSource.h"
#include "Common/PacketCache.h"
#include "Util/RingBuffer.h"

#define RTMP_GOP_SIZE 512

namespace mediakit {

/**
 * Data abstraction of rtmp media source
 * rtmp has three key elements: metadata, config frame, and ordinary frame
 * Metadata is optional, and some encoding formats do not have config frames (such as MP3)
 * As long as these three elements are generated, it is very simple to implement rtmp push stream and rtmp server
 * In the rtmp push and pull stream protocol, metadata is transmitted first, then config frame, and then ordinary frames are continuously transmitted
 */
class RtmpMediaSource : public MediaSource, public toolkit::RingDelegate<RtmpPacket::Ptr>, private PacketCache<RtmpPacket>{
public:
    using Ptr = std::shared_ptr<RtmpMediaSource>;
    using RingDataType = std::shared_ptr<toolkit::List<RtmpPacket::Ptr> >;
    using RingType = toolkit::RingBuffer<RingDataType>;

    /**
     * Constructor
     * @param vhost Virtual host name
     * @param app Application name
     * @param stream_id Stream id
     * @param ring_size You can set a fixed ring buffer size, 0 is adaptive
     */
    RtmpMediaSource(const MediaTuple& tuple, int ring_size = RTMP_GOP_SIZE): MediaSource(RTMP_SCHEMA, tuple), _ring_size(ring_size) {}

    ~RtmpMediaSource() override {
        try {
            flush();
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }

    /**
     * 	Get the ring buffer of the media source
     */
    const RingType::Ptr &getRing() const {
        return _ring;
    }

    void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                       const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) override {
        _ring->getInfoList(cb, on_change);
    }

    /**
     * Get the number of players
     * @return
     */
    int readerCount() override {
        return _ring ? _ring->readerCount() : 0;
    }

    /**
     * Get metadata
     */
    template <typename FUNC>
    void getMetaData(const FUNC &func) const {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        if (_metadata) {
            func(_metadata);
        }
    }

    /**
     * Get all config frames
     */
    template <typename FUNC>
    void getConfigFrame(const FUNC &func) {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        for (auto &pr : _config_frame_map) {
            func(pr.second);
        }
    }

    /**
     * Set metadata
     */
    virtual void setMetaData(const AMFValue &metadata);

    /**
     * Input rtmp packet
     * @param pkt rtmp packet
     */
    void onWrite(RtmpPacket::Ptr pkt, bool = true) override;

    /**
     * Get the current timestamp
     */
    uint32_t getTimeStamp(TrackType trackType) override;

    void clearCache() override{
        PacketCache<RtmpPacket>::clearCache();
        _ring->clearCache();
    }

    bool haveVideo() const {
        return _have_video;
    }

    bool haveAudio() const {
        return _have_audio;
    }

private:
    /**
     * Trigger this function when batch flushing rtmp packets
     * @param rtmp_list rtmp packet list
     * @param key_pos Whether it contains key frames
    */
    void onFlush(std::shared_ptr<toolkit::List<RtmpPacket::Ptr> > rtmp_list, bool key_pos) override {
        // If there is no video, then there is no point in having a GOP cache, so is_key is always true to ensure that the GOP cache is always cleared
        _ring->write(std::move(rtmp_list), _have_video ? key_pos : true);
    }

private:
    bool _have_video = false;
    bool _have_audio = false;
    int _ring_size;
    uint32_t _track_stamps[TrackMax] = {0};
    AMFValue _metadata;
    RingType::Ptr _ring;

    mutable std::recursive_mutex _mtx;
    std::unordered_map<int, RtmpPacket::Ptr> _config_frame_map;
};

} /* namespace mediakit */

#endif /* SRC_RTMP_RTMPMEDIASOURCE_H_ */
