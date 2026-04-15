#ifndef MOTION_DEMUXER_H
#define MOTION_DEMUXER_H

#ifdef ENABLE_MOTION

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MotionBlock.h"
#include "Util/BlockStorageEngine.h"

namespace mediakit {

/**
 * A time interval during which motion was confirmed, as recorded by a Summary block.
 */
struct MotionInterval {
    uint64_t             start_ms     = 0;
    uint64_t             end_ms       = 0;
    uint16_t             rows         = 0;
    uint16_t             cols         = 0;
    uint16_t             active_cells = 0;
    std::vector<uint8_t> bitmap;       // bit-packed motion map (rows*cols bits, MSB-first)
};

/**
 * Reads a single day's motion block file (.mblk) with its mmap'd index (.idx).
 *
 * Provides:
 *   - Sequential block reading  (readBlock)
 *   - O(log n) stamp-based seek (seekTo)
 *   - Motion interval query     (getMotionIntervals) — scans Summary blocks only
 *
 * Thread safety: NOT thread-safe. Use from a single thread or protect externally.
 */
class MotionDemuxer {
public:
    using Ptr = std::shared_ptr<MotionDemuxer>;

    MotionDemuxer() = default;
    ~MotionDemuxer() { close(); }

    /**
     * Open a .mblk file and its corresponding index.
     * If idx_path is empty, it is derived by replacing ".mblk" with ".idx".
     * @return true if both files opened successfully.
     */
    bool open(const std::string &blk_path, const std::string &idx_path = "");

    void close();

    bool isOpen() const { return _open; }

    /** Stamp of the first index entry (ms), 0 if empty. */
    uint64_t getFirstStamp() const;

    /** Stamp of the last index entry (ms), 0 if empty. */
    uint64_t getLastStamp() const;

    /** Total number of index entries (includes the Meta block if present). */
    size_t entryCount() const { return _storage.entryCount(); }

    /** True when the file contains a Meta block at index 0. */
    bool             hasMeta() const { return _has_meta; }
    const MotionMeta &getMeta() const { return _meta; }

    /**
     * Position the read cursor to the first block whose stamp >= stamp_ms.
     * Uses binary search on the index (O(log n)).
     * @return The stamp of the block positioned at, or -1 on error / empty file.
     */
    int64_t seekTo(uint64_t stamp_ms);

    /**
     * Read the next block at the current cursor position.
     * @param out  Populated with header, ext_header bytes, and payload bytes.
     * @param eof  Set to true when all blocks have been read.
     * @return true if a block was read successfully.
     */
    bool readBlock(MotionBlock &out, bool &eof);

    /**
     * Collect all time intervals with confirmed motion in the range [from_ms, to_ms].
     * Reads from Summary blocks only; Event blocks are skipped via the index.
     * The current read cursor is NOT affected by this call.
     *
     * @param from_ms  Inclusive lower bound (default: beginning of file).
     * @param to_ms    Inclusive upper bound (default: end of file).
     */
    std::vector<MotionInterval> getMotionIntervals(uint64_t from_ms = 0,
                                                    uint64_t to_ms   = UINT64_MAX);

private:
    /** First index position with stamp >= target_ms. Returns entryCount() if none. */
    size_t lowerBound(uint64_t target_ms) const;

private:
    toolkit::BlockStorageReader<MotionIndexEntry> _storage;
    size_t     _cursor     = 0;     // index position of next readBlock() call
    size_t     _data_start = 0;     // 1 when Meta block occupies index entry 0
    bool       _open       = false;
    bool       _has_meta   = false;
    MotionMeta _meta;
};

/**
 * Demuxer that aggregates all .mblk/.idx files found under a base directory.
 * Files must follow the naming convention YYYYMMDD.mblk / YYYYMMDD.idx.
 *
 * Provides the same interface as MotionDemuxer but spans multiple day-files.
 */
class MultiMotionDemuxer {
public:
    using Ptr = std::shared_ptr<MultiMotionDemuxer>;

    MultiMotionDemuxer() = default;
    ~MultiMotionDemuxer() = default;

    /**
     * Scan base_path for all YYYYMMDD.mblk files and open them.
     * @return Number of files successfully opened.
     */
    int openDir(const std::string &base_path);

    /**
     * Add a single .mblk file explicitly.
     * @return true if opened successfully.
     */
    bool addFile(const std::string &blk_path, const std::string &idx_path = "");

    void closeAll();

    bool isEmpty() const { return _demuxers.empty(); }

    /**
     * Returns the MotionMeta from the first file, or nullptr if no file has a Meta block.
     * Valid for the lifetime of this MultiMotionDemuxer.
     */
    const MotionMeta *getMeta() const {
        if (_demuxers.empty()) return nullptr;
        const auto &d = _demuxers.begin()->second;
        return d->hasMeta() ? &d->getMeta() : nullptr;
    }

    uint64_t getFirstStamp() const;
    uint64_t getLastStamp() const;

    /**
     * Seek across all files to the first block at-or-after stamp_ms.
     * @return Stamp actually seeked to, or -1 if no data.
     */
    int64_t seekTo(uint64_t stamp_ms);

    /** Read the next block, crossing file boundaries automatically. */
    bool readBlock(MotionBlock &out, bool &eof);

    /** Aggregate motion intervals from all files in [from_ms, to_ms]. */
    std::vector<MotionInterval> getMotionIntervals(uint64_t from_ms = 0,
                                                    uint64_t to_ms   = UINT64_MAX);

private:
    // Keyed by first_stamp of each file for ordered traversal.
    std::map<uint64_t, MotionDemuxer::Ptr>           _demuxers;
    std::map<uint64_t, MotionDemuxer::Ptr>::iterator _it{_demuxers.end()};
};

} // namespace mediakit

#endif // ENABLE_MOTION

#endif // MOTION_DEMUXER_H
