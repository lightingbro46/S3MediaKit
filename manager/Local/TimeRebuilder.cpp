#include <algorithm>
#include <set>
#include <vector>
#include "TimeRebuilder.h"
#include "TimeFileDirtyTracker.h"
#include "Common/Parser.h"
#include "TimeDemuxer.h"
#include "TimeFileAccessManager.h"
#include "Common/StrUtil.h"
#include "Local/StatisticRecorder.h"
#include "Util/base64.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// A rebuild holds its file until the swap ran, which waits for readers; a concurrent rebuild of
// the same file waits this long before giving up for this cycle. Kept short: playback readers can
// hold the first swap back for a long time, and this blocks a tier or storage pool thread.
static constexpr int kRebuildWaitMs = 2000;

//////////////////////////////TimeRebuilder////////////////////////////////

TimeRebuilder::TimeRebuilder(const string &src_path) : _src_path(src_path) {
    if (_src_path.empty() || !end_with(_src_path, ".s3db")) {
        throw std::invalid_argument("Source path must be a .s3db file");
    }
}

static TimeRecorder::Ptr getTimeRecorder(const string &device_path) {
    auto record_path = File::parentDir(device_path);
    auto device_id = findSubString(device_path.data() + record_path.size(), nullptr, "/");
    return TimeRecorderManager::Instance().getRecorder(device_id);
}

void TimeRebuilder::createTempFile() {
    string parent_path = File::parentDir(_src_path);
    string filename = findSubString(_src_path.data() + parent_path.size(), nullptr, nullptr);
    string temp_filename = "." + filename;
    _full_path_tmp = parent_path + temp_filename;
    _full_path = _src_path;
    // File::parentDir() keeps a trailing '/', but TimeRecorder marks files dirty
    // under the device path without it; a mismatched key made every version
    // lookup return 0 and silently disabled the concurrent-write check.
    _device_path = parent_path;
    while (_device_path.size() > 1 && _device_path.back() == '/')
        _device_path.pop_back();

    // Capture the dirty version before reading so the simple swap can detect any block
    // written to the source file while the rebuild is running.
    _dirty_version_at_start = TimeFileDirtyTracker::Instance().currentVersion(_device_path, _src_path);

    // The recorder of this device tells us dynamically whether the file is currently
    // being appended to (live or SD-card import). This replaces the old date-based
    // "in use" heuristic: any file with an open muxer needs the mirrored swap path.
    _recorder = getTimeRecorder(parent_path);

    // delete tmp file
    if (File::fileExist(_full_path_tmp)) {
        TraceL << "Remove old template file: " << _full_path_tmp;
        File::delete_file(_full_path_tmp);
    }

    try {
        _writer = std::make_shared<TimeRecorder>(_full_path_tmp);
        TraceL << "Open tmp time file: " << _full_path_tmp;
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }

}

static void renameTimeFile(const string &db_path_tmp, const string &db_path) {
    // rename file
    TraceL << "Rename time file: " << db_path_tmp << "-->" << db_path;
    rename(db_path_tmp.data(), db_path.data());
}

static void renameMakerFile(const string &db_path_tmp, const string &db_path) {
    string maker_path_tmp(db_path_tmp);
    replace(maker_path_tmp, ".s3db", ".idx");
    string maker_path(db_path);
    replace(maker_path, ".s3db", ".idx");
    // rename file
    TraceL << "Rename time maker file: " << maker_path_tmp << "-->" << maker_path;
    rename(maker_path_tmp.data(), maker_path.data());
}

static void deleteTimeFile(const string &db_path) {
    // delete file
    TraceL << "Delete time file: " << db_path;
    File::delete_file(db_path);
} 

static void deleteMakerFile(const string &db_path) {
    string maker_path(db_path);
    replace(maker_path, ".s3db", ".idx");
    // delete file
    TraceL << "Delete time maker file: " << maker_path;
    File::delete_file(maker_path);
}

