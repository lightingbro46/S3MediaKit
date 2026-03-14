#ifndef MOTION_AGGREGATOR_H
#define MOTION_AGGREGATOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "MotionBlock.h"

namespace mediakit {

/**
 * Aggregates a stream of MotionEventBlock objects over a time window and
 * produces a MotionSummaryBlock by OR-ing all event bitmaps together.
 *
 * Usage (window-based):
 *   MotionAggregator agg(5000 /*ms*\/, [](MotionSummaryBlock::Ptr s) {
 *       writer.appendSummary(*s);
 *   });
 *   agg.inputEvent(eventBlock);   // call for every event
 *   agg.flush();                  // emit any pending summary at stream end
 *
 * A summary is emitted automatically when:
 *   - The next event's stamp falls outside the current window, OR
 *   - flush() is called explicitly.
 */
class MotionAggregator {
public:
    using Ptr        = std::shared_ptr<MotionAggregator>;
    using OnSummary  = std::function<void(MotionSummaryBlock::Ptr)>;

    /**
     * @param window_ms   Time window in milliseconds. All events within
     *                    [window_start, window_start + window_ms) are merged
     *                    into one summary block.
     * @param on_summary  Callback invoked each time a summary is ready.
     */
    explicit MotionAggregator(uint64_t window_ms, OnSummary on_summary)
        : _window_ms(window_ms), _on_summary(std::move(on_summary)) {}

    ~MotionAggregator() = default;

    MotionAggregator(const MotionAggregator &) = delete;
    MotionAggregator &operator=(const MotionAggregator &) = delete;

    /**
     * Feed one event into the aggregator.
     * If the event falls outside the current window, the accumulated summary
     * is emitted first, then a new window is started.
     */
    void inputEvent(const MotionEventBlock &event) {
        const auto &ext   = event.extHeader();
        const int   rows  = ext.rows;
        const int   cols  = ext.cols;
        const uint64_t ts = event.stamp();

        // Emit current window if this event is outside it.
        if (_has_data && ts >= _window_end) {
            _emit();
        }

        // Start a new window.
        if (!_has_data) {
            _start(ts, rows, cols);
        }

        // Dimension mismatch: emit what we have and restart.
        if (rows != _rows || cols != _cols) {
            _emit();
            _start(ts, rows, cols);
        }

        // OR this event's bitmap into the accumulator.
        const size_t bitmap_bytes = (static_cast<size_t>(rows) * cols + 7) / 8;
        const auto  &src          = event.bitmap();
        if (src.size() >= bitmap_bytes) {
            int added_cells = 0;
            for (size_t i = 0; i < bitmap_bytes; ++i) {
                const uint8_t before = _bitmap[i];
                _bitmap[i] |= src[i];
                // Count newly set bits (population count of changed bits).
                added_cells += __builtin_popcount(static_cast<uint8_t>(_bitmap[i] & ~before));
            }
            _active_cells += added_cells;
        }

        _end_stamp = ts;
        _event_count++;
    }

    /**
     * Emit any pending summary immediately.
     * Call at the end of a segment/stream to ensure the last window is flushed.
     */
    void flush() {
        if (_has_data) {
            _emit();
        }
    }

    /** Number of events accumulated in the current (not yet emitted) window. */
    int  pendingEventCount() const { return _event_count; }
    bool hasPendingData()    const { return _has_data; }

private:
    void _start(uint64_t start_stamp, int rows, int cols) {
        _rows        = rows;
        _cols        = cols;
        _start_stamp = start_stamp;
        _end_stamp   = start_stamp;
        _window_end  = start_stamp + _window_ms;
        _active_cells = 0;
        _event_count  = 0;
        _bitmap.assign((static_cast<size_t>(rows) * cols + 7) / 8, 0);
        _has_data    = true;
    }

    void _emit() {
        if (!_has_data) return;

        MotionSummaryExtHeader ext{};
        ext.end_stamp    = _end_stamp;
        ext.rows         = static_cast<uint16_t>(_rows);
        ext.cols         = static_cast<uint16_t>(_cols);
        ext.active_cells = static_cast<uint16_t>(
            _active_cells > 0xFFFF ? 0xFFFF : _active_cells);

        auto block = std::make_shared<MotionSummaryBlock>(
            _start_stamp, ext, _bitmap);

        if (_on_summary) {
            _on_summary(std::move(block));
        }

        _has_data     = false;
        _event_count  = 0;
        _active_cells = 0;
        _bitmap.clear();
    }

    const uint64_t _window_ms;
    OnSummary      _on_summary;

    bool          _has_data     = false;
    int           _rows         = 0;
    int           _cols         = 0;
    uint64_t      _start_stamp  = 0;
    uint64_t      _end_stamp    = 0;
    uint64_t      _window_end   = 0;
    int           _active_cells = 0;
    int           _event_count  = 0;
    std::vector<uint8_t> _bitmap;
};

} // namespace mediakit

#endif // MOTION_AGGREGATOR_H
