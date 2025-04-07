#include "Stamp.h"

// Timestamp maximum allowable jump is 3 seconds, mainly to prevent network jitter caused by the jump
#define MAX_DELTA_STAMP (3 * 1000)
#define STAMP_LOOP_DELTA (60 * 1000)
#define MAX_CTS 500
#define ABS(x) ((x) > 0 ? (x) : (-x))

using namespace toolkit;

namespace mediakit {

DeltaStamp::DeltaStamp() {
    // Timestamp maximum allowable jump is 300ms
    _max_delta = 300;
}

void DeltaStamp::reset() {
    _last_stamp = 0;
    _relative_stamp = 0;
    _last_delta = 1;
}

int64_t DeltaStamp::relativeStamp(int64_t stamp, bool enable_rollback) {
    _relative_stamp += deltaStamp(stamp, enable_rollback);
    return _relative_stamp;
}

int64_t DeltaStamp::relativeStamp() {
    return _relative_stamp;
}

int64_t DeltaStamp::deltaStamp(int64_t stamp, bool enable_rollback) {
    if (!_last_stamp) {
        // Calculate the timestamp increment for the first time, the timestamp increment is 0
        if (stamp) {
            _last_stamp = stamp;
        }
        return 0;
    }

    int64_t ret = stamp - _last_stamp;
    if (ret >= 0) {
        // The timestamp increment is positive, return it
        _last_stamp = stamp;
        // In the live broadcast case, the timestamp increment must not be greater than MAX_DELTA_STAMP, otherwise the relative timestamp is forced to add 1
        if (ret > _max_delta) {
            needSync();
            return _last_delta;
        }
        _last_delta = ret;
        return ret;
    }

    // The timestamp increment is negative, indicating that the timestamp has looped or retreated
    _last_stamp = stamp;
    if (!enable_rollback || -ret > _max_delta) {
        // Not allowed to retreat or retreat too much, force the timestamp to add 1
        needSync();
        return _last_delta;
    }
    return ret;
}

void DeltaStamp::setMaxDelta(size_t max_delta) {
    _max_delta = max_delta;
}

void Stamp::setPlayBack(bool playback) {
    _playback = playback;
}

void Stamp::syncTo(Stamp &other) {
    _need_sync = true;
    _sync_master = &other;
}

void Stamp::needSync() {
    _need_sync = true;
}

void Stamp::enableRollback(bool flag) {
    _enable_rollback = flag;
}

void Stamp::reset() {
    DeltaStamp::reset();
    _relative_stamp = 0;
    _last_dts_in = 0;
    _last_dts_out = 0;
    _last_pts_out = 0;
}

// Limit dts retreat
void Stamp::revise(int64_t dts, int64_t pts, int64_t &dts_out, int64_t &pts_out, bool modifyStamp) {
    revise_l(dts, pts, dts_out, pts_out, modifyStamp);
    if (_playback) {
        // Playback allows timestamp rollback
        return;
    }

    if (dts_out < _last_dts_out) {
        // WarnL << "dts rollback:" << dts_out << " < " << _last_dts_out;
        dts_out = _last_dts_out;
        pts_out = _last_pts_out;
        return;
    }
    _last_dts_out = dts_out;
    _last_pts_out = pts_out;
}

// Audio and video timestamp synchronization
void Stamp::revise_l(int64_t dts, int64_t pts, int64_t &dts_out, int64_t &pts_out, bool modifyStamp) {
    revise_l2(dts, pts, dts_out, pts_out, modifyStamp);
    if (!_sync_master || modifyStamp || _playback) {
        // Automatically generate timestamps or playback or synchronization is complete
        return;
    }

    // Need to synchronize timestamps
    if (_sync_master && _sync_master->_last_dts_in && (_need_sync || _sync_master->_need_sync)) {
        // Audio and video dts current time difference
        int64_t dts_diff = _last_dts_in - _sync_master->_last_dts_in;
        if (ABS(dts_diff) < 5000) {
            // If the absolute timestamp is less than 5 seconds, then it means that their starting timestamps are consistent, then force synchronization
            auto target_stamp = _sync_master->_relative_stamp + dts_diff;
            if (target_stamp > _relative_stamp || _enable_rollback) {
                // After forced synchronization, the timestamp increases jump, or allows rollback
                TraceL << "Relative stamp changed: " << _relative_stamp << " -> " << target_stamp;
                _relative_stamp = target_stamp;
            } else {
                // Not allowed to rollback, then let the timestamp of the other Track increase
                target_stamp = _relative_stamp - dts_diff;
                TraceL << "Relative stamp changed: " << _sync_master->_relative_stamp << " -> " << target_stamp;
                _sync_master->_relative_stamp = target_stamp;
            }
        }
        _need_sync = false;
        _sync_master->_need_sync = false;
    }
}

// Obtain the relative timestamp
void Stamp::revise_l2(int64_t dts, int64_t pts, int64_t &dts_out, int64_t &pts_out, bool modifyStamp) {
    if (!pts) {
        // There is no playback timestamp, set it to the decoding timestamp
        pts = dts;
    }

    if (_playback) {
        // This is on-demand
        dts_out = dts;
        pts_out = pts;
        _relative_stamp = dts_out;
        _last_dts_in = dts;
        return;
    }

    // The difference between pts and dts
    int64_t pts_dts_diff = pts - dts;

    if (_last_dts_in != dts) {
        // Timestamp changed
        if (modifyStamp) {
            // Internal production of timestamps
            _relative_stamp = _ticker.elapsedTime();
        } else {
            _relative_stamp += deltaStamp(dts, _enable_rollback);
        }
        _last_dts_in = dts;
    }
    dts_out = _relative_stamp;

    // ////////////The following is the calculation of the playback timestamp//////////////////
    if (ABS(pts_dts_diff) > MAX_CTS) {
        // If the difference is too large, it is considered that the timestamp is messed up due to looping
        pts_dts_diff = 0;
    }

    pts_out = dts_out + pts_dts_diff;
}

void Stamp::setRelativeStamp(int64_t relativeStamp) {
    _relative_stamp = relativeStamp;
}

int64_t Stamp::getRelativeStamp() const {
    return _relative_stamp;
}

bool DtsGenerator::getDts(uint64_t pts, uint64_t &dts) {
    bool ret = false;
    if (pts == _last_pts) {
        // pts does not change, indicating that dts will not change, return the last dts
        if (_last_dts) {
            dts = _last_dts;
            ret = true;
        }
    } else {
        // pts changed, try to calculate dts
        ret = getDts_l(pts, dts);
        if (ret) {
            // Get the dts, save the current result
            _last_dts = dts;
        }
    }

    if (!ret) {
        // The pts sorting queue length is not yet known, that is, it is not known whether there is a B frame,
        // Then force dts == pts first, which may cause the starting picture to have a few frames rollback in the case of B frames
        dts = pts;
    }

    // Record the last pts
    _last_pts = pts;
    return ret;
}

// The core idea of this algorithm is to sort the pts, and the sorted pts is the dts.
// Sorting has a certain lag, so it is necessary to add the timestamp offset caused by sorting
bool DtsGenerator::getDts_l(uint64_t pts, uint64_t &dts) {
    if (_sorter_max_size == 1) {
        // There is no B frame, dts is equal to pts
        dts = pts;
        return true;
    }

    if (!_sorter_max_size) {
        // The length of the pts sorting queue (that is, the number of B frames between P frames) has not been calculated yet
        if (pts > _last_max_pts) {
            // The pts timestamp has increased, which means that this frame is not a B frame (it means it is a P frame or a key frame)
            if (_frames_since_last_max_pts && _count_sorter_max_size++ > 0) {
                // There have been multiple non-B frames, so we can know the number of B frames between P frames
                _sorter_max_size = _frames_since_last_max_pts;
                // We record the time interval between P frames (that is, the cumulative increment of multiple B frame timestamps)
                _dts_pts_offset = (pts - _last_max_pts);
                // Divide by 2 to prevent dts from being greater than pts
                _dts_pts_offset /= 2;
            }
            // When encountering a P frame or a key frame, the continuous B frame count is cleared
            _frames_since_last_max_pts = 0;
            // Record the pts timestamp of the last non-B frame (which is also dts), used to count the continuous B frame timestamp increment
            _last_max_pts = pts;
        }
        // If the pts timestamp is less than the previous P frame, then it is determined that this is a B frame, and we record the number of consecutive B frames
        ++_frames_since_last_max_pts;
    }

    // Put pts into the sorting cache queue, the maximum cache queue is equal to the number of consecutive B frames
    _pts_sorter.emplace(pts);

    if (_sorter_max_size && _pts_sorter.size() > _sorter_max_size) {
        // If pts sorting is enabled (meaning there are B frames), and the length of the pts sorting cache queue is greater than the number of consecutive B frames,
        // It means that the subsequent pts will be larger than the earliest pts, which means that the earliest pts can be taken out, and this pts will be used as the dts baseline for this frame
        auto it = _pts_sorter.begin();

        // Since this pts is the pts of the previous _sorter_max_size frames (that is, the dts of that frame),
        // Then we add the timestamp offset, which is basically equal to the dts of this frame
        dts = *it + _dts_pts_offset;
        if (dts > pts) {
            // dts cannot be greater than pts (it is basically impossible to reach this logic)
            dts = pts;
        }

        // pts sorting cache dequeue
        _pts_sorter.erase(it);
        return true;
    }

    // The sorting cache is not full yet
    return false;
}

void NtpStamp::setNtpStamp(uint32_t rtp_stamp, uint64_t ntp_stamp_ms) {
    if (!ntp_stamp_ms || !rtp_stamp) {
        // It has been found that some rtsp servers send rtp timestamps and ntp timestamps that are always 0
        WarnL << "Invalid sender report rtcp, ntp_stamp_ms = " << ntp_stamp_ms << ", rtp_stamp = " << rtp_stamp;
        return;
    }
    update(rtp_stamp, ntp_stamp_ms * 1000);
}

void NtpStamp::update(uint32_t rtp_stamp, uint64_t ntp_stamp_us) {
    _last_rtp_stamp = rtp_stamp;
    _last_ntp_stamp_us = ntp_stamp_us;
}

uint64_t NtpStamp::getNtpStamp(uint32_t rtp_stamp, uint32_t sample_rate) {
    if (rtp_stamp == _last_rtp_stamp) {
        return _last_ntp_stamp_us / 1000;
    }
    return getNtpStampUS(rtp_stamp, sample_rate) / 1000;
}

uint64_t NtpStamp::getNtpStampUS(uint32_t rtp_stamp, uint32_t sample_rate) {
    if (!_last_ntp_stamp_us) {
        // The sender report rtcp packet has not been received yet, so assign it to the local system timestamp
        update(rtp_stamp, getCurrentMicrosecond(true));
    }

    // The rtp timestamp is increasing
    if (rtp_stamp >= _last_rtp_stamp) {
        auto diff_us = static_cast<int64_t>((rtp_stamp - _last_rtp_stamp) / (sample_rate / 1000000.0f));
        if (diff_us < MAX_DELTA_STAMP * 1000) {
            // The timestamp is increasing normally
            update(rtp_stamp, _last_ntp_stamp_us + diff_us);
            return _last_ntp_stamp_us;
        }

        // The timestamp jumps significantly
        uint64_t loop_delta_hz = STAMP_LOOP_DELTA * sample_rate / 1000;
        if (_last_rtp_stamp < loop_delta_hz && rtp_stamp > UINT32_MAX - loop_delta_hz) {
            // It should be rtp timestamp overflow + out of order
            uint64_t max_rtp_us = uint64_t(UINT32_MAX) * 1000000 / sample_rate;
            return _last_ntp_stamp_us + diff_us - max_rtp_us;
        }
        // The timestamp jumps significantly for unknown reasons, directly return the last value
        WarnL << "rtp stamp abnormal increased:" << _last_rtp_stamp << " -> " << rtp_stamp;
        update(rtp_stamp, _last_ntp_stamp_us);
        return _last_ntp_stamp_us;
    }

    // The rtp timestamp is decreasing
    auto diff_us = static_cast<int64_t>((_last_rtp_stamp - rtp_stamp) / (sample_rate / 1000000.0f));
    if (diff_us < MAX_DELTA_STAMP * 1000) {
        // The timestamp retreats within the normal range, indicating that the rtp is out of order
        return _last_ntp_stamp_us - diff_us;
    }

    // The timestamp retreats significantly
    uint64_t loop_delta_hz = STAMP_LOOP_DELTA * sample_rate / 1000;
    if (rtp_stamp < loop_delta_hz && _last_rtp_stamp > UINT32_MAX - loop_delta_hz) {
        // Determine if it is a timestamp overflow
        uint64_t max_rtp_us = uint64_t(UINT32_MAX) * 1000000 / sample_rate;
        update(rtp_stamp, _last_ntp_stamp_us + (max_rtp_us - diff_us));
        return _last_ntp_stamp_us;
    }
    // Timestamp rollback for unknown reasons, return the last value directly
    WarnL << "rtp stamp abnormal reduced:" << _last_rtp_stamp << " -> " << rtp_stamp;
    update(rtp_stamp, _last_ntp_stamp_us);
    return _last_ntp_stamp_us;
}

} // namespace mediakit
