#ifndef S3MEDIAKIT_WEBMMEDIASOURCE_H
#define S3MEDIAKIT_WEBMMEDIASOURCE_H

#include "Common/MediaSource.h"
#include "Common/PacketCache.h"
#include "Util/RingBuffer.h"

#define WEBM_GOP_SIZE 512

namespace mediakit {

// WebM Live Data Packet
class WebMPacket : public toolkit::BufferString {
public:
    using Ptr = std::shared_ptr<WebMPacket>;

    template<typename ...ARGS>
    WebMPacket(ARGS && ...args) : toolkit::BufferString(std::forward<ARGS>(args)...) {};

public:
    uint64_t time_stamp = 0;
};

// WebM Live Source
class WebMMediaSource final 
    : public MediaSource
    , public toolkit::RingDelegate<WebMPacket::Ptr>
    , private PacketCache<WebMPacket> {
public:
    using Ptr = std::shared_ptr<WebMMediaSource>;
    using RingDataType = std::shared_ptr<toolkit::List<WebMPacket::Ptr> >;
    using RingType = toolkit::RingBuffer<RingDataType>;

    WebMMediaSource(const MediaTuple& tuple, int ring_size = WEBM_GOP_SIZE) : MediaSource(WEBM_SCHEMA, tuple), _ring_size(ring_size) {}

    ~WebMMediaSource() override {
        try {
            flush();
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }

    /**
     * Get the circular buffer of the media source
     */
    const RingType::Ptr &getRing() const {
        return _ring;
    }

    void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                       const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) override {
        _ring->getInfoList(cb, on_change);
    }

    //todo:


    /**
     * Get the number of players
     */
    int readerCount() override {
        return _ring ? _ring->readerCount() : 0;
    }

    /**
     * Input WebM packet
     * @param packet WebM packet
     * @param key Whether it is the first packet of the key frame
     */
    void onWrite(WebMPacket::Ptr packet, bool key) override {
        if (!_ring) {
            createRing();
        }
        if (key) {
            _have_video = true;
        }
        _speed[TrackVideo] += packet->size();
        auto stamp = packet->time_stamp;
        PacketCache<WebMPacket>::inputPacket(stamp, true, std::move(packet), key);
    }

    /**
     * Clear GOP cache
     */
    void clearCache() override {
        PacketCache<WebMPacket>::clearCache();
        _ring->clearCache();
    }

private:
    void createRing(){
        std::weak_ptr<WebMMediaSource> weak_self = std::static_pointer_cast<WebMMediaSource>(shared_from_this());
        _ring = std::make_shared<RingType>(_ring_size, [weak_self](int size) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->onReaderChanged(size);
        });
        regist();
    }

    /**
     * Merge write callback
     * @param packet_list Merge write cache queue
     * @param key_pos Whether it contains a key frame
     */
    void onFlush(std::shared_ptr<toolkit::List<WebMPacket::Ptr> > packet_list, bool key_pos) override {
        // If there is no video, then there is no meaning to the existence of GOP cache, so make sure to clear the GOP cache all the time
        _ring->write(std::move(packet_list), _have_video ? key_pos : true);
    }

private:
    bool _have_video = false;
    int _ring_size;
    std::string _init_segment;
    RingType::Ptr _ring;
};

} // namespace mediakit

#endif // S3MEDIAKIT_WEBMMEDIASOURCE_H