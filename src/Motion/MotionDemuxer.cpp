#ifdef ENABLE_MOTION

#include "MotionDemuxer.h"

#include "Util/logger.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

using namespace std;
using namespace toolkit;

namespace mediakit {

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string inferIdxPath(const std::string &blk_path) {
    // Replace trailing ".mblk" with ".idx", fallback to appending ".idx".
    const std::string ext = ".mblk";
    if (blk_path.size() > ext.size() &&
        blk_path.compare(blk_path.size() - ext.size(), ext.size(), ext) == 0) {
        return blk_path.substr(0, blk_path.size() - ext.size()) + ".idx";
    }
    return blk_path + ".idx";
}

// ── MotionDemuxer ─────────────────────────────────────────────────────────────

bool MotionDemuxer::open(const std::string &blk_path, const std::string &idx_path) {
    close();

    const std::string real_idx = idx_path.empty() ? inferIdxPath(blk_path) : idx_path;

    if (!_storage.open(blk_path, real_idx)) {
        WarnL << "MotionDemuxer: cannot open files: " << blk_path;
        return false;
    }

    _cursor     = 0;
    _data_start = 0;
    _has_meta   = false;
    _meta       = {};
    _open       = true;

    // If the first index entry is a Meta block, decode it and advance the
    // sequential read cursor past it so callers never see Meta blocks.
    if (entryCount() > 0) {
        MotionIndexEntry first_entry{};
        _storage.getIndexEntry(0, first_entry);
        if (static_cast<MotionBlockType>(first_entry.type) == MotionBlockType::Meta) {
            toolkit::BlockHeader meta_hdr{};
            std::vector<uint8_t> ext_vec, payload_vec;
            bool meta_eof = false;
            if (_storage.readBlockAt(0, meta_hdr, ext_vec, payload_vec, meta_eof)
                    && ext_vec.size() >= sizeof(MotionMetaExtHeader)) {
                MotionMetaExtHeader ext{};
                std::memcpy(&ext, ext_vec.data(), sizeof(ext));
                const uint8_t *p   = payload_vec.data();
                const size_t   n   = payload_vec.size();
                size_t off = 0;
                if (off + ext.device_id_len <= n) { _meta.device_id.assign(p + off, p + off + ext.device_id_len); off += ext.device_id_len; }
                if (off + ext.stream_id_len <= n) { _meta.stream_id.assign(p + off, p + off + ext.stream_id_len); off += ext.stream_id_len; }
                if (off + ext.roi_mask_len  <= n) { _meta.roi_mask.assign( p + off, p + off + ext.roi_mask_len);  }
                _meta.rows = ext.rows;
                _meta.cols = ext.cols;
                _has_meta = true;
            }
            _data_start = 1;
            _cursor     = 1;
            if (entryCount() > 1) _storage.seekToEntry(1);
        }
    }

    DebugL << "MotionDemuxer: opened " << blk_path << " (" << entryCount() << " entries"
           << (_has_meta ? ", has meta: " + _meta.device_id + "/" + _meta.stream_id : "") << ")";
    return true;
}

void MotionDemuxer::close() {
    _storage.close();
    _cursor     = 0;
    _data_start = 0;
    _has_meta   = false;
    _meta       = {};
    _open       = false;
}

uint64_t MotionDemuxer::getFirstStamp() const {
    if (!_open || entryCount() == 0) return 0;
    MotionIndexEntry e{};
    _storage.getIndexEntry(0, e);
    return e.stamp;
}

uint64_t MotionDemuxer::getLastStamp() const {
    const size_t n = entryCount();
    if (!_open || n == 0) return 0;
    MotionIndexEntry e{};
    _storage.getIndexEntry(n - 1, e);
    return e.stamp;
}

size_t MotionDemuxer::lowerBound(uint64_t target_ms) const {
    size_t lo = 0, hi = entryCount();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        MotionIndexEntry e{};
        _storage.getIndexEntry(mid, e);
        if (e.stamp < target_ms) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int64_t MotionDemuxer::seekTo(uint64_t stamp_ms) {
    if (!_open) return -1;

    const size_t pos = std::max(_data_start, lowerBound(stamp_ms));
    if (pos >= entryCount()) {
        _cursor = entryCount();
        return -1;
    }
    if (!_storage.seekToEntry(pos)) return -1;
    _cursor = pos;

    MotionIndexEntry e{};
    _storage.getIndexEntry(pos, e);
    return static_cast<int64_t>(e.stamp);
}

bool MotionDemuxer::readBlock(MotionBlock &out, bool &eof) {
    eof = false;
    if (!_open) { eof = true; return false; }

    // Skip any Meta blocks at any position (there may be more than one if the
    // stream config changed mid-file).  We loop so callers always get a data block.
    while (_cursor < entryCount()) {
        MotionIndexEntry e{};
        if (_storage.getIndexEntry(_cursor, e)
                && static_cast<MotionBlockType>(e.type) == MotionBlockType::Meta) {
            // Advance without reading the payload into *out.
            if (!_storage.seekToEntry(_cursor + 1)) {
                eof = true; return false;
            }
            ++_cursor;
            continue;
        }
        break;
    }

    if (_cursor >= entryCount()) { eof = true; return false; }

    if (!_storage.readNextBlock(out.header, out.ext_header, out.payload, eof)) return false;
    ++_cursor;

    // Verify CRC when stored value is non-zero (zero = legacy block written before CRC was implemented).
    if (out.header.crc != 0) {
        const uint32_t computed = motionCrc32(
            out.ext_header.data(), out.ext_header.size(),
            out.payload.data(),    out.payload.size());
        if (computed != out.header.crc) {
            WarnL << "MotionDemuxer: CRC mismatch at stamp=" << out.header.stamp
                  << " stored=" << out.header.crc << " computed=" << computed;
        }
    }
    return true;
}

std::vector<MotionInterval> MotionDemuxer::getMotionIntervals(uint64_t from_ms, uint64_t to_ms) {
    std::vector<MotionInterval> result;
    if (!_open) return result;

    const size_t n = entryCount();
    if (n == 0) return result;

    const size_t saved_cursor = _cursor;

    const size_t start_pos = lowerBound(from_ms);
    for (size_t i = start_pos; i < n; ++i) {
        MotionIndexEntry entry{};
        if (!_storage.getIndexEntry(i, entry)) break;
        if (entry.stamp > to_ms) break;
        if (static_cast<MotionBlockType>(entry.type) != MotionBlockType::Summary) continue;

        toolkit::BlockHeader hdr{};
        std::vector<uint8_t> ext_vec, payload_vec;
        bool eof = false;
        if (!_storage.readBlockAt(i, hdr, ext_vec, payload_vec, eof)) {
            WarnL << "MotionDemuxer: failed to read summary at index " << i;
            continue;
        }
        if (ext_vec.size() < sizeof(MotionSummaryExtHeader)) continue;

        MotionSummaryExtHeader ext{};
        std::memcpy(&ext, ext_vec.data(), sizeof(ext));

        MotionInterval iv;
        iv.start_ms     = hdr.stamp;
        iv.end_ms       = ext.end_stamp;
        iv.rows         = ext.rows;
        iv.cols         = ext.cols;
        iv.active_cells = ext.active_cells;
        iv.bitmap       = std::move(payload_vec);
        result.push_back(std::move(iv));
    }

    // Restore sequential read position (readBlockAt uses random seek internally).
    if (saved_cursor < n) _storage.seekToEntry(saved_cursor);
    _cursor = saved_cursor;

    return result;
}

// ── MultiMotionDemuxer ────────────────────────────────────────────────────────

int MultiMotionDemuxer::openDirectory(const std::string &base_path) {
    int count = 0;
    DIR *dir = opendir(base_path.c_str());
    if (!dir) {
        WarnL << "MultiMotionDemuxer: cannot open directory: " << base_path;
        return 0;
    }

    const std::string suffix = ".mblk";
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.size() <= suffix.size()) continue;
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

        const std::string full_path = base_path + (base_path.back() == '/' ? "" : "/") + name;
        if (addFile(full_path)) ++count;
    }
    closedir(dir);
    return count;
}

bool MultiMotionDemuxer::addFile(const std::string &blk_path, const std::string &idx_path) {
    auto d = std::make_shared<MotionDemuxer>();
    if (!d->open(blk_path, idx_path)) return false;

    const uint64_t first = d->getFirstStamp();
    _demuxers.emplace(first, std::move(d));
    _it = _demuxers.end();
    return true;
}

void MultiMotionDemuxer::closeAll() {
    _demuxers.clear();
    _it = _demuxers.end();
}

uint64_t MultiMotionDemuxer::getFirstStamp() const {
    if (_demuxers.empty()) return 0;
    return _demuxers.begin()->second->getFirstStamp();
}

uint64_t MultiMotionDemuxer::getLastStamp() const {
    if (_demuxers.empty()) return 0;
    return _demuxers.rbegin()->second->getLastStamp();
}

int64_t MultiMotionDemuxer::seekTo(uint64_t stamp_ms) {
    if (_demuxers.empty()) return -1;

    // Find the last file whose first stamp <= stamp_ms.
    auto it = _demuxers.upper_bound(stamp_ms);
    if (it != _demuxers.begin()) --it;

    _it = it;
    const int64_t actual = _it->second->seekTo(stamp_ms);
    if (actual < 0) {
        // Stamp is past the end of this file; advance to the next one if available.
        ++_it;
        if (_it != _demuxers.end()) {
            _it->second->seekTo(0); // beginning of next file
            return static_cast<int64_t>(_it->second->getFirstStamp());
        }
        _it = _demuxers.end();
        return -1;
    }
    return actual;
}

bool MultiMotionDemuxer::readBlock(MotionBlock &out, bool &eof) {
    eof = false;
    if (_it == _demuxers.end()) { eof = true; return false; }

    bool local_eof = false;
    if (_it->second->readBlock(out, local_eof)) return true;

    if (!local_eof) { eof = true; return false; } // real error

    // Advance to next file.
    ++_it;
    if (_it == _demuxers.end()) { eof = true; return false; }

    _it->second->seekTo(0); // read from start of next file
    return _it->second->readBlock(out, eof);
}

std::vector<MotionInterval> MultiMotionDemuxer::getMotionIntervals(uint64_t from_ms,
                                                                     uint64_t to_ms) {
    std::vector<MotionInterval> result;
    for (auto &it : _demuxers) {
        auto &d = it.second;
        if (d->getLastStamp() < from_ms) continue;
        if (d->getFirstStamp() > to_ms) break;
        auto ivs = d->getMotionIntervals(from_ms, to_ms);
        result.insert(result.end(), ivs.begin(), ivs.end());
    }
    return result;
}

} // namespace mediakit

#endif // ENABLE_MOTION