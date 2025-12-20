#ifndef S3MEDIAKIT_RTPRECEIVER_H
#define S3MEDIAKIT_RTPRECEIVER_H

#include <map>
#include <string>
#include <memory>
#include "Rtsp/Rtsp.h"
#include "Extension/Frame.h"
// for NtpStamp
#include "Common/Stamp.h"
#include "Util/TimeTicker.h"

namespace mediakit {

template<typename T, typename SEQ = uint16_t>
class PacketSortor {
public:
    static constexpr SEQ SEQ_MAX = (std::numeric_limits<SEQ>::max)();
    using iterator = typename std::map<SEQ, T>::iterator;

    virtual ~PacketSortor() = default;

    void setOnSort(std::function<void(SEQ seq, T packet)> cb) { _cb = std::move(cb); }

    /**
     * Clear the state
     */
    void clear() {
        _started = false;
        _ticker.resetTime();
        _pkt_sort_cache_map.clear();
    }

    /**
     * Get the length of the sorting cache
     */
    size_t getJitterSize() const { return _pkt_sort_cache_map.size(); }

    /**
     * Input and sort
     * @param seq Sequence number
     * @param packet Packet payload
     */
    void sortPacket(SEQ seq, T packet) {
        _latest_seq = seq;
        if (!_started) {
            // Record the first seq
            _started = true;
            _next_seq = seq;
        }
        if (seq == _next_seq) {
            // Receive the next seq
            output(seq, std::move(packet));
            // Clear the continuous packet list
            flushPacket();
            _pkt_drop_cache_map.clear();
            return;
        }

        if (seq < _next_seq && !mayLooped(_next_seq, seq)) {
            // No loop risk, cache seq rollback packets
            _pkt_drop_cache_map.emplace(seq, std::move(packet));
            if (_pkt_drop_cache_map.size() > _max_distance || _ticker.elapsedTime() > _max_buffer_ms) {
                // Too many seq rollback packets, the source may reset the seq counter, this part of data needs to be output
                forceFlush(_next_seq);
                // After clearing the data of the old seq counter, assign the data of the new seq counter to the sorting queue
                _pkt_sort_cache_map = std::move(_pkt_drop_cache_map);
                popIterator(_pkt_sort_cache_map.begin());
            }
            return;
        }
        _pkt_sort_cache_map.emplace(seq, std::move(packet));

        if (needForceFlush(seq)) {
            forceFlush(_next_seq);
        }
    }

    void flush() {
        if (!_pkt_sort_cache_map.empty()) {
            forceFlush(_next_seq);
            _pkt_sort_cache_map.clear();
        }
    }

    void setParams(size_t max_buffer_size, size_t max_buffer_ms, size_t max_distance) {
        _max_buffer_size = max_buffer_size;
        _max_buffer_ms = max_buffer_ms;
        _max_distance = max_distance;
    }

private:
    SEQ distance(SEQ seq) {
        SEQ ret;
        if (seq > _next_seq) {
            ret = seq - _next_seq;
        } else {
            ret = _next_seq - seq;
        }
        if (ret > SEQ_MAX >> 1) {
            return SEQ_MAX - ret;
        }
        return ret;
    }

    bool needForceFlush(SEQ seq) {
        return _pkt_sort_cache_map.size() > _max_buffer_size || distance(seq) > _max_distance || _ticker.elapsedTime() > _max_buffer_ms;
    }

