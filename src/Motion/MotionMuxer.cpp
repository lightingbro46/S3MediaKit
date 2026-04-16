#ifdef ENABLE_MOTION

#include "MotionMuxer.h"

#include "Common/config.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Util/NoticeCenter.h"
#include "Util/util.h"

#include <algorithm>
#include <sys/stat.h>

using namespace std;
using namespace toolkit;

namespace mediakit {

MotionMuxer::MotionMuxer(const MediaTuple &tuple, std::string roi_mask,
                         std::string save_path, uint64_t summary_window_ms,
                         std::string record_stream_id)
    : _meta(tuple.app, tuple.stream, std::move(roi_mask), MOTION_GRID_ROWS, MOTION_GRID_COLS)
    , _base_path(std::move(save_path))
    , _summary_window_ms(summary_window_ms)
    , _record_stream_id(std::move(record_stream_id)) {

    GET_CONFIG(uint32_t, max_second, Protocol::kMP4MaxSecond);
    _max_buffer_ms = static_cast<uint64_t>(max_second) * 2 * 1000;

}

void MotionMuxer::setMediaSourceListener() {
    // Subscribe to MP4 segment release events from the recording stream (primary).
    // When motion detection runs on a secondary stream while recording is on the
    // primary stream, _record_stream_id is set to the primary stream's ID so that
    // .mblk files are written only when the primary segment is committed.
    // If empty, fall back to own stream ID (motion detect == record stream).
    const std::string my_app = _meta.device_id;
    const std::string record_stream = _record_stream_id.empty() ? _meta.stream_id : _record_stream_id;
    std::weak_ptr<MotionMuxer> weak_self = shared_from_this();

    NoticeCenter::Instance().addListener(
        this, Broadcast::kBroadcastRecordMP4,
        [weak_self, my_app, record_stream](BroadcastRecordMP4Args) {
            if (info.app != my_app || info.stream != record_stream) return;
            if (auto self = weak_self.lock()) self->onSegmentCommit(info);
        });
}

MotionMuxer::~MotionMuxer() {
    NoticeCenter::Instance().delListener(this, Broadcast::kBroadcastRecordMP4);
}

bool MotionMuxer::inputEvent(const MotionEventBlock &block) {
    // Trim events that are too old to belong to any pending segment.
    if (!_raw_buffer.empty() &&
        block.stamp() > _raw_buffer.front().stamp() + _max_buffer_ms) {
        const uint64_t cutoff = block.stamp() - _max_buffer_ms;
        while (!_raw_buffer.empty() && _raw_buffer.front().stamp() < cutoff) {
            _raw_buffer.pop_front();
        }
    }
    _raw_buffer.push_back(block);
    return true;
}

// ── onSegmentCommit ───────────────────────────────────────────────────────────

void MotionMuxer::onSegmentCommit(const RecordInfo &info) {
    if (info.time_len <= 0.0f) return;

    const uint64_t seg_start_ms = static_cast<uint64_t>(info.start_time) * 1000ULL;
    const uint64_t seg_end_ms   = seg_start_ms +
                                   static_cast<uint64_t>(info.time_len * 1000.0f);

    // 1. Drop events that pre-date this segment (they belong to no segment).
    while (!_raw_buffer.empty() && _raw_buffer.front().stamp() < seg_start_ms) {
        _raw_buffer.pop_front();
    }

    // 2. Collect events in [seg_start_ms, seg_end_ms].
    std::vector<MotionEventBlock> seg_events;
    for (const auto &e : _raw_buffer) {
        if (e.stamp() > seg_end_ms) break;
        seg_events.push_back(e);
    }

    if (seg_events.empty()) {
        DebugL << "MotionMuxer: no motion events for segment " << info.file_name;
        return;
    }

    // 3. Build time-ordered list of events and summaries.
    auto ordered = buildOrderedBlocks(seg_events);

    // 4. Open (or append to) the daily .mblk for the segment's date.
    File::create_path(_base_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    const std::string date = getTimeStr("%Y-%m-%d", info.start_time);
    const std::string blk  = _base_path + date + ".mblk";
    const std::string idx  = _base_path + date + ".idx";

    MotionEventWriter writer;
    if (!writer.open(blk, idx)) {
        WarnL << "MotionMuxer: cannot open " << blk;
        return;
    }

    // Write a Meta block to anchor device/stream identity.
    MotionMetaBlock meta_block(getCurrentMillisecond(true),
                               _meta.device_id, _meta.stream_id, _meta.roi_mask,
                               _meta.rows, _meta.cols);
    writer.appendMeta(meta_block);

    // 5. Write events and summaries in timestamp order.
    for (const auto &entry : ordered) {
        if (entry->type() == static_cast<uint16_t>(MotionBlockType::Event)) {
            writer.appendEvent(static_cast<const MotionEventBlock &>(*entry));
        } else {
            writer.appendSummary(static_cast<const MotionSummaryBlock &>(*entry));
        }
    }

    writer.flush();
    writer.close();

    DebugL << "MotionMuxer: committed " << seg_events.size()
           << " events for segment " << info.file_name
           << " [" << seg_start_ms << ", " << seg_end_ms << "] → " << blk;

    // 6. Remove committed events from buffer; keep events after seg_end_ms.
    while (!_raw_buffer.empty() && _raw_buffer.front().stamp() <= seg_end_ms) {
        _raw_buffer.pop_front();
    }
}

// ── buildOrderedBlocks ────────────────────────────────────────────────────────

std::vector<MotionMuxer::TimeOrderedBlock>
MotionMuxer::buildOrderedBlocks(const std::vector<MotionEventBlock> &seg_events) const {
    // Aggregate the segment events into summary blocks (in-memory only).
    std::vector<MotionSummaryBlock> summaries;
    MotionAggregator agg(_summary_window_ms, [&](MotionSummaryBlock::Ptr s) {
        if (s) summaries.push_back(std::move(*s));
    });
    for (const auto &e : seg_events) {
        agg.inputEvent(e);
    }
    agg.flush();

    // Merge events and summaries into a single stamp-sorted list.
    std::vector<TimeOrderedBlock> ordered;
    ordered.reserve(seg_events.size() + summaries.size());

    size_t ei = 0, si = 0;
    while (ei < seg_events.size() && si < summaries.size()) {
        // A summary's stamp is its window start; place it before events at the
        // same stamp so that readers see the summary first (consistent ordering).
        if (summaries[si].stamp() <= seg_events[ei].stamp()) {
            ordered.push_back(std::make_shared<MotionSummaryBlock>(std::move(summaries[si++])));
        } else {
            ordered.push_back(std::make_shared<MotionEventBlock>(seg_events[ei++]));
        }
    }
    while (ei < seg_events.size()) ordered.push_back(std::make_shared<MotionEventBlock>(seg_events[ei++]));
    while (si < summaries.size())  ordered.push_back(std::make_shared<MotionSummaryBlock>(std::move(summaries[si++])));

    return ordered;
}

} // namespace mediakit

#endif // ENABLE_MOTION