// Replace the source with the rebuilt file. rename() swaps the name atomically,
// so a directory scan never observes the day's file missing; deleting first and
// renaming afterwards left a window where queries silently skipped that day.
static void replaceTimeFiles(const string &db_path_tmp, const string &db_path) {
    if (db_path_tmp.empty())
        return;
    if (File::fileSize(db_path_tmp) == 0) {
        // Nothing left to keep: drop both the empty rebuild and the source.
        deleteTimeFile(db_path_tmp);
        deleteMakerFile(db_path_tmp);
        deleteTimeFile(db_path);
        deleteMakerFile(db_path);
        return;
    }
    renameTimeFile(db_path_tmp, db_path);
    string maker_path_tmp(db_path_tmp);
    replace(maker_path_tmp, ".s3db", ".idx");
    if (File::fileExist(maker_path_tmp)) {
        renameMakerFile(db_path_tmp, db_path);
    } else {
        // A stale index would point at offsets of the replaced file.
        deleteMakerFile(db_path);
    }
}

void TimeRebuilder::closeTempFile() {
    // close writer to close tmp file
    auto writer = _writer;
    _writer.reset();
    if (writer) {
        writer->closeNow();
    }

    // rename file with original name in background thread
    auto full_path_tmp = _full_path_tmp;;
    auto full_path = _full_path;
    auto src_path = _src_path;
    auto device_path = _device_path;
    auto version_at_start = _dirty_version_at_start;
    auto on_swapped = _on_swapped;
    auto rebuild_guard = std::move(_rebuild_guard);
    TimeFileAccessManager::Instance().beginPendingSwap(full_path);
    TimeFileAccessManager::Instance().finalizeSwapWhenReadableIdle(full_path, [full_path_tmp, full_path, src_path, device_path, version_at_start, on_swapped, rebuild_guard]() {
        // Abort the destructive swap if the source file was written to while we were
        // rebuilding it (e.g. SD-card import re-opened this past-date file). Deleting
        // the source now would drop those blocks; keep it dirty for the next cycle.
        uint64_t current_version = TimeFileDirtyTracker::Instance().currentVersion(device_path, src_path);
        if (current_version != version_at_start) {
            WarnL << "Source time file changed during rebuild, discard temp file: " << src_path;
            if (!full_path_tmp.empty()) {
                deleteTimeFile(full_path_tmp);
                deleteMakerFile(full_path_tmp);
            }
            return;
        }
        replaceTimeFiles(full_path_tmp, full_path);
        if (on_swapped)
            on_swapped();
    });
}

void TimeRebuilder::closeTempFileAfterActiveSwap() {
    auto writer = _writer;
    auto recorder = _recorder;
    _writer.reset();

    auto full_path_tmp = _full_path_tmp;
    auto full_path = _full_path;
    auto src_path = _src_path;
    auto on_swapped = _on_swapped;
    auto rebuild_guard = std::move(_rebuild_guard);
    TimeFileAccessManager::Instance().beginPendingSwap(full_path);
    TimeFileAccessManager::Instance().finalizeSwapWhenReadableIdle(full_path, [writer, recorder, full_path_tmp, full_path, src_path, on_swapped, rebuild_guard]() {
        auto swap_file = [full_path_tmp, full_path]() {
            replaceTimeFiles(full_path_tmp, full_path);
        };

        if (recorder) {
            recorder->finishRebuildSwap(src_path, writer, swap_file);
        } else if (writer) {
            writer->closeNow();
            swap_file();
        } else {
            swap_file();
        }
        if (on_swapped)
            on_swapped();
    });
}

struct ArchivedChanges {
    size_t archived_size = 0;
    size_t archived_count = 0;
    uint64_t archived_start_time = 0;
    uint64_t archived_end_time = 0;
};
using StreamArchivedChanges = unordered_map<string /*stream_id*/, ArchivedChanges>;
using CameraArchivedChanges = unordered_map<string /*camera_id*/, StreamArchivedChanges>;

