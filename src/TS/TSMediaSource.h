#ifndef S3MEDIAKIT_TSMEDIASOURCE_H
#define S3MEDIAKIT_TSMEDIASOURCE_H

#include "Common/MediaSource.h"
#include "Common/PacketCache.h"
#include "Util/RingBuffer.h"

#define TS_GOP_SIZE 512

namespace mediakit {

// TS Live Data Packet
class TSPacket : public toolkit::BufferOffset<toolkit::Buffer::Ptr>{
public:
    using Ptr = std::shared_ptr<TSPacket>;

    template<typename ...ARGS>
    TSPacket(ARGS && ...args) : BufferOffset<Buffer::Ptr>(std::forward<ARGS>(args)...) {};

public:
    uint64_t time_stamp = 0;
};

// TS Live Source
class TSMediaSource final : public MediaSource, public toolkit::RingDelegate<TSPacket::Ptr>, private PacketCache<TSPacket>{
public:
    using Ptr = std::shared_ptr<TSMediaSource>;
    using RingDataType = std::shared_ptr<toolkit::List<TSPacket::Ptr> >;
    using RingType = toolkit::RingBuffer<RingDataType>;

    TSMediaSource(const MediaTuple& tuple, int ring_size = TS_GOP_SIZE): MediaSource(TS_SCHEMA, tuple), _ring_size(ring_size) {}

    ~TSMediaSource() override {
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

    /**
     * Get the number of players
     */
    int readerCount() override {
        return _ring ? _ring->readerCount() : 0;
    }

    /**
     * Input TS packet
     * @param packet TS packet
     * @param key Whether it is the first packet of the key frame
     */
    void onWrite(TSPacket::Ptr packet, bool key) override {
        _speed[TrackVideo] += packet->size();
        if (!_ring) {
            createRing();
        }
        if (key) {
            _have_video = true;
        }
        auto stamp = packet->time_stamp;
        PacketCache<TSPacket>::inputPacket(stamp, true, std::move(packet), key);
    }

    /**
     * Clear GOP cache
     */
    void clearCache() override {
        PacketCache<TSPacket>::clearCache();
        _ring->clearCache();
    }

private:
    void createRing(){
        std::weak_ptr<TSMediaSource> weak_self = std::static_pointer_cast<TSMediaSource>(shared_from_this());
        _ring = std::make_shared<RingType>(_ring_size, [weak_self](int size) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->onReaderChanged(size);
        });
        // Register media source
        regist();
    }

    /**
     * Merge write callback
     * @param packet_list Merge write cache queue
     * @param key_pos Whether it contains a key frame
     */
    void onFlush(std::shared_ptr<toolkit::List<TSPacket::Ptr> > packet_list, bool key_pos) override {
        // If there is no video, then there is no meaning to the existence of GOP cache, so make sure to clear the GOP cache all the time
        _ring->write(std::move(packet_list), _have_video ? key_pos : true);
    }

private:
    bool _have_video = false;
    int _ring_size;
    RingType::Ptr _ring;
};


}//namespace mediakit
#endif //S3MEDIAKIT_TSMEDIASOURCE_H
