#ifndef S3MEDIAKIT_EVENTRECORDSESSION_H
#define S3MEDIAKIT_EVENTRECORDSESSION_H

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include "Util/util.h"
#include "Recorder.h"

namespace mediakit {

/**
 * Handle for an event-based recording clip created by
 * MultiMediaSourceMuxer::startEventRecord().
 *
 * Lifecycle:
 *   1. startEventRecord(type, back_ms, forward_ms=0) → returns a session.
 *      - forward_ms == 0  : clip runs indefinitely until stop() is called.
 *      - forward_ms >  0  : clip auto-stops after forward_ms (one-shot clip).
 *   2. extend(post_ms) : while event is still ongoing, push the deadline to
 *                         (last_frame_dts + post_ms).  Safe to call any time.
 *   3. stop(post_ms=0) : mark event as finished; record post_ms more ms then
 *                         close the file automatically.
 *
 * Thread-safe: extend() and stop() may be called from any thread.
 *
 * Internal fields are public so that the ring-reader lambda defined inside
 * MultiMediaSourceMuxer::startEventRecord() can access them directly without
 * requiring additional friend boilerplate.
 */
class EventRecordSession {
public:
    using Ptr = std::shared_ptr<EventRecordSession>;

    // ── public API ──────────────────────────────────────────────────────────

    /**
     * Extend the recording window.  Pushes the DTS stop-deadline to at least
     * (last_frame_dts + post_ms) without ever moving it backward.
     * Call this while the triggering event is still ongoing.
     */
    void extend(uint32_t post_ms) {
        auto last = last_dts.load(std::memory_order_relaxed);
        if (last > 0) {
            auto new_end = last + static_cast<uint64_t>(post_ms);
            auto cur = end_dts.load(std::memory_order_relaxed);
            // Only push forward, never backward.
            while (new_end > cur &&
                   !end_dts.compare_exchange_weak(cur, new_end,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {}
        }
        auto new_wall = toolkit::getCurrentMillisecond(true) + post_ms + 3000ULL;
        auto cur_wall = wall_deadline_ms.load(std::memory_order_relaxed);
        while (new_wall > cur_wall &&
               !wall_deadline_ms.compare_exchange_weak(cur_wall, new_wall,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {}
    }

    /**
     * Signal that the triggering event has ended.
     * Records post_ms more ms (from the last written frame) then closes file.
     * Calling stop() when forward_ms > 0 overrides the auto-stop deadline.
     */
    void stop(uint32_t post_ms = 0) {
        auto last = last_dts.load(std::memory_order_relaxed);
        // Set deadline = last_written_dts + post_ms.
        // If stop() is called before any frame was written (last==0), use
        // post_ms as the very first deadline so the callback picks it up.
        uint64_t new_end = (last > 0) ? (last + static_cast<uint64_t>(post_ms))
                                      : static_cast<uint64_t>(post_ms);
        end_dts.store(new_end, std::memory_order_release);
        wall_deadline_ms.store(toolkit::getCurrentMillisecond(true) + post_ms + 3000ULL, std::memory_order_release);
    }

    /** Recording type (MP4Recorder manages individual file names). */
    const Recorder::type &getRecordingType() const { return type; }

    /** True while the ring reader is still active (file is being written). */
    bool isActive() const { return active.load(std::memory_order_acquire); }

public:
    // ── internal fields (used by MultiMediaSourceMuxer callback) ────────────

    Recorder::type type;
    uint32_t    initial_forward_ms = 0; ///< 0 = infinite (requires stop())

    std::atomic<int>      selected_index{-1};
    std::atomic<uint64_t> last_dts{0};

    /// DTS at which to stop writing.  std::numeric_limits<uint64_t>::max()
    /// means "no DTS limit" (infinite until stop() is called).
    std::atomic<uint64_t> end_dts{std::numeric_limits<uint64_t>::max()};

    /// Wall-clock epoch-ms at which to force-stop (live stream safety net).
    /// std::numeric_limits<uint64_t>::max() = no wall-clock limit.
    std::atomic<uint64_t> wall_deadline_ms{std::numeric_limits<uint64_t>::max()};

    std::atomic<bool> active{true};
};

} // namespace mediakit

#endif // S3MEDIAKIT_EVENTRECORDSESSION_H
