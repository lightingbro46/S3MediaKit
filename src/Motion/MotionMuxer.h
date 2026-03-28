#ifndef MOTION_MUXER_H
#define MOTION_MUXER_H

#ifdef ENABLE_MOTION

#include "MotionBlock.h"
#include "MotionAggregator.h"
#include "Util/BlockStorageEngine.h"
#include "Record/Recorder.h"
#include "Poller/Timer.h"
#include <deque>

namespace mediakit {

/**
 * Writes MotionEventBlock and MotionSummaryBlock to a block file with
 * a mmap-backed MotionIndexEntry index (stamp + offset + type).
 * Extends toolkit::BlockStorageEngine<MotionIndexEntry> – inherits
 * open(), close(), flush(), entryCount(), getIndexEntry().
 *
 * Usage:
 *   MotionEventWriter w;
 *   w.open("segment.blk", "segment.idx");
 *   w.appendEvent(eventBlock);
 *   w.close();
 */
class MotionEventWriter : public toolkit::BlockStorageWriter<MotionIndexEntry> {
public:
    using Ptr = std::shared_ptr<MotionEventWriter>;

    MotionEventWriter() = default;

    bool appendEvent(const MotionEventBlock &block) {
        return append(block, MotionBlockType::Event,
                      reinterpret_cast<const uint8_t *>(&block.extHeader()), sizeof(MotionEventExtHeader),
                      block.bitmap().data(), static_cast<uint32_t>(block.bitmap().size()));
    }

    bool appendSummary(const MotionSummaryBlock &block) {
        return append(block, MotionBlockType::Summary,
                      reinterpret_cast<const uint8_t *>(&block.extHeader()), sizeof(MotionSummaryExtHeader),
                      block.bitmap().data(), static_cast<uint32_t>(block.bitmap().size()));
    }

    bool appendMeta(const MotionMetaBlock &block) {
        auto payload = block.buildPayload();
        return append(block, MotionBlockType::Meta,
                      reinterpret_cast<const uint8_t *>(&block.extHeader()), sizeof(MotionMetaExtHeader),
                      payload.data(), static_cast<uint32_t>(payload.size()));
    }

private:
    bool append(const toolkit::BlockInterface &block, MotionBlockType type,
                const uint8_t *ext, uint16_t ext_size,
                const uint8_t *payload, uint32_t payload_size) {
        MotionIndexEntry entry{};
        entry.type = static_cast<uint16_t>(type);
        auto hdr   = block.buildBaseHeader();
        TraceL << "Append block stamp: " << hdr.stamp
               << " type: " << static_cast<int>(type)
               << " ext_size: " << ext_size
               << " payload_size: " << payload_size;
        return appendBlock(entry, hdr, ext, ext_size, payload, payload_size);
    }
};

/**
 * High-level motion file muxer.
 *
 * Responsibilities:
 *   1. Accept MotionEventBlock inputs from MotionProcessor
 *   2. Feed each event to MotionAggregator to automatically produce MotionSummaryBlock
 *   3. Write events and summaries via MotionEventWriter
 *   4. Auto-roll to a new day file when the wall-clock date changes
 *
 * On-disk layout under base_path:
 *   {base_path}/YYYY-MM-DD.blk  — packed block data
 *   {base_path}/YYYY-MM-DD.idx  — mmap index (stamp + offset + type)
 *
 * Usage:
 *   MotionMuxer muxer("/record/motion/app/stream/");
 *   muxer.inputEvent(eventBlock);  // auto-writes summaries on window expiry
 *   muxer.flush();                 // at stream close
 */
class MotionMuxer : public std::enable_shared_from_this<MotionMuxer> {
public:
    using Ptr = std::shared_ptr<MotionMuxer>;

    /**
     * @param tuple              Stream identity (app / stream / vhost).
     * @param roi_mask           Serialised ROI mask binary blob.
     * @param save_path          Directory where date-named files are created.
     *                           Created automatically if it does not exist.
     * @param summary_window_ms  Aggregation window for MotionAggregator (ms).
     *                           Default: 5 minutes.
     */
    explicit MotionMuxer(const MediaTuple &tuple,  std::string roi_mask,
                         std::string save_path, uint64_t summary_window_ms = 10000);
    ~MotionMuxer();

    /**
     * Enable or disable recording.
     * Called by MotionEventController's confirmed-motion callback:
     *   true  → motion confirmed, flush pre-buffer then start writing incoming events
     *   false → motion ended, flush summary window and discard pre-buffer
     */
    void setRecording(bool recording);

    /**
     * Discard all buffered pre-buffer events.
     * Called when the debounce window is interrupted (motion disappears before confirmation).
     */
    void clearPreBuffer();

    /**
     * Write one event block.
     * - When not recording: buffered in the pre-buffer (ring of recent events).
     * - When recording: written directly to file.
     * Rolls the output file automatically if the wall-clock day has changed.
     * @return false if the underlying writer could not be opened or write failed.
     */
    bool inputEvent(const MotionEventBlock &block);

    /**
     * Flush the pending aggregator summary window and the underlying writer.
     * Call before destroying the muxer or when the stream ends.
     */
    void flush();

private:
    // Open writer for the given date; re-creates the aggregator with a callback.
    void openForDate(const std::string &date);

    // Flush aggregator then close and reset the writer.
    void closeWriter();

    // Roll to today's file if the current date has changed.
    void rollIfNeeded();

private:
    std::string            _current_date;
    MotionMeta             _meta;
    std::string            _base_path;
    uint64_t               _summary_window_ms;
    bool                   _recording = false;        // controlled by MotionEventController
    std::deque<MotionEventBlock> _pre_buffer;         // events buffered before motion is confirmed
    MotionEventWriter::Ptr _writer;
    MotionAggregator::Ptr  _agg;
    toolkit::Timer::Ptr    _flush_timer;  // periodic checkpoint every 30s while recording
};

} // namespace mediakit

#endif // ENABLE_MOTION

#endif // MOTION_MUXER_H
