#ifndef LOCAL_TIMEREBUILDER_H_
#define LOCAL_TIMEREBUILDER_H_

#include <functional>
#include "Util/onceToken.h"
#include "TimeRecorderManager.h"
#include "TimeFileDirtyTracker.h"

namespace managerkit {

using TimeFileDirtyEntry = TimeFileDirtyTracker::DirtyEntry;

struct TimeBlockIdentity {
    std::string camera_id;
    std::string stream_id;
    uint64_t start_time = 0;
    std::string segment_path;
};

struct TimeBlockSizePatch {
    std::string camera_id;
    std::string stream_id;
    uint64_t start_time = 0;
    std::string segment_path;
    uint64_t new_size = 0;
};

class TimeRebuilder final {
public:
    using Ptr = std::shared_ptr<TimeRebuilder>;
    using KeepTimeMap = std::unordered_map<std::string /*camera_id/stream_id*/, uint64_t /*keep_time*/>;

    TimeRebuilder(const std::string &src_path);

    ~TimeRebuilder() = default;

    /**
     * Recreate time file with callback
     */
    size_t rebuildTimeLine(const KeepTimeMap &map);

    // Remove only the durable catalog entries whose exact identities are supplied.
    size_t removeTimeBlocks(const std::vector<TimeBlockIdentity> &blocks);
    size_t patchTimeBlocks(const std::vector<TimeBlockSizePatch> &patches);
    size_t verifyTimeBlocks(const std::vector<TimeBlockSizePatch> &patches) const;

    // Rewrite the file keeping the blocks `keep_block` accepts, in start-time order.
    // `keep_block` may modify a block it keeps.
    size_t rebuildTimeLineWithPredicate(const std::function<bool(TimeBlock &)> &keep_block);

    // Invoked on the swap thread after the rebuilt file replaced the source.
    // Not invoked when the swap is discarded because the source changed.
    void setOnSwapped(const std::function<void()> &cb) { _on_swapped = cb; }

private:
    /**
     * Create template file 
     */
    void createTempFile();

    /**
     * Close time file and rename template file to offical name.
     * Guards against concurrent writes (e.g. SD-card import re-opening a past-date file)
     * during the read/rewrite window by discarding the temp file when the dirty version
     * of the source changed since the rebuild started.
     */
    void closeTempFile();

    void closeTempFileAfterActiveSwap();

private:
    TimeRecorder::Ptr _writer;
    TimeRecorder::Ptr _recorder;
    std::string _src_path;
    std::string _device_path;
    std::string _full_path_tmp;
    std::string _full_path;
    // dirty version of the source file captured right before reading it, used by the
    // simple (non-mirrored) swap to detect concurrent writes.
    uint64_t _dirty_version_at_start = 0;
    std::function<void()> _on_swapped;
    // Held from the start of a rebuild until its swap callback finished (or it was discarded).
    std::shared_ptr<toolkit::onceToken> _rebuild_guard;
};

class MultiTimeRebuilder {
public:
    using Ptr = std::shared_ptr<MultiTimeRebuilder>;
    using KeepTimeMap = TimeRebuilder::KeepTimeMap;

    MultiTimeRebuilder(const std::string &src_path);

    ~MultiTimeRebuilder() = default;

    /**
     * Recreate time file according to keep time map.
     * Only files that changed since the last rebuild (dirty) or that still contain
     * expirable blocks are rebuilt; untouched recent files are skipped.
     */
    size_t rebuildTimeLine(const KeepTimeMap &map);

    size_t removeTimeBlocks(const std::vector<TimeBlockIdentity> &blocks);
    size_t patchTimeBlocks(const std::vector<TimeBlockSizePatch> &patches);
    size_t verifyTimeBlocks(const std::vector<TimeBlockSizePatch> &patches) const;

    /**
     * Rewrite in start-time order the files holding a block appended after newer ones (gap fill,
     * SD-card import), today's file included, at most `max_files` of them. Returns how many
     * rebuilds were started.
     */
    size_t sortTimeFiles(size_t max_files);

private:
    /**
     * Open all time files under source path
     */    
    void openTimeFiles(const std::string &src_path);

    // Shared by every rebuild: `select(stamp, path, entry)` picks the files (entry is null when the
    // file is neither dirty nor unsorted), `keep` the blocks. A rebuilt file loses its dirty and
    // unsorted marks once its swap ran, and marks of files that no longer exist are dropped.
    size_t rebuildFiles(const std::function<bool(uint64_t, const std::string &,
                                                 const TimeFileDirtyEntry *)> &select,
                        const std::function<bool(const TimeBlock &)> &keep,
                        size_t max_files = 0);

private:
    std::string _src_path;
    std::map<uint64_t, std::string> _timefiles_map;
};

} // namespace managerkit

#endif //LOCAL_TIMEREBUILDER_H_
