#ifndef COMMON_EVENTSPOOL_H
#define COMMON_EVENTSPOOL_H

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "Common/EventRecord.h"

namespace managerkit {

/**
 * Disk spool of event bus records, kept while the broker cannot take them and replayed in order
 * afterwards.
 *
 *   <dir>/seg-<20 digits>.log   append-only: magic u16 | flags u16 | len u32 | crc32 u32 | payload[len]
 *   <dir>/checkpoint            "<segment> <offset>\n": next record to send (tmp + rename)
 *
 * Payload: key, header count, header names/values, value; each string u32 length + bytes, all
 * little endian. Segments wholly before the checkpoint are deleted. open() cuts a segment at its
 * first damaged record (short write, bad CRC).
 *
 * Blocking file IO and not thread-safe: one serial queue on a blocking pool owns it.
 */
class EventSpool {
public:
    struct Options {
        std::string dir;
        uint64_t maxBytes = 1024ULL << 20;    // over it the oldest segment is dropped
        uint64_t segmentBytes = 16ULL << 20;  // a segment is closed once it reaches this size
        uint64_t maxAgeSec = 72 * 3600;       // segments last written longer ago are dropped (0 = no limit)
        uint64_t minFreeBytes = 512ULL << 20; // append refuses when the file system has less free space
    };

    struct Position {
        uint64_t segment = 0;
        uint64_t offset = 0;
        bool operator==(const Position &o) const { return segment == o.segment && offset == o.offset; }
        bool operator!=(const Position &o) const { return !(*this == o); }
    };

    enum class AppendResult { Ok, NoSpace, IoError };
    enum class DropReason { MaxSize, MaxAge };

    struct Stats {
        uint64_t bytes = 0;          // all segment files
        uint64_t pending = 0;        // records after the checkpoint
        uint64_t corrupt = 0;        // damaged tails cut by open()
        uint64_t droppedRecords = 0; // unsent records lost to maxBytes / maxAge, since open()
    };

    explicit EventSpool(Options opts);
    ~EventSpool();

    // Creates the directory (0700), scans the segments, loads the checkpoint.
    bool open(std::string &err);

    // Appends in order; all or none. Dropping old segments to stay under maxBytes happens here.
    AppendResult append(const std::vector<EventRecord> &records, std::string *err = nullptr);

    // fdatasync of the segment being written, when it has unsynced data.
    void sync();

    // Up to `max` records from `from` on (skips to the next segment at the end of one). `out` holds
    // each record with its own position; `next` is the position after the last one returned.
    void read(const Position &from, size_t max, std::vector<std::pair<Position, EventRecord>> &out, Position &next);

    // `records` records from the checkpoint up to `next` were delivered. The checkpoint file is
    // written, and the segments wholly before it deleted, by flushCheckpoint().
    void commit(const Position &next, uint64_t records);
    // Writes the checkpoint file when it changed since the last call, then deletes sent segments.
    bool flushCheckpoint();

    // Drops segments last written more than maxAgeSec before `now`. Returns records dropped.
    uint64_t dropExpired(time_t now);

    Position checkpoint() const { return _checkpoint; }
    Position end() const;
    bool empty() const { return _stats.pending == 0; }
    const Stats &stats() const { return _stats; }

    // Called after records were dropped: how many and why (maxBytes / maxAge).
    void setDropCallback(std::function<void(uint64_t records, DropReason reason)> cb) { _on_drop = std::move(cb); }

    static void encode(const EventRecord &record, std::string &out);
    // Decodes one payload; false when it is malformed.
    static bool decodePayload(const char *data, size_t size, EventRecord &out);

private:
    struct Segment {
        uint64_t id = 0;
        uint64_t bytes = 0;
        uint64_t records = 0;
        time_t mtime = 0;
    };

    std::string segmentPath(uint64_t id) const;
    bool scanSegment(Segment &seg, std::string &err);
    bool openWriter(std::string &err);
    void closeWriter();
    void dropOldest(DropReason reason);
    // Removes the first segment (all sent); its file goes once the checkpoint past it is written.
    void retireFront();

private:
    Options _opts;
    std::deque<Segment> _segments; // ascending ids; the last one is written to
    uint64_t _next_id = 1;         // id of the next segment created
    int _fd = -1;                  // writer of _segments.back()
    bool _dirty = false;           // data written since the last fdatasync
    Position _checkpoint;
    uint64_t _checkpoint_records = 0; // records of the checkpoint segment before the checkpoint
    bool _checkpoint_dirty = false;
    std::vector<uint64_t> _retired; // sent segments, deleted by flushCheckpoint()
    Stats _stats;
    std::function<void(uint64_t, DropReason)> _on_drop;
};

} // namespace managerkit

#endif // COMMON_EVENTSPOOL_H
