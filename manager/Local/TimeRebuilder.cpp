#include "TimeRebuilder.h"
#include "Common/Parser.h"
#include "TimeDemuxer.h"
#include "Common/StrUtil.h"
#include "Local/StatisticRecorder.h"
#include "Thread/WorkThreadPool.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

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

    // determine whether the source time file is in use
    string current_time_str = getTimeStr("%Y-%m-%d");
    _in_use = filename.find(current_time_str) != string::npos;
    if (_in_use) {
        TraceL << "Time file " << _src_path << " is in use";
        _recorder = getTimeRecorder(parent_path);
    }
    
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

void TimeRebuilder::closeTempFile() {
    // close writer to close tmp file
    _writer.reset();

    // rename file with original name in background thread
    auto full_path_tmp = _full_path_tmp;;
    auto full_path = _full_path;
    auto src_path = _src_path;
    WorkThreadPool::Instance().getExecutor()->async([full_path_tmp, full_path, src_path]() {
        if (!src_path.empty()) {
            deleteTimeFile(src_path);
            deleteMakerFile(src_path);
        }
        if (!full_path_tmp.empty()) {
            // Get file size
            uint64_t file_size = File::fileSize(full_path_tmp);
            if (file_size == 0) {
                deleteTimeFile(full_path_tmp);
                deleteMakerFile(full_path_tmp);
                return;
            }
            // Change the temporary file name to the official file name to prevent access to the file before it is completed
            renameTimeFile(full_path_tmp, full_path);
            renameMakerFile(full_path_tmp, full_path);
        }
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

size_t TimeRebuilder::rebuildTimeLine(const KeepTimeMap &map) {
    Ticker ticket;
    size_t removed_bytes = 0;
    
    auto keep_block = [&removed_bytes, &map](const TimeBlock &block) -> bool { 
        string key = (StrPrinter << block.app() << "/" << block.stream()); 
        auto it = map.find(key);
        if (it != map.end()) {
            if (block.start_time() >= it->second) {
                TraceL << "Keep time block: " << key << " " << block.start_time();
                return true;
            }
        }
        TraceL << "Remove time block: " << key << " " << block.start_time();
        removed_bytes += block.file_size();
        TraceL << "Increase: " << format_bytes_human_readable(block.file_size()) << ". Removed bytes: " << format_bytes_human_readable(removed_bytes);
        return false;
    };

    // step 1: delete old template file and create writer
    createTempFile();
    
    if (_in_use) {
        // step 2: flush time recorder and start recording time block to both file and memory
        _recorder->enableMemoryMuxer();
    }

    CameraArchivedChanges tmp_changes;
    {
        // step 3: read all block in current file with filter and write them into new one 
        auto demuxer = make_shared<TimeDemuxer>();
        demuxer->openFile(_src_path);
        bool eof = false;
        while (!eof) {
            TimeBlock block;
            demuxer->readBlock(block, eof);
            if (eof) {
                break;
            }
            if (keep_block(block)) {
                _writer->inputBlock(block);
            } else {
                addTempArchivedChanges(tmp_changes, block);
            }
        }
    }

    if (_in_use) {
        // step 3: enable atomic flag, read all block in memory and write them into new file
        auto writeMemoryBlock = [&](const string &buf) {
            auto demuxer = std::make_shared<TimeMemoryDemuxer>(buf);
            bool eof = false;
            while (!eof) {
                TimeBlock block;
                demuxer->readBlock(block, eof);
                if (eof) {
                    break;
                }
                if (keep_block(block)) {
                    _writer->inputBlock(block);
                } else {
                    addTempArchivedChanges(tmp_changes, block);
                }
            }
        };

        // step 4: close file and rename filename
        auto afterClose = [&]() { 
            closeTempFile(); 
            commitArchivedChanges(tmp_changes);
        };
        
        _recorder->getMemoryBlockAndRefresh(writeMemoryBlock, afterClose);
    } else {
        // step 4: close file and rename filename
        closeTempFile(); 
        commitArchivedChanges(tmp_changes);
    }

    TraceL << "Recreated time file: " << _src_path << ". Removed bytes: " << format_bytes_human_readable(removed_bytes)
           << ". Elapsed: " << formatDuration(ticket.elapsedTime());

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

size_t MultiTimeRebuilder::rebuildTimeLine(const KeepTimeMap &map) {
    size_t total_removed_bytes = 0;
    if (_timefiles_map.empty()) {
        WarnL << "No time files found under path: " << _src_path;
        return total_removed_bytes;
    }

    auto min_keep_time = findMinKeepTime(_src_path, map);;
    TraceL << "Only rebuild time file under : " << getTimeStr("%Y-%m-%d %H:%M:%S", min_keep_time);

    for (const auto &it : _timefiles_map) {
        const auto &stamp = it.first;
        const auto &timefile = it.second;

        if (stamp >= min_keep_time) {
            TraceL << "Skip rebuild time file: " << timefile;
            continue;
        }

        try {
            TraceL << "Rebuild time file: " << timefile;
            auto rebuilder = std::make_shared<TimeRebuilder>(timefile);
            size_t removed_bytes = rebuilder->rebuildTimeLine(map);
            total_removed_bytes += removed_bytes;
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }
    return total_removed_bytes;
}

} // namespace managerkit