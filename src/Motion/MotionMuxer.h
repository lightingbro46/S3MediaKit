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
 *   1. Accept MotionEventBlock inputs from MotionProcessor and cache them in
 *      a single per-stream deque (_raw_buffer), sorted by stamp.
 *   2. On kBroadcastRecordMP4 (MP4 segment released):
 *        a. Drop events with stamp < seg_start_ms (before this segment).
 *        b. Collect events with stamp in [seg_start_ms, seg_end_ms].
 *        c. Re-aggregate collected events via MotionAggregator to produce
 *           summaries, interleaved in timestamp order with the events.
 *        d. Write Meta + interleaved (events + summaries) to the daily .mblk.
 *        e. Pop committed events from the front; keep events > seg_end_ms
 *           for the next segment.
 *   3. setRecording(false) only triggers a broadcast notification; no disk I/O.
 *
 * On-disk layout under base_path (one file pair per calendar day):
 *   {base_path}/YYYY-MM-DD.mblk
 *   {base_path}/YYYY-MM-DD.idx
 */
class MotionMuxer : public std::enable_shared_from_this<MotionMuxer> {
public:
    using Ptr = std::shared_ptr<MotionMuxer>;

    /**
     * @param tuple              Stream identity (app / stream / vhost).
     * @param roi_mask           Serialised ROI mask binary blob.
     * @param save_path          Directory where date-named files are created.
     * @param summary_window_ms  Aggregation window for MotionAggregator (ms).
     */
    explicit MotionMuxer(const MediaTuple &tuple, std::string roi_mask,
                         std::string save_path, uint64_t summary_window_ms = 10000,
                         std::string record_stream_id = "");
    ~MotionMuxer();

    /**
     * Set up listener to the media source's kBroadcastRecordMP4 events, which trigger the .mblk writes.
     */
    void setMediaSourceListener();

    /**
     * Cache one event block.
     * Events are always appended to _raw_buffer; no disk I/O here.
     * Old events beyond 2× kMP4MaxSecond are trimmed to bound memory.
     */
    bool inputEvent(const MotionEventBlock &block);

    /**
     * Called when a MP4 segment is released (kBroadcastRecordMP4).
     * Writes the motion data for [info.start_time, start_time+time_len] to
     * the daily .mblk file and trims the committed events from _raw_buffer.
     */
    void onSegmentCommit(const RecordInfo &info);

private:
    /** One entry in the time-ordered write list: either an event or a summary. */
    using TimeOrderedBlock = std::shared_ptr<toolkit::BlockInterface>;

    /** Build the time-ordered block list for [seg_start_ms, seg_end_ms]. */
    std::vector<TimeOrderedBlock> buildOrderedBlocks(
        const std::vector<MotionEventBlock> &seg_events) const;

    void emitEvent(bool start, uint64_t seg_end_ms);

private:
    MotionMeta  _meta;
    std::string _base_path;
    uint64_t    _summary_window_ms;
    uint64_t    _max_buffer_ms = 600000; // 2 × default 5-min segment

    // Stream ID whose kBroadcastRecordMP4 events trigger .mblk writes.
    // Defaults to _meta.stream_id (own stream); overridden when motion
    // detection runs on a secondary stream but recording is on primary.
    std::string _record_stream_id;

    std::deque<MotionEventBlock> _raw_buffer; // all cached events, sorted by stamp
};

} // namespace mediakit

#endif // ENABLE_MOTION

#endif // MOTION_MUXER_H
