#ifndef MOTION_MUXER_H
#define MOTION_MUXER_H

#include "MotionBlock.h"
#include "MotionAggregator.h"
#include "Util/BlockStorageEngine.h"
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
                       reinterpret_cast<const uint8_t *>(&block.extHeader()),
                       sizeof(MotionEventExtHeader));
    }

    bool appendSummary(const MotionSummaryBlock &block) {
        return append(block, MotionBlockType::Summary,
                       reinterpret_cast<const uint8_t *>(&block.extHeader()),
                       sizeof(MotionSummaryExtHeader));
    }

private:
    bool append(const toolkit::BlockInterface &block, MotionBlockType type,
                 const uint8_t *ext, uint16_t ext_size) {
        auto hdr = block.buildBaseHeader();

        // Build index entry with type before writing (position() is the pre-write offset).
        MotionIndexEntry entry{};
        entry.stamp  = hdr.stamp;
        entry.offset = writer().position();
        entry.type   = static_cast<uint16_t>(type);

        const uint8_t *bitmap_data = nullptr;
        uint32_t       bitmap_size = 0;
        // Decode the concrete type to get the raw bitmap pointer.
        if (type == MotionBlockType::Event) {
            const auto &b = static_cast<const MotionEventBlock &>(block);
            bitmap_data   = b.bitmap().data();
            bitmap_size   = static_cast<uint32_t>(b.bitmap().size());
        } else {
            const auto &b = static_cast<const MotionSummaryBlock &>(block);
            bitmap_data   = b.bitmap().data();
            bitmap_size   = static_cast<uint32_t>(b.bitmap().size());
        }
        TraceL << "Append block" << " stamp: " << hdr.stamp
               << " ext_size: " << ext_size
               << " bitmap_size: " << bitmap_size;
        if (!writer().appendBlock(hdr, ext, ext_size, bitmap_data, bitmap_size)) {
            return false;
        }
        TraceL << "Block written at offset " << entry.offset;
        return index().addEntry(entry);
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
 *   {base_path}/YYYYMMDD.blk  — packed block data
 *   {base_path}/YYYYMMDD.idx  — mmap index (stamp + offset + type)
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
     * @param base_path          Directory where date-named files are created.
     *                           Created automatically if it does not exist.
     * @param summary_window_ms  Aggregation window for MotionAggregator (ms).
     *                           Default: 5 minutes.
     */
    explicit MotionMuxer(std::string base_path, uint64_t summary_window_ms = 10000);
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
    std::string            _base_path;
    std::string            _current_date;
    uint64_t               _summary_window_ms;
    bool                   _recording = false;        // controlled by MotionEventController
    std::deque<MotionEventBlock> _pre_buffer;         // events buffered before motion is confirmed
    MotionEventWriter::Ptr _writer;
    MotionAggregator::Ptr  _agg;
};

} // namespace mediakit

#endif // MOTION_MUXER_H
