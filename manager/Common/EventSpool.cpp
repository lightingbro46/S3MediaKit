#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>
#include <algorithm>
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/EventSpool.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

static const uint16_t kMagic = 0x5345; // "ES"
static const size_t kHeaderSize = 12;
// Larger lengths are damage, not records (a record is an event of a few hundred bytes).
static const uint32_t kMaxRecordSize = 4U << 20;

static void putU16(string &out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

static void putU32(string &out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}

static uint32_t getU32(const char *p) {
    auto u = reinterpret_cast<const unsigned char *>(p);
    return u[0] | (u[1] << 8) | (u[2] << 16) | (static_cast<uint32_t>(u[3]) << 24);
}

static uint16_t getU16(const char *p) {
    auto u = reinterpret_cast<const unsigned char *>(p);
    return static_cast<uint16_t>(u[0] | (u[1] << 8));
}

static void putStr(string &out, const string &s) {
    putU32(out, static_cast<uint32_t>(s.size()));
    out.append(s);
}

static bool getStr(const char *&p, const char *end, string &out) {
    if (end - p < 4) {
        return false;
    }
    uint32_t len = getU32(p);
    p += 4;
    if (static_cast<uint64_t>(end - p) < len) {
        return false;
    }
    out.assign(p, len);
    p += len;
    return true;
}

static uint32_t crcOf(const char *data, size_t size) {
    return static_cast<uint32_t>(crc32(0L, reinterpret_cast<const Bytef *>(data), static_cast<uInt>(size)));
}

// Whole-buffer pread; false on error or short file.
static bool preadAll(int fd, char *buf, size_t size, uint64_t offset) {
    while (size) {
        auto n = ::pread(fd, buf, size, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        buf += n;
        size -= static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
    }
    return true;
}

void EventSpool::encode(const EventRecord &record, string &out) {
    string payload;
    putStr(payload, record.key);
    putU32(payload, static_cast<uint32_t>(record.headers.size()));
    for (auto &h : record.headers) {
        putStr(payload, h.first);
        putStr(payload, h.second);
    }
    putStr(payload, record.value);

    putU16(out, kMagic);
    putU16(out, 0);
    putU32(out, static_cast<uint32_t>(payload.size()));
    putU32(out, crcOf(payload.data(), payload.size()));
    out.append(payload);
}

bool EventSpool::decodePayload(const char *data, size_t size, EventRecord &out) {
    const char *p = data;
    const char *end = data + size;
    if (!getStr(p, end, out.key) || end - p < 4) {
        return false;
    }
    uint32_t count = getU32(p);
    p += 4;
    out.headers.clear();
    for (uint32_t i = 0; i < count; ++i) {
        string name, value;
        if (!getStr(p, end, name) || !getStr(p, end, value)) {
            return false;
        }
        out.headers.emplace_back(std::move(name), std::move(value));
    }
    return getStr(p, end, out.value) && p == end;
}

EventSpool::EventSpool(Options opts) : _opts(std::move(opts)) {
    // The segment being written is never dropped for size: keep room for at least two.
    _opts.maxBytes = std::max<uint64_t>(_opts.maxBytes, 2);
    _opts.segmentBytes = std::max<uint64_t>(std::min(_opts.segmentBytes, _opts.maxBytes / 2), 1);
}

EventSpool::~EventSpool() {
    closeWriter();
    flushCheckpoint();
}

string EventSpool::segmentPath(uint64_t id) const {
    char name[64];
    snprintf(name, sizeof(name), "/seg-%020llu.log", static_cast<unsigned long long>(id));
    return _opts.dir + name;
}

bool EventSpool::open(string &err) {
    if (!File::create_path(_opts.dir + "/", 0700)) {
        err = "cannot create " + _opts.dir + ": " + strerror(errno);
        return false;
    }
    ::chmod(_opts.dir.data(), 0700);

    vector<uint64_t> ids;
    if (auto dir = opendir(_opts.dir.data())) {
        while (auto ent = readdir(dir)) {
            unsigned long long id = 0;
            char tail[8] = { 0 };
            if (sscanf(ent->d_name, "seg-%20llu.%4s", &id, tail) == 2 && strcmp(tail, "log") == 0) {
                ids.push_back(id);
            }
        }
        closedir(dir);
    } else {
        err = "cannot list " + _opts.dir + ": " + strerror(errno);
        return false;
    }
    std::sort(ids.begin(), ids.end());

    bool have_checkpoint = false;
    if (auto fp = fopen((_opts.dir + "/checkpoint").data(), "r")) {
        unsigned long long seg = 0, off = 0;
        have_checkpoint = fscanf(fp, "%llu %llu", &seg, &off) == 2;
        fclose(fp);
        _checkpoint.segment = seg;
        _checkpoint.offset = off;
    }
    if (!have_checkpoint) {
        _checkpoint.segment = ids.empty() ? 1 : ids.front();
        _checkpoint.offset = 0;
    }

    _segments.clear();
    _stats = Stats();
    _checkpoint_records = 0;
    uint64_t next_id = _checkpoint.segment;
    for (auto id : ids) {
        next_id = std::max(next_id, id);
        if (id < _checkpoint.segment) {
            // Sent before the last run stopped.
            ::unlink(segmentPath(id).data());
            continue;
        }
        Segment seg;
        seg.id = id;
        if (!scanSegment(seg, err)) {
            return false;
        }
        if (!seg.bytes) {
            ::unlink(segmentPath(id).data());
            continue;
        }
        _stats.bytes += seg.bytes;
        _segments.push_back(seg);
    }
    _next_id = next_id + 1;

    if (_segments.empty() || _segments.front().id != _checkpoint.segment) {
        // The checkpoint segment was fully sent (and removed): start at the next one.
        _checkpoint.segment = _segments.empty() ? _next_id : _segments.front().id;
        _checkpoint.offset = 0;
        _checkpoint_records = 0;
        _checkpoint_dirty = true;
    }
    uint64_t total = 0;
    for (auto &seg : _segments) {
        total += seg.records;
    }
    _stats.pending = total - std::min(total, _checkpoint_records);
    if (_stats.pending || _stats.corrupt) {
        InfoL << "EventSpool " << _opts.dir << ": " << _stats.pending << " records pending in " << _segments.size()
              << " segments (" << _stats.bytes << " bytes), checkpoint " << _checkpoint.segment << ":"
              << _checkpoint.offset << ", damaged tails cut " << _stats.corrupt;
    }
    return true;
}

bool EventSpool::scanSegment(Segment &seg, string &err) {
    auto path = segmentPath(seg.id);
    int fd = ::open(path.data(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        err = "cannot open " + path + ": " + strerror(errno);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        err = "cannot stat " + path + ": " + strerror(errno);
        ::close(fd);
        return false;
    }
    seg.mtime = st.st_mtime;
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    const bool is_checkpoint = seg.id == _checkpoint.segment;
    uint64_t offset = 0;
    char header[kHeaderSize];
    string payload;
    while (offset < size) {
        if (size - offset < kHeaderSize || !preadAll(fd, header, kHeaderSize, offset)) {
            break;
        }
        uint32_t len = getU32(header + 4);
        if (getU16(header) != kMagic || len > kMaxRecordSize || size - offset - kHeaderSize < len) {
            break;
        }
        payload.resize(len);
        if (len && !preadAll(fd, &payload[0], len, offset + kHeaderSize)) {
            break;
        }
        if (crcOf(payload.data(), len) != getU32(header + 8)) {
            break;
        }
        if (is_checkpoint && offset < _checkpoint.offset) {
            ++_checkpoint_records;
        }
        offset += kHeaderSize + len;
        ++seg.records;
    }
    if (offset < size) {
        WarnL << "EventSpool " << path << ": damaged record at " << offset << ", cutting " << (size - offset)
              << " bytes";
        ++_stats.corrupt;
        if (ftruncate(fd, static_cast<off_t>(offset)) != 0) {
            WarnL << "EventSpool truncate " << path << " failed: " << strerror(errno);
        }
    }
    ::close(fd);
    seg.bytes = offset;
    if (is_checkpoint && _checkpoint.offset > offset) {
        _checkpoint.offset = offset;
        _checkpoint_dirty = true;
    }
    return true;
}

EventSpool::Position EventSpool::end() const {
    if (_segments.empty()) {
        return _checkpoint;
    }
    Position ret;
    ret.segment = _segments.back().id;
    ret.offset = _segments.back().bytes;
    return ret;
}

bool EventSpool::openWriter(string &err) {
    closeWriter();
    Segment seg;
    seg.id = _next_id++;
    seg.mtime = time(nullptr);
    auto path = segmentPath(seg.id);
    _fd = ::open(path.data(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (_fd < 0) {
        err = "cannot create " + path + ": " + strerror(errno);
        return false;
    }
    _segments.push_back(seg);
    return true;
}

void EventSpool::closeWriter() {
    if (_fd < 0) {
        return;
    }
    if (_dirty) {
        ::fdatasync(_fd);
        _dirty = false;
    }
    ::close(_fd);
    _fd = -1;
}

void EventSpool::sync() {
    if (_fd >= 0 && _dirty) {
        ::fdatasync(_fd);
        _dirty = false;
    }
}

EventSpool::AppendResult EventSpool::append(const vector<EventRecord> &records, string *err) {
    if (records.empty()) {
        return AppendResult::Ok;
    }
    struct statvfs vfs;
    if (statvfs(_opts.dir.data(), &vfs) == 0 &&
        static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize < _opts.minFreeBytes) {
        return AppendResult::NoSpace;
    }
    string buf;
    for (auto &record : records) {
        encode(record, buf);
    }

    string open_err;
    if ((_fd < 0 || _segments.back().bytes >= _opts.segmentBytes) && !openWriter(open_err)) {
        if (err) {
            *err = open_err;
        }
        return AppendResult::IoError;
    }
    auto &seg = _segments.back();
    const char *p = buf.data();
    size_t left = buf.size();
    while (left) {
        auto n = ::write(_fd, p, left);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            const int write_errno = n < 0 ? errno : ENOSPC;
            // All or none: a partial batch would be cut as damage on the next open anyway.
            if (ftruncate(_fd, static_cast<off_t>(seg.bytes)) != 0) {
                WarnL << "EventSpool truncate " << segmentPath(seg.id) << " failed: " << strerror(errno);
            }
            if (err) {
                *err = string("write ") + segmentPath(seg.id) + ": " + strerror(write_errno);
            }
            return write_errno == ENOSPC ? AppendResult::NoSpace : AppendResult::IoError;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }

    if (!_stats.pending) {
        // Everything before was sent: the checkpoint moves to these records, older segments go.
        _checkpoint.segment = seg.id;
        _checkpoint.offset = seg.bytes;
        _checkpoint_records = seg.records;
        _checkpoint_dirty = true;
        while (_segments.front().id != seg.id) {
            retireFront();
        }
    }
    seg.bytes += buf.size();
    seg.records += records.size();
    seg.mtime = time(nullptr);
    _stats.bytes += buf.size();
    _stats.pending += records.size();
    _dirty = true;

    while (_stats.bytes > _opts.maxBytes && _segments.size() > 1) {
        dropOldest(DropReason::MaxSize);
    }
    return AppendResult::Ok;
}

void EventSpool::dropOldest(DropReason reason) {
    auto &front = _segments.front();
    const uint64_t sent = front.id == _checkpoint.segment ? _checkpoint_records : 0;
    const uint64_t dropped = front.records - std::min(front.records, sent);
    if (_segments.size() == 1) {
        closeWriter();
    }
    ::unlink(segmentPath(front.id).data());
    _stats.bytes -= front.bytes;
    _segments.pop_front();

    _checkpoint.segment = _segments.empty() ? _next_id : _segments.front().id;
    _checkpoint.offset = 0;
    _checkpoint_records = 0;
    _checkpoint_dirty = true;
    _stats.pending -= std::min(_stats.pending, dropped);
    _stats.droppedRecords += dropped;
    if (dropped && _on_drop) {
        _on_drop(dropped, reason);
    }
}

uint64_t EventSpool::dropExpired(time_t now) {
    if (!_opts.maxAgeSec) {
        return 0;
    }
    const uint64_t before = _stats.droppedRecords;
    while (!_segments.empty() && _segments.front().mtime + static_cast<time_t>(_opts.maxAgeSec) < now) {
        dropOldest(DropReason::MaxAge);
    }
    return _stats.droppedRecords - before;
}

void EventSpool::read(const Position &from, size_t max, vector<pair<Position, EventRecord>> &out, Position &next) {
    next = from;
    size_t idx = 0;
    while (idx < _segments.size() && _segments[idx].id < from.segment) {
        ++idx;
    }
    if (idx == _segments.size()) {
        return;
    }
    Position pos = from;
    if (_segments[idx].id != from.segment) {
        pos.segment = _segments[idx].id;
        pos.offset = 0;
    }
    int fd = -1;
    uint64_t fd_segment = 0;
    char header[kHeaderSize];
    string payload;
    while (out.size() < max) {
        const auto &seg = _segments[idx];
        if (pos.offset >= seg.bytes) {
            if (idx + 1 == _segments.size()) {
                break;
            }
            ++idx;
            pos.segment = _segments[idx].id;
            pos.offset = 0;
            continue;
        }
        if (fd < 0 || fd_segment != seg.id) {
            if (fd >= 0) {
                ::close(fd);
            }
            fd = ::open(segmentPath(seg.id).data(), O_RDONLY | O_CLOEXEC);
            fd_segment = seg.id;
            if (fd < 0) {
                WarnL << "EventSpool cannot open " << segmentPath(seg.id) << ": " << strerror(errno);
                break;
            }
        }
        EventRecord record;
        bool ok = preadAll(fd, header, kHeaderSize, pos.offset) && getU16(header) == kMagic;
        uint32_t len = ok ? getU32(header + 4) : 0;
        ok = ok && len <= kMaxRecordSize && pos.offset + kHeaderSize + len <= seg.bytes;
        if (ok) {
            payload.resize(len);
            ok = (!len || preadAll(fd, &payload[0], len, pos.offset + kHeaderSize)) &&
                 crcOf(payload.data(), len) == getU32(header + 8) && decodePayload(payload.data(), len, record);
        }
        if (!ok) {
            // Checked by open() or written by this process: only a disk fault gets here.
            WarnL << "EventSpool unreadable record at " << seg.id << ":" << pos.offset << ", skipping the rest of the segment";
            pos.offset = seg.bytes;
            continue;
        }
        out.emplace_back(pos, std::move(record));
        pos.offset += kHeaderSize + len;
    }
    if (fd >= 0) {
        ::close(fd);
    }
    next = pos;
}

void EventSpool::commit(const Position &next, uint64_t records) {
    if (_segments.empty() || next.segment < _checkpoint.segment ||
        (next.segment == _checkpoint.segment && next.offset <= _checkpoint.offset)) {
        return;
    }
    uint64_t passed = 0; // records of the segments left behind that were still pending
    while (_segments.size() > 1 &&
           (_segments.front().id < next.segment ||
            (_segments.front().id == next.segment && next.offset >= _segments.front().bytes))) {
        auto &front = _segments.front();
        const uint64_t sent = front.id == _checkpoint.segment ? _checkpoint_records : 0;
        passed += front.records - std::min(front.records, sent);
        retireFront();
        _checkpoint.segment = _segments.front().id;
        _checkpoint.offset = 0;
        _checkpoint_records = 0;
    }
    if (_segments.front().id == next.segment) {
        _checkpoint_records += records - std::min(records, passed);
        _checkpoint.offset = std::min(next.offset, _segments.front().bytes);
    }
    _checkpoint_records = std::min(_checkpoint_records, _segments.front().records);
    _stats.pending -= std::min(_stats.pending, records);
    _checkpoint_dirty = true;
}

bool EventSpool::flushCheckpoint() {
    if (!_checkpoint_dirty) {
        return true;
    }
    auto tmp = _opts.dir + "/checkpoint.tmp";
    int fd = ::open(tmp.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        WarnL << "EventSpool cannot write " << tmp << ": " << strerror(errno);
        return false;
    }
    char line[64];
    int len = snprintf(line, sizeof(line), "%llu %llu\n", static_cast<unsigned long long>(_checkpoint.segment),
                       static_cast<unsigned long long>(_checkpoint.offset));
    bool ok = ::write(fd, line, static_cast<size_t>(len)) == len && ::fdatasync(fd) == 0;
    ::close(fd);
    ok = ok && ::rename(tmp.data(), (_opts.dir + "/checkpoint").data()) == 0;
    if (!ok) {
        WarnL << "EventSpool cannot write the checkpoint in " << _opts.dir << ": " << strerror(errno);
        return false;
    }
    _checkpoint_dirty = false;
    // Only now: a crash before the rename must find the segments the old checkpoint points into.
    for (auto id : _retired) {
        ::unlink(segmentPath(id).data());
    }
    _retired.clear();
    return true;
}

void EventSpool::retireFront() {
    _stats.bytes -= _segments.front().bytes;
    _retired.push_back(_segments.front().id);
    _segments.pop_front();
}

} // namespace managerkit