    void forceFlush(SEQ next_seq) {
        if (_pkt_sort_cache_map.empty()) {
            return;
        }
        // Find the nearest seq that is greater than next_seq
        auto it = _pkt_sort_cache_map.lower_bound(next_seq);
        if (it == _pkt_sort_cache_map.end()) {
            // There is no seq greater than next_seq, it should be caused by packet loss during loopback
            it = _pkt_sort_cache_map.begin();
        }
        // Packet loss cannot be recovered, treat this packet as next_seq
        popIterator(it);
        // Clear the continuous packet list
        flushPacket();
        // Delete packets that are too far away from next_seq
        for (auto it = _pkt_sort_cache_map.begin(); it != _pkt_sort_cache_map.end();) {
            if (distance(it->first) > _max_distance) {
                it = _pkt_sort_cache_map.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool mayLooped(SEQ last_seq, SEQ now_seq) { return last_seq > SEQ_MAX - _max_distance || now_seq < _max_distance; }

    void flushPacket() {
        if (_pkt_sort_cache_map.empty()) {
            return;
        }
        auto it = _pkt_sort_cache_map.lower_bound(_next_seq);
        if (!mayLooped(_next_seq, _next_seq)) {
            // No loop risk, clear values less than next_seq
            it = _pkt_sort_cache_map.erase(_pkt_sort_cache_map.begin(), it);
        }

        while (it != _pkt_sort_cache_map.end()) {
            // Find the next packet
            if (it->first == _next_seq) {
                it = popIterator(it);
                continue;
            }
            break;
        }
    }

    iterator popIterator(iterator it) {
        try {
            output(it->first, std::move(it->second));
            return _pkt_sort_cache_map.erase(it);
        } catch (...) {
            // To prevent exceptions from being thrown, the iterator is not removed, causing the rtp package to be empty.
            _pkt_sort_cache_map.erase(it);
            throw;
        }
    }

    void output(SEQ seq, T packet) {
        if (seq != _next_seq) {
            WarnL << "packet dropped: " << _next_seq << " -> " << static_cast<SEQ>(seq - 1)
                  << ", latest seq: " << _latest_seq
                  << ", jitter buffer size: " << _pkt_sort_cache_map.size()
                  << ", jitter buffer ms: " << _ticker.elapsedTime();
        }
        _next_seq = static_cast<SEQ>(seq + 1);
        _cb(seq, std::move(packet));
        _ticker.resetTime();
    }

private:
    bool _started = false;
    // Maximum data length of sorting cache, unit: milliseconds
    size_t _max_buffer_ms = 1000;
    // Maximum number of data in sorting cache
    size_t _max_buffer_size = 1024;
    // Maximum seq jump distance
    size_t _max_distance = 256;
    // Record the time since the last output
    toolkit::Ticker _ticker;
    // The most recently input seq
    SEQ _latest_seq = 0;
    // The next SEQ to be output
    SEQ _next_seq = 0;
    // pkt sorting cache, sorted by seq
    std::map<SEQ, T> _pkt_sort_cache_map;
    // Pre-discard packet list
    std::map<SEQ, T> _pkt_drop_cache_map;
    // Callback
    std::function<void(SEQ seq, T packet)> _cb;
};

class RtpTrack : public PacketSortor<RtpPacket::Ptr> {
public:
    class BadRtpException : public std::invalid_argument {
    public:
        template<typename Type>
        BadRtpException(Type &&type) : invalid_argument(std::forward<Type>(type)) {}
    };

    RtpTrack();

    void clear();
    uint32_t getSSRC() const;
    RtpPacket::Ptr inputRtp(TrackType type, int sample_rate, uint8_t *ptr, size_t len);
    void setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms);
    void setPayloadType(uint8_t pt);

protected:
    virtual void onRtpSorted(RtpPacket::Ptr rtp) {}
    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &rtp) {}

private:
    bool _disable_ntp = false;
    uint8_t _pt = 0xFF;
    uint32_t _ssrc = 0;
    toolkit::Ticker _ssrc_alive;
    NtpStamp _ntp_stamp;
};

class RtpTrackImp : public RtpTrack{
public:
    using OnSorted = std::function<void(RtpPacket::Ptr)>;
    using BeforeSorted = std::function<void(const RtpPacket::Ptr &)>;

    void setOnSorted(OnSorted cb);
    void setBeforeSorted(BeforeSorted cb);

protected:
    void onRtpSorted(RtpPacket::Ptr rtp) override;
    void onBeforeRtpSorted(const RtpPacket::Ptr &rtp) override;

private:
    OnSorted _on_sorted;
    BeforeSorted _on_before_sorted;
};

template<int kCount = 2>
class RtpMultiReceiver {
public:
    RtpMultiReceiver() {
        int index = 0;
        for (auto &track : _track) {
            track.setOnSorted([this, index](RtpPacket::Ptr rtp) {
                onRtpSorted(std::move(rtp), index);
            });
            track.setBeforeSorted([this, index](const RtpPacket::Ptr &rtp) {
                onBeforeRtpSorted(rtp, index);
            });
            ++index;
        }
    }

    virtual ~RtpMultiReceiver() = default;

    /**
     * Generate and sort rtp packets from input data pointer
     * @param index Track index
     * @param type Track type
     * @param samplerate RTP timestamp base clock, 90000 for video, sample rate for audio
     * @param ptr RTP data pointer
     * @param len RTP data pointer length
     * @return Return true if parsing is successful
     */
    bool handleOneRtp(int index, TrackType type, int sample_rate, uint8_t *ptr, size_t len) {
        assert(index < kCount && index >= 0);
        return _track[index].inputRtp(type, sample_rate, ptr, len).operator bool();
    }

    /**
     * Set ntp timestamp, set when receiving rtcp sender report
     * If rtp_stamp/sample_rate/ntp_stamp_ms are all 0, then use rtp timestamp as ntp timestamp
     * @param index Track index
     * @param rtp_stamp RTP timestamp
     * @param ntp_stamp_ms NTP timestamp
     */
    void setNtpStamp(int index, uint32_t rtp_stamp, uint64_t ntp_stamp_ms) {
        assert(index < kCount && index >= 0);
        _track[index].setNtpStamp(rtp_stamp, ntp_stamp_ms);
    }

    void setPayloadType(int index, uint8_t pt){
        assert(index < kCount && index >= 0);
        _track[index].setPayloadType(pt);
    }

    void clear() {
        for (auto &track : _track) {
            track.clear();
        }
    }

    size_t getJitterSize(int index) const {
        assert(index < kCount && index >= 0);
        return _track[index].getJitterSize();
    }

    uint32_t getSSRC(int index) const {
        assert(index < kCount && index >= 0);
        return _track[index].getSSRC();
    }

protected:
    /**
     * Output rtp data packets after sorting
     * @param rtp RTP data packet
     * @param track_index Track index
     */
    virtual void onRtpSorted(RtpPacket::Ptr rtp, int index) {}

    /**
     * RTP data packet parsed but not yet sorted
     * @param rtp RTP data packet
     * @param track_index Track index
     */
    virtual void onBeforeRtpSorted(const RtpPacket::Ptr &rtp, int index) {}

private:
    RtpTrackImp _track[kCount];
};

using RtpReceiver = RtpMultiReceiver<2>;

}//namespace mediakit


#endif //S3MEDIAKIT_RTPRECEIVER_H
