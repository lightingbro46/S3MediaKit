#include "MotionMuxer.h"

#include "Util/File.h"
#include "Util/logger.h"
#include "Util/util.h"

#include <sys/stat.h>

using namespace std;
using namespace toolkit;

namespace mediakit {

MotionMuxer::MotionMuxer(std::string base_path, uint64_t summary_window_ms)
    : _base_path(std::move(base_path)), _summary_window_ms(summary_window_ms) {}

MotionMuxer::~MotionMuxer() {
    closeWriter();
}

bool MotionMuxer::inputEvent(const MotionEventBlock &block) {
    if (!_recording) {
        // Buffer the event so it is not lost if motion is confirmed shortly after.
        _pre_buffer.push_back(block);
        return true;
    }
    rollIfNeeded();
    if (!_writer) {
        return false;
    }
    if (!_writer->appendEvent(block)) {
        WarnL << "MotionMuxer: appendEvent failed";
        return false;
    }
    _agg->inputEvent(block);
    return true;
}

void MotionMuxer::setRecording(bool recording) {
    if (recording && !_recording) {
        // Motion just confirmed — flush the pre-buffer so no events are missed.
        rollIfNeeded();
        if (_writer) {
            const size_t n = _pre_buffer.size();
            for (const auto &e : _pre_buffer) {
                _writer->appendEvent(e);
                _agg->inputEvent(e);
            }
            if (n > 0) {
                DebugL << "MotionMuxer: flushed " << n << " pre-buffered events on motion confirm";
            }
            // Flush stdio buffer to disk so the .mblk file is visible immediately.
            // The index is mmap-backed and already on disk; the block file uses a
            // 64 KB stdio buffer that would otherwise stay in userspace until full.
            _writer->flush();
        }
        _pre_buffer.clear();
    } else if (!recording) {
        // Motion ended — flush the aggregator summary window, then flush the
        // block file so all pending data reaches disk before the next reader.
        _pre_buffer.clear();
        if (_agg) _agg->flush();
        if (_writer) _writer->flush();
    }
    _recording = recording;
}

void MotionMuxer::clearPreBuffer() {
    _pre_buffer.clear();
}

void MotionMuxer::flush() {
    if (_agg)    { _agg->flush(); }
    if (_writer) { _writer->flush(); }
}

void MotionMuxer::rollIfNeeded() {
    const std::string today = getTimeStr("%Y-%m-%d");
    if (today != _current_date) {
        closeWriter();
        openForDate(today);
        _current_date = today;
    }
}

void MotionMuxer::openForDate(const std::string &date) {
    // Ensure the directory tree exists before creating files in it.
    File::create_path(_base_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    const std::string blk = _base_path + date + ".mblk";
    const std::string idx = _base_path + date + ".idx";

    auto writer = std::make_shared<MotionEventWriter>();
    if (!writer->open(blk, idx)) {
        WarnL << "MotionMuxer: failed to open " << blk;
        return;
    }
    _writer = std::move(writer);

    // Aggregator callback writes completed summaries to the current _writer.
    // Uses weak_ptr so that a delayed flush after destruction is a safe no-op.
    std::weak_ptr<MotionMuxer> weak_self = shared_from_this();
    _agg = std::make_shared<MotionAggregator>(
        _summary_window_ms, [weak_self](MotionSummaryBlock::Ptr summary) {
            auto self = weak_self.lock();
            if (self && self->_writer && summary) {
                if (!self->_writer->appendSummary(*summary)) {
                    WarnL << "MotionMuxer: appendSummary failed";
                }
            }
        });

    DebugL << "MotionMuxer: opened " << blk;
}

void MotionMuxer::closeWriter() {
    // Flush the aggregator first so any pending summary goes to _writer.
    if (_agg) {
        _agg->flush();
        _agg.reset();
    }
    if (_writer) {
        _writer->flush();
        _writer->close();
        _writer.reset();
    }
}

} // namespace mediakit
