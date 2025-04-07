#ifndef ZLMEDIAKIT_STAMP_H
#define ZLMEDIAKIT_STAMP_H

#include <set>
#include <cstdint>
#include "Util/TimeTicker.h"

namespace mediakit {

class DeltaStamp {
public:
    DeltaStamp();
    virtual ~DeltaStamp() = default;

    /**
     * Calculate the timestamp increment
     * @param stamp Absolute timestamp
     * @param enable_rollback Whether to allow the timestamp to roll back
     * @return Timestamp increment
     */
    int64_t deltaStamp(int64_t stamp, bool enable_rollback = true);
    int64_t relativeStamp(int64_t stamp, bool enable_rollback = true);
    int64_t relativeStamp();

    // Set the maximum allowed rollback or jump amplitude
    void setMaxDelta(size_t max_delta);

    // Reset
    void reset();

protected:
    virtual void needSync() {}

protected:
    int _max_delta;
    int _last_delta = 1;
    int64_t _last_stamp = 0;
    int64_t _relative_stamp = 0;
};

// This class solves the problem of timestamp loopback and rollback
// Calculate the relative timestamp or generate a smooth timestamp
class Stamp : public DeltaStamp{
public:
    /**
     * Get the relative timestamp, which also implements audio and video synchronization, limits dts rollback, etc.
     * @param dts Input dts, if it is 0, it will be generated according to the system timestamp
     * @param pts Input pts, if it is 0, it is equal to dts
     * @param dts_out Output dts
     * @param pts_out Output pts
     * @param modifyStamp Whether to overwrite with the system timestamp
     */
    void revise(int64_t dts, int64_t pts, int64_t &dts_out, int64_t &pts_out,bool modifyStamp = false);

    /**
     * Set the relative timestamp again, used for seek
     * @param relativeStamp Relative timestamp
     */
    void setRelativeStamp(int64_t relativeStamp);

    /**
     * Get the current relative timestamp
     * @return
     */
    int64_t getRelativeStamp() const ;

    /**
     * Set whether it is playback mode, playback mode allows timestamp rollback
     * @param playback Whether it is playback mode
     */
    void setPlayBack(bool playback = true);

    /**
     * Used for audio and video synchronization, audio should be synchronized with video (only modify audio timestamp)
     * Because modifying the audio timestamp does not affect the playback speed
     */
    void syncTo(Stamp &other);

    /**
     * Whether to allow timestamp rollback
     */
    void enableRollback(bool flag);

    /**
     * Reset
     */
    void reset();

private:
    // Mainly implements audio and video timestamp synchronization function
    void revise_l(int64_t dts, int64_t pts, int64_t &dts_out, int64_t &pts_out,bool modifyStamp = false);

    // Mainly implements the function of obtaining the relative timestamp
    void revise_l2(int64_t dts, int64_t pts, int64_t &dts_out, int64_t &pts_out,bool modifyStamp = false);

    void needSync() override;

private:
    bool _playback = false;
    bool _need_sync = false;
    // Default does not allow timestamp rollback
    bool _enable_rollback = false;
    int64_t _relative_stamp = 0;
    int64_t _last_dts_in = 0;
    int64_t _last_dts_out = 0;
    int64_t _last_pts_out = 0;
    toolkit::SmoothTicker _ticker;
    Stamp *_sync_master = nullptr;
};

// dts generator,
// pts after sorting is dts
class DtsGenerator{
public:
    bool getDts(uint64_t pts, uint64_t &dts);

private:
    bool getDts_l(uint64_t pts, uint64_t &dts);

private:
    uint64_t _dts_pts_offset = 0;
    uint64_t _last_dts = 0;
    uint64_t _last_pts = 0;
    uint64_t _last_max_pts = 0;
    size_t _frames_since_last_max_pts = 0;
    size_t _sorter_max_size = 0;
    size_t _count_sorter_max_size = 0;
    std::set<uint64_t> _pts_sorter;
};

class NtpStamp {
public:
    void setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms);
    uint64_t getNtpStamp(uint32_t rtp_stamp, uint32_t sample_rate);

private:
    void update(uint32_t rtp_stamp, uint64_t ntp_stamp_us);
    uint64_t getNtpStampUS(uint32_t rtp_stamp, uint32_t sample_rate);

private:
    uint32_t _last_rtp_stamp = 0;
    uint64_t _last_ntp_stamp_us = 0;
};

}//namespace mediakit

#endif //ZLMEDIAKIT_STAMP_H
