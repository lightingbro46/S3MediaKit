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
 *   2. resume()          : a new event arrived while session is in post-tail;
 *                         resets deadline to infinite so the clip stays open
 *                         until the next stop() call.
 *   3. extend(post_ms)   : push the DTS deadline forward by post_ms from the
 *                         last frame.  Only useful after stop() has been called
 *                         and the deadline is already finite — do NOT use this
 *                         when a new event arrives (use resume() instead).
 *   4. stop(post_ms=0)  : mark event as finished; record post_ms more ms then
 *                         close the file automatically.
 *
 * Thread-safe: all public methods may be called from any thread.
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
     * Resume an infinite session after a previous stop() was called but the
     * file has not yet closed (i.e. still in the post-event tail window).
     * Resets both DTS and wall-clock deadlines to "no limit" so the clip keeps
     * recording until the next stop() call, regardless of how long the new
     * event lasts.
     * No-op if the session is already infinite (end_dts == UINT64_MAX).
     */
    void resume() {
        end_dts.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
        wall_deadline_ms.store(std::numeric_limits<uint64_t>::max(), std::memory_order_release);
    }

    /**
     * Push the DTS stop-deadline further into the future.
     * Only useful after stop() has set a finite deadline — if end_dts is still
     * UINT64_MAX (infinite) this is a no-op.
     * Do NOT call this when a new event arrives mid-tail; use resume() instead
     * so the clip stays open however long the new event lasts.
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

    void setOnStop(const std::function<void()> &cb) { onStop = std::move(cb); }

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

    std::function<void()> onStop; // callback to trigger when the session is stopped (file is closed)
};

} // namespace mediakit

#endif // S3MEDIAKIT_EVENTRECORDSESSION_H
