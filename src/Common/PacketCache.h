#ifndef S3MEDIAKIT_PACKET_CACHE_H_
#define S3MEDIAKIT_PACKET_CACHE_H_

#include "Common/config.h"
#include "Util/List.h"

namespace mediakit {
// / Cache refresh strategy class
class FlushPolicy {
public:
    bool isFlushAble(bool is_video, bool is_key, uint64_t new_stamp, size_t cache_size);

private:
    // Last timestamp of audio and video
    uint64_t _last_stamp[2] = { 0, 0 };
};

// / Merge write cache template
// / \tparam packet Packet type
// / \tparam policy Refresh cache strategy
// / \tparam packet_list Packet cache type
template<typename packet, typename policy = FlushPolicy, typename packet_list = toolkit::List<std::shared_ptr<packet> > >
class PacketCache {
public:
    PacketCache() { _cache = std::make_shared<packet_list>(); }

    virtual ~PacketCache() = default;

    void inputPacket(uint64_t stamp, bool is_video, std::shared_ptr<packet> pkt, bool key_pos) {
        bool flag = flushImmediatelyWhenCloseMerge();
        if (!flag && _policy.isFlushAble(is_video, key_pos, stamp, _cache->size())) {
            flush();
        }

        // Append data to the end
        _cache->emplace_back(std::move(pkt));
        if (key_pos) {
            _key_pos = key_pos;
        }

        if (flag) {
            flush();
        }
    }

    void flush() {
        if (_cache->empty()) {
            return;
        }
        onFlush(std::move(_cache), _key_pos);
        _cache = std::make_shared<packet_list>();
        _key_pos = false;
    }

    virtual void clearCache() {
        _cache->clear();
    }

    virtual void onFlush(std::shared_ptr<packet_list>, bool key_pos) = 0;

private:
    bool flushImmediatelyWhenCloseMerge() {
        // Generally, when the protocol closes the merge write, the cache is refreshed immediately, which can reduce the delay of one frame, but RTP is an exception.
        // Because the RTP packet is very small, and a RtpPacket does not contain a complete frame of image, so when closing the merge write,
        // It is still necessary to buffer one frame of RTP (that is, RTP with the same timestamp) before outputting. Although this will increase the delay of one frame,
        // But it greatly improves performance, so it is still worthwhile to do so.

        GET_CONFIG(int, mergeWriteMS, General::kMergeWriteMS);
        GET_CONFIG(int, rtspLowLatency, Rtsp::kLowLatency);
        return std::is_same<packet, RtpPacket>::value ? rtspLowLatency : (mergeWriteMS <= 0);
    }

private:
    bool _key_pos = false;
    policy _policy;
    std::shared_ptr<packet_list> _cache;
};
}

#endif //S3MEDIAKIT_PACKET_CACHE_H_
