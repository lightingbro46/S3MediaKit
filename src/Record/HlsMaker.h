#ifndef HLSMAKER_H
#define HLSMAKER_H

#include <string>
#include <deque>
#include <tuple>
#include <cstdint>

namespace mediakit {

class HlsMaker {
public:
    /**
     * @param is_fmp4 Use fmp4 or mpegts
     * @param seg_duration Segment file length
     * @param seg_number Number of segments
     * @param seg_keep Whether to keep the segment file
     */
    HlsMaker(bool is_fmp4 = false, float seg_duration = 5, uint32_t seg_number = 3, bool seg_keep = false);
    virtual ~HlsMaker() = default;

    /**
     * Write ts data
     * @param data Data
     * @param len Data length
     * @param timestamp Millisecond timestamp
     * @param is_idr_fast_packet Whether it is the first packet of the key frame
     */
    void inputData(const char *data, size_t len, uint64_t timestamp, bool is_idr_fast_packet);

    /**
     * Input fmp4 init segment
     * @param data Data
     * @param len Data length
     */
    void inputInitSegment(const char *data, size_t len);

    /**
     * Whether it is live
     */
    bool isLive() const;

    /**
     * Whether to keep the segment file
     */
    bool isKeep() const;

    /**
     * Whether to use fmp4 segmentation or mpegts
     */
    bool isFmp4() const;

    /**
     * Clear records
     */
    void clear();

protected:
    /**
     * Create ts segment file callback
     * @param index
     * @return
     */
    virtual std::string onOpenSegment(uint64_t index) = 0;

    /**
     * Delete ts segment file callback
     * @param index
     */
    virtual void onDelSegment(uint64_t index) = 0;

    /**
     * Write init.mp4 segment file callback
     * @param data
     * @param len
     */
    virtual void onWriteInitSegment(const char *data, size_t len) = 0;

    /**
     * Write ts segment file callback
     * @param data
     * @param len
     */
    virtual void onWriteSegment(const char *data, size_t len) = 0;

    /**
     * Write m3u8 file callback
     */
    virtual void onWriteHls(const std::string &data, bool include_delay) = 0;

    /**
     * The previous ts segment is written, you can notify here
     * @param duration_ms The duration of the previous ts segment, in milliseconds
     */
    virtual void onFlushLastSegment(uint64_t duration_ms) {};

    /**
     * Close the previous ts segment and write the m3u8 index
     * @param eof Whether the HLS live broadcast has ended
     */
    void flushLastSegment(bool eof);

private:
    /**
     * Generate m3u8 file
     * @param eof true represents on-demand
     */
    void makeIndexFile(bool include_delay, bool eof = false);

    /**
     * Delete old ts segments
     */
    void delOldSegment();

    /**
     * Add new ts segments
     * @param timestamp
     */
    void addNewSegment(uint64_t timestamp);

private:
    bool _is_fmp4 = false;
    float _seg_duration = 0;
    uint32_t _seg_number = 0;
    bool _seg_keep = false;
    uint64_t _last_timestamp = 0;
    uint64_t _last_seg_timestamp = 0;
    uint64_t _file_index = 0;
    std::string _last_file_name;
    std::deque<std::tuple<int,std::string> > _seg_dur_list;
};

}//namespace mediakit
#endif //HLSMAKER_H