static void addTempArchivedChanges(CameraArchivedChanges &tmp_map, const TimeBlock &block) {
    auto &changes = tmp_map[block.app()][block.stream()];
    changes.archived_size += block.file_size();
    changes.archived_count++;
    changes.archived_start_time = changes.archived_start_time > 0 ? min(changes.archived_start_time, block.start_time()) : block.start_time();
    changes.archived_end_time = max(changes.archived_end_time, block.start_time() + block.time_len());
}

static void commitArchivedChanges(CameraArchivedChanges &camera_changes) {
    for (const auto &camera_change : camera_changes) {
        const auto &camera_id = camera_change.first;
        const auto &stream_changes = camera_change.second;
        for (const auto &stream_change : stream_changes) {
            const auto &stream_id = stream_change.first;
            const auto &changes = stream_change.second;
            StatisticRecorder::Instance().addArchiveSize(camera_id, stream_id, changes.archived_count, changes.archived_size, changes.archived_start_time, changes.archived_end_time, false);
        }
    }
}

static function<bool(const TimeBlock &)> keepByThreshold(const TimeRebuilder::KeepTimeMap &map) {
    return [&map](const TimeBlock &block) -> bool {
        string key = (StrPrinter << block.app() << "/" << block.stream()); 
        auto it = map.find(key);
        return it != map.end() && block.start_time() >= it->second;
    };
}

static function<bool(const TimeBlock &)> keepUnlessListed(const vector<TimeBlockIdentity> &blocks) {
    return [&blocks](const TimeBlock &block) -> bool {
        for (const auto &identity : blocks) {
            if (identity.camera_id != block.app() ||
                identity.stream_id != block.stream() ||
                identity.start_time != block.start_time())
                continue;
            if (!identity.segment_path.empty() && !block.file_path().empty()) {
                const string decoded = decodeBase64(block.file_path());
                const string suffix = identity.segment_path + ".mp4";
                if (decoded.size() < suffix.size() ||
                    decoded.compare(decoded.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
            }
            return false;
        }
        return true;
    };
}

size_t TimeRebuilder::rebuildTimeLine(const KeepTimeMap &map) {
    return rebuildTimeLineWithPredicate(keepByThreshold(map));
}

size_t TimeRebuilder::removeTimeBlocks(const vector<TimeBlockIdentity> &blocks) {
    return rebuildTimeLineWithPredicate(keepUnlessListed(blocks));
}

size_t TimeRebuilder::patchTimeBlocks(const vector<TimeBlockSizePatch> &patches) {
    size_t patched = 0;
    auto patch_block = [&patches, &patched](TimeBlock &block) -> bool {
        for (const auto &patch : patches) {
            if (patch.camera_id != block.app() || patch.stream_id != block.stream() ||
                patch.start_time != block.start_time())
                continue;
            if (!patch.segment_path.empty() && !block.file_path().empty()) {
                const string decoded = decodeBase64(block.file_path());
                const string suffix = patch.segment_path + ".mp4";
                if (decoded.size() < suffix.size() ||
                    decoded.compare(decoded.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
            }
            if (block.file_size() != patch.new_size) {
                block.set_file_size(patch.new_size);
                ++patched;
            }
            break;
        }
        return true;
    };
    rebuildTimeLineWithPredicate(patch_block);
    return patched;
}

size_t TimeRebuilder::verifyTimeBlocks(const vector<TimeBlockSizePatch> &patches) const {
    size_t verified = 0;
    TimeDemuxer demuxer;
    demuxer.openFile(_src_path);
    bool eof = false;
    while (!eof) {
        TimeBlock block;
        demuxer.readBlock(block, eof);
        if (eof) break;
        for (const auto &patch : patches) {
            if (patch.camera_id != block.app() || patch.stream_id != block.stream() ||
                patch.start_time != block.start_time() || patch.new_size != block.file_size())
                continue;
            if (!patch.segment_path.empty() && !block.file_path().empty()) {
                const string decoded = decodeBase64(block.file_path());
                const string suffix = patch.segment_path + ".mp4";
                if (decoded.size() < suffix.size() ||
                    decoded.compare(decoded.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
            }
            ++verified;
            break;
        }
    }
    return verified;
}

size_t TimeRebuilder::rebuildTimeLineWithPredicate(const function<bool(TimeBlock &)> &keep_predicate) {
    Ticker ticket;
    size_t removed_bytes = 0;
    auto keep_block = [&removed_bytes, &keep_predicate](TimeBlock &block) -> bool {
        if (keep_predicate(block))
            return true;
        removed_bytes += block.file_size();
        TraceL << "Remove time block: " << block.app() << "/" << block.stream()
               << " " << block.start_time();
        return false;
    };

    // step 0: one rebuild per file; a second one would delete this one's temp file before its swap.
    _rebuild_guard = TimeFileAccessManager::Instance().tryBeginRebuild(_src_path, kRebuildWaitMs);
    if (!_rebuild_guard) {
        WarnL << "Skip time file rebuild, another rebuild of the file is still running: " << _src_path;
        return 0;
    }

    // step 1: delete old template file and create writer
    createTempFile();
    if (!_writer) {
        WarnL << "Failed to create TimeRecorder for tmp file: " << _full_path_tmp;
        return 0;
    }
    

    // step 2: if the file is currently being appended to (live or SD-card import),
    // start capturing new blocks and freeze the read snapshot. This is decided
    // dynamically by the recorder, not by the file date.
    bool capture_started = false;
    uint64_t snapshot_size = 0;
    capture_started = _recorder && _recorder->beginRebuildCapture(_src_path, snapshot_size);
    if (!capture_started) {
        TraceL << "Time file is not active in recorder, rebuild as inactive file: " << _src_path;
    }

    CameraArchivedChanges tmp_changes;
    vector<TimeBlock> keep_blocks;
    {
        // step 3: read all block in current file with filter, then sort before writing tmp.
        auto demuxer = make_shared<TimeDemuxer>();
        if (!capture_started || snapshot_size > 0) {
            demuxer->openFile(_src_path);
            if (capture_started) {
                demuxer->setReadLimit(snapshot_size);
            }
            bool eof = false;
            while (!eof) {
                TimeBlock block;
                demuxer->readBlock(block, eof);
                if (eof) {
                    break;
                }
                if (keep_block(block)) {
                    keep_blocks.emplace_back(std::move(block));
                } else {
                    addTempArchivedChanges(tmp_changes, block);
                }
            }
        }
    }

    stable_sort(keep_blocks.begin(), keep_blocks.end(), [](const TimeBlock &a, const TimeBlock &b) {
        return a.start_time() < b.start_time();
    });
    for (const auto &block : keep_blocks) {
        _writer->inputBlock(block);
    }

    if (capture_started) {
        // step 4: drain captured blocks to tmp and mirror future live blocks to tmp until rename is safe.
        auto writeMemoryBlock = [&](const string &buf) {
            auto demuxer = std::make_shared<TimeMemoryDemuxer>(buf);
            vector<TimeBlock> captured_blocks;
            bool eof = false;
            while (!eof) {
                TimeBlock block;
                demuxer->readBlock(block, eof);
                if (eof) {
                    break;
                }
                if (keep_block(block)) {
                    captured_blocks.emplace_back(std::move(block));
                } else {
                    addTempArchivedChanges(tmp_changes, block);
                }
            }
            stable_sort(captured_blocks.begin(), captured_blocks.end(), [](const TimeBlock &a, const TimeBlock &b) {
                return a.start_time() < b.start_time();
            });
            for (const auto &block : captured_blocks) {
                _writer->inputBlock(block);
            }
        };

        if (_recorder->drainRebuildCaptureAndStartMirror(_src_path, _writer, writeMemoryBlock)) {
            closeTempFileAfterActiveSwap();
        } else {
            WarnL << "Failed to start live mirror for active time file, close tmp without active swap: " << _src_path;
            closeTempFile();
        }
        commitArchivedChanges(tmp_changes);
    } else {
        // step 4: close file and rename filename when there is no reader
        closeTempFile(); 
        commitArchivedChanges(tmp_changes);
    }

    TraceL << "Recreated time file: " << _src_path << ". Removed bytes: " << format_bytes_human_readable(removed_bytes)
           << ". Elapsed: " << format_duration_verbose(ticket.elapsedTime());

    return removed_bytes;    
}

//////////////////////////////MultiTimeRebuilder////////////////////////////////

MultiTimeRebuilder::MultiTimeRebuilder(const std::string &src_path) : _src_path(src_path) {
    openTimeFiles(src_path);
}

void MultiTimeRebuilder::openTimeFiles(const string &src_path) {
    File::scanDir(_src_path, [&](const string &path, bool isDir) {
        if (!isDir && end_with(path, ".s3db")) {
            auto filename = findSubString(path.data() + _src_path.size(), "/", ".s3db");
            auto stamp = StrTimeUtils::getTsFromDateStr(filename);
            _timefiles_map.emplace(stamp, path);
        }
        return true;
    });
}

static uint64_t findMinKeepTime(const string &src_path, const TimeRebuilder::KeepTimeMap &map) {
    uint64_t min_keep_time = 0;
    auto record_path = File::parentDir(src_path);
    auto device_id = findSubString(src_path.data() + record_path.size(), nullptr, nullptr);

    File::scanDir(src_path, [&](const string &path, bool isDir) {
        if (isDir) {
            auto stream_id = findSubString(path.data() + src_path.size(), "/", nullptr);
            string key = (StrPrinter << device_id << "/" << stream_id);
            auto it = map.find(key);
            if (it != map.end()) {
                if (min_keep_time == 0 || min_keep_time < it->second) {
                    min_keep_time = it->second;
                }
            }
        }
        return true;
    }, true);

    if (min_keep_time == 0) {
        min_keep_time = time(nullptr);
    }
    return min_keep_time;
}

size_t MultiTimeRebuilder::rebuildFiles(
        const function<bool(uint64_t, const string &, const TimeFileDirtyEntry *)> &select,
        const function<bool(const TimeBlock &)> &keep,
        size_t max_files) {
    size_t total_removed_bytes = 0;

    // Snapshot the files that changed (or went out of order) since the last rebuild.
    auto &tracker = TimeFileDirtyTracker::Instance();
    unordered_map<string, TimeFileDirtyEntry> marked;
    for (const auto &entry : tracker.snapshot(_src_path)) {
        // A rebuild that kept nothing deleted the file; its marks are of no use any more.
        if (!File::fileExist(entry.file_path.data())) {
            tracker.clearRebuilt(_src_path, entry);
            continue;
        }
        marked.emplace(entry.file_path, entry);
    }

    size_t started = 0;
    for (const auto &it : _timefiles_map) {
        const auto &stamp = it.first;
        const auto &timefile = it.second;

        auto marked_it = marked.find(timefile);
        const TimeFileDirtyEntry *entry = marked_it == marked.end() ? nullptr : &marked_it->second;
        if (!select(stamp, timefile, entry)) {
            TraceL << "Skip rebuild time file: " << timefile;
            continue;
        }
        if (max_files && started >= max_files) {
            break;
        }
        ++started;

        try {
            TraceL << "Rebuild time file: " << timefile << (entry ? " (dirty)" : "");
            auto rebuilder = std::make_shared<TimeRebuilder>(timefile);
            if (entry) {
                // Clear the marks only after the rebuilt file replaced the source, and only
                // when nothing was written since the snapshot. Clearing earlier made the
                // pending swap see a version change and discard itself.
                const string device_path = _src_path;
                const TimeFileDirtyEntry snapshot_entry = *entry;
                rebuilder->setOnSwapped([device_path, snapshot_entry]() {
                    TimeFileDirtyTracker::Instance().clearRebuilt(device_path, snapshot_entry);
                });
            }
            total_removed_bytes += rebuilder->rebuildTimeLineWithPredicate(keep);
        } catch (std::exception &ex) {
            WarnL << "Failed to rebuild " << timefile << ": " << ex.what();
        }
    }
    return total_removed_bytes;
}

size_t MultiTimeRebuilder::rebuildTimeLine(const KeepTimeMap &map) {
    if (_timefiles_map.empty()) {
        WarnL << "No time files found under path: " << _src_path;
        return 0;
    }

    // The latest keep threshold among all streams: files starting after it hold only
    // blocks that must be kept, so they never need trimming for expiry.
    auto latest_keep_time = findMinKeepTime(_src_path, map);
    TraceL << "Rebuild time files older than: " << getTimeStr("%Y-%m-%d %H:%M:%S", latest_keep_time);

    // Skip files that are entirely within the keep window and were not modified.
    return rebuildFiles(
        [latest_keep_time](uint64_t stamp, const string &, const TimeFileDirtyEntry *entry) {
            return entry != nullptr || stamp < latest_keep_time;
        },
        keepByThreshold(map));
}

size_t MultiTimeRebuilder::removeTimeBlocks(const vector<TimeBlockIdentity> &blocks) {
    if (blocks.empty())
        return 0;
    // Daily files hold blocks by the local date of their start time; only rewrite
    // the files that can contain a requested block. Files whose name is not a date
    // (stamp 0) are always considered.
    std::set<uint64_t> block_days;
    for (const auto &identity : blocks)
        block_days.insert(StampUtils::getStartOfDay(identity.start_time));
    return rebuildFiles(
        [&block_days](uint64_t stamp, const string &, const TimeFileDirtyEntry *) {
            return stamp == 0 || block_days.count(stamp) != 0;
        },
        keepUnlessListed(blocks));
}

size_t MultiTimeRebuilder::sortTimeFiles(size_t max_files) {
    size_t started = 0;
    rebuildFiles(
        [&started](uint64_t, const string &, const TimeFileDirtyEntry *entry) {
            if (!entry || entry->unsorted_version == 0)
                return false;
            ++started;
            return true;
        },
        [](const TimeBlock &) { return true; },
        max_files);
    return max_files ? std::min(started, max_files) : started;
}

size_t MultiTimeRebuilder::patchTimeBlocks(const vector<TimeBlockSizePatch> &patches) {
    size_t total_patched = 0;
    if (patches.empty())
        return 0;
    std::set<uint64_t> block_days;
    for (const auto &patch : patches)
        block_days.insert(StampUtils::getStartOfDay(patch.start_time));
    for (const auto &it : _timefiles_map) {
        if (it.first != 0 && block_days.count(it.first) == 0)
            continue;
        try {
            TimeRebuilder rebuilder(it.second);
            total_patched += rebuilder.patchTimeBlocks(patches);
        } catch (const std::exception &ex) {
            WarnL << "Failed to patch TimeFile sizes in " << it.second << ": " << ex.what();
        }
    }
    return total_patched;
}

size_t MultiTimeRebuilder::verifyTimeBlocks(const vector<TimeBlockSizePatch> &patches) const {
    size_t total_verified = 0;
    if (patches.empty()) return 0;
    std::set<uint64_t> block_days;
    for (const auto &patch : patches)
        block_days.insert(StampUtils::getStartOfDay(patch.start_time));
    for (const auto &it : _timefiles_map) {
        if (it.first != 0 && block_days.count(it.first) == 0)
            continue;
        try {
            total_verified += TimeRebuilder(it.second).verifyTimeBlocks(patches);
        } catch (const std::exception &ex) {
            WarnL << "Failed to verify TimeFile sizes in " << it.second << ": " << ex.what();
        }
    }
    return total_verified;
}

} // namespace managerkit
