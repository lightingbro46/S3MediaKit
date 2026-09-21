#include <cmath>
#include <ctime>
#include <vector>
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"
#include "TimeFileDirtyTracker.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

///////////////////// TimeRecorder ///////////////////////////

static void asyncCloseMuxer(const TimeMuxer::Ptr &muxer, const string &path) {
    if (!muxer) {
        return;
    }
    TraceL << "Start close time file: " << path;
    WorkThreadPool::Instance().getExecutor()->async([muxer, path]() mutable {
        // Closing a file can be time-consuming, do it on a background thread.
        muxer->closeFile();
        TraceL << "Closed time file: " << path;
    });
}

TimeRecorder::TimeRecorder(const string &path) {
    _path = path;
    if (_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
        GET_CONFIG(string, appName, Record::kAppName);
        _path = File::absolutePath(appName, recordPath);
    }
    _is_dir = File::is_dir(_path);
}

TimeRecorder::~TimeRecorder() {
    TraceL << "Destroy TimeRecorder for path: " << _path;
    try {
        std::lock_guard<std::mutex> lock(_mutex);
        closeAllLocked(true);
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

string TimeRecorder::resolveTargetPath(uint64_t block_time) const {
    if (!_is_dir) {
        return _path;
    }
    if (block_time == 0) {
        block_time = time(nullptr);
    }
    auto date_str = getTimeStr("%Y-%m-%d", block_time);
    return StrPrinter << _path << "/" << date_str << ".s3db";
}

bool TimeRecorder::isLivePath(const string &file_path) const {
    if (!_is_dir) {
        return true;
    }
    return file_path == resolveTargetPath(time(nullptr));
}

void TimeRecorder::evictIdleImportWritersLocked() {
    if (!_is_dir) {
        return;
    }
    string today = resolveTargetPath(time(nullptr));
    // Count evictable import writers (past-date, not participating in a rebuild) and
    // close the least recently used ones until there is room for a new import writer.
    for (;;) {
        FileWriter::Ptr victim;
        size_t evictable = 0;
        for (const auto &it : _writers) {
            const auto &w = it.second;
            if (w->full_path == today || w->mem_muxer || w->mirror_recorder) {
                continue;
            }
            ++evictable;
            if (!victim || w->last_access < victim->last_access) {
                victim = w;
            }
        }
        if (evictable < _max_import_writers || !victim) {
            break;
        }
        TraceL << "Evict idle import time file: " << victim->full_path;
        asyncCloseMuxer(victim->muxer, victim->full_path);
        _writers.erase(victim->full_path);
    }
}

TimeRecorder::FileWriter::Ptr TimeRecorder::getOrCreateWriter(const string &file_path) {
    auto it = _writers.find(file_path);
    if (it != _writers.end()) {
        return it->second;
    }

    if (!isLivePath(file_path)) {
        evictIdleImportWritersLocked();
    }

    auto writer = std::make_shared<FileWriter>();
    writer->full_path = file_path;
    // A just-opened writer is not the least recently used one, and must not look idle forever to
    // closeIdleWriters() when it is opened by a path that does not write immediately.
    writer->last_access = getCurrentMillisecond();
    try {
        writer->muxer = std::make_shared<TimeMuxer>();
        TraceL << "Open time file: " << file_path;
        writer->muxer->openFile(file_path);
    } catch (std::exception &ex) {
        WarnL << ex.what();
        return nullptr;
    }
    _writers[file_path] = writer;
    return writer;
}

size_t TimeRecorder::closeIdleWriters(uint64_t idle_ms) {
    if (idle_ms == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const auto now = getCurrentMillisecond();
    size_t closed = 0;
    for (auto it = _writers.begin(); it != _writers.end();) {
        auto &writer = it->second;
        // Same exclusion as the import eviction: a rebuild owns these writers.
        if (writer->mem_muxer || writer->mirror_recorder) {
            ++it;
            continue;
        }
        if (now < writer->last_access + idle_ms) {
            ++it;
            continue;
        }
        // Closed synchronously, unlike the import eviction: that one runs inside inputBlock() on the
        // recording path, this one runs on BlockingExecutors::storage() where blocking is expected.
        // It also removes a race — an async close still holding buffered data while a new writer
        // reopens the same path would make TimeMuxer::setLastOffset() read a stale file size.
        TraceL << "Close idle time file: " << writer->full_path;
        if (writer->muxer) {
            writer->muxer->closeFile();
            writer->muxer = nullptr;
        }
        it = _writers.erase(it);
        ++closed;
    }
    return closed;
}

void TimeRecorder::closeAllLocked(bool async) {
    for (auto &it : _writers) {
        auto &w = it.second;
        if (!w->muxer) {
            continue;
        }
        if (async) {
            asyncCloseMuxer(w->muxer, w->full_path);
        } else {
            w->muxer->closeFile();
        }
        w->muxer = nullptr;
    }
    _writers.clear();
}

bool TimeRecorder::inputBlock(const TimeBlock &block) {
    std::lock_guard<std::mutex> lock(_mutex);

    string target = resolveTargetPath(block.start_time());
    auto writer = getOrCreateWriter(target);
    if (!writer || !writer->muxer) {
        return false;
    }
    writer->last_access = getCurrentMillisecond();

    // Mark the file dirty BEFORE writing so the dirty version is always bumped before
    // the block reaches disk. This closes the race where a concurrent rebuild swap could
    // read the old version, delete the source, and drop a block that was just flushed but
    // not yet marked. A rare spurious dirty mark (if the write below fails) only costs one
    // extra, harmless rebuild.
    if (_is_dir) {
        TimeFileDirtyTracker::Instance().markDirty(_path, target);
    }

    // Capture blocks written after the rebuild snapshot into memory.
    if (writer->mem_muxer) {
        writer->mem_muxer->inputBlock(block);
    }
    auto ret = writer->muxer->inputBlock(block);
    // Mirror live blocks to the temporary rebuild recorder so the swapped file is complete.
    if (writer->mirror_recorder) {
        writer->mirror_recorder->inputBlock(block);
    }
    return ret;
}

bool TimeRecorder::beginRebuildCapture(const string &file, uint64_t &snapshot_size) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _writers.find(file);
    if (it == _writers.end() || !it->second->muxer) {
        return false;
    }
    if (!it->second->mem_muxer) {
        it->second->mem_muxer = std::make_shared<TimeMuxerMemory>();
        TraceL << "Enable rebuild capture for time file: " << file;
    }
    snapshot_size = File::fileSize(file);
    return true;
}

bool TimeRecorder::drainRebuildCaptureAndStartMirror(const string &file,
                                                     const Ptr &tmp_recorder,
                                                     const function<void(const string &buf)> &on_data) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _writers.find(file);
    if (it == _writers.end() || !it->second->muxer || !tmp_recorder) {
        return false;
    }
    auto &writer = it->second;
    if (writer->mem_muxer) {
        auto buf = writer->mem_muxer->getMemoryBlock();
        on_data(buf);
        writer->mem_muxer = nullptr;
    }
    writer->mirror_recorder = tmp_recorder;
    TraceL << "Mirror live time blocks to tmp recorder for: " << file;
    return true;
}

void TimeRecorder::finishRebuildSwap(const string &file, const Ptr &tmp_recorder, const function<void()> &on_ready) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _writers.find(file);
    if (it != _writers.end()) {
        auto &writer = it->second;
        if (writer->mirror_recorder == tmp_recorder) {
            writer->mirror_recorder = nullptr;
        }
        writer->mem_muxer = nullptr;
        if (writer->muxer) {
            TraceL << "Close active time file before rebuild swap: " << file;
            writer->muxer->closeFile();
            writer->muxer = nullptr;
        }
        _writers.erase(it);
    }
    if (tmp_recorder) {
        tmp_recorder->closeNow();
    }
    on_ready();
}

void TimeRecorder::closeNow() {
    std::lock_guard<std::mutex> lock(_mutex);
    closeAllLocked(false);
}

} // namespace managerkit
