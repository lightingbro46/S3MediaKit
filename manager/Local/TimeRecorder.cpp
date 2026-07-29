#include <cmath>
#include <ctime>
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"
#include "Common/StrUtil.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

///////////////////// TimeRecorder ///////////////////////////

TimeRecorder::TimeRecorder(const string &path) {
    _path = path;
    if (_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
        GET_CONFIG(string, appName, Record::kAppName);
        _path = File::absolutePath(appName, recordPath);
    }
}

TimeRecorder::~TimeRecorder() {
    TraceL << "Destroy TimeRecorder for path: " << _full_path;
    try {
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void TimeRecorder::createFile() {
    closeFile();

    if (File::is_dir(_path)) {
        uint64_t now = time(nullptr);
        auto date_str = getTimeStr("%Y-%m-%d", now);
        _full_path = StrPrinter << _path << "/" << date_str << ".s3db";
        _next_open_time = StampUtils::getStartOfDay(now) + 86400; // close file after one day
    } else {
        _full_path = _path;
    }
    
    try {
        // open time file
        _muxer = std::make_shared<TimeMuxer>();
        TraceL << "Open time file: " << _full_path;
        _muxer->openFile(_full_path);

    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void TimeRecorder::createSDFile(uint64_t date_sec) {
    closeFile();

    if (File::is_dir(_path)) {
        uint64_t now = time(nullptr);
        if (date_sec == 0) 
            date_sec = now;
        std::string date_str = getTimeStr("%Y-%m-%d", date_sec);
        _full_path = StrPrinter << _path << "/" << date_str << "_sd" << ".s3db";
        _next_open_time = StampUtils::getStartOfDay(date_sec) + 86400; // close file after one day
    } else {
        _full_path = _path;
    }
    
    try {
        // open time file
        _muxer = std::make_shared<TimeMuxer>();
        TraceL << "Open replay time file: " << _full_path;
        _muxer->openFile(_full_path);

    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void TimeRecorder::asyncClose() {
    auto muxer = _muxer;
    auto full_path = _full_path;
    TraceL << "Start close time file: " << full_path;
    WorkThreadPool::Instance().getExecutor()->async([muxer, full_path]() mutable {
        // Closing file can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing time file: " << full_path;
        muxer->closeFile();
        TraceL << "Closed time file: " << full_path;
    });
}

void TimeRecorder::closeFile() {
    if (_muxer) {
        asyncClose();
        _muxer = nullptr;
    }
    if (_mem_muxer) {
        _mem_muxer = nullptr ;
    }
    _rebuild_mirror_recorder = nullptr;
}

void TimeRecorder::closeFileDirect() {
    if (_muxer) {
        TraceL << "Close time file directly: " << _full_path;
        _muxer->closeFile();
        _muxer = nullptr;
    }
    if (_mem_muxer) {
        _mem_muxer = nullptr;
    }
    _rebuild_mirror_recorder = nullptr;
}

void TimeRecorder::closeNow() {
    std::lock_guard<std::mutex> lock(_mutex);
    closeFileDirect();
}

bool TimeRecorder::inputBlock(const TimeBlock &block) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mem_muxer) {
        _mem_muxer->inputBlock(block);
    }
    if (!_muxer) {
        // Generate time file
        createFile();
    }
    if (_muxer) {
        auto ret = _muxer->inputBlock(block);
        if (_rebuild_mirror_recorder) {
            _rebuild_mirror_recorder->inputBlock(block);
        }
        return ret;
    }
    return false;
}

bool TimeRecorder::inputSDBlock(const TimeBlock &block) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mem_muxer) {
        _mem_muxer->inputBlock(block);
    }
    if (!_muxer) {
        // Generate time file
        createSDFile(block.start_time());
    }
    if (_muxer) {
        std::string date_str = getTimeStr("%Y-%m-%d", block.start_time());
        std::string new_path = StrPrinter << _path << "/" << date_str << "_sd" << ".s3db";
        if (new_path != _full_path){
            createSDFile(block.start_time());
        }
        return _muxer->inputBlock(block);
    }
    return false;
}

bool TimeRecorder::enableMemoryMuxer() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_mem_muxer) {
        _mem_muxer = std::make_shared<TimeMuxerMemory>();
        TraceL << "Enable record time block in memory";
        return true;
    }
    return false;
}

void TimeRecorder::getMemoryBlockAndRefresh(const std::function<void(const string &buf)> &on_data, const std::function<void()> &on_close) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mem_muxer) {
        auto mem_muxer_ptr = dynamic_pointer_cast<TimeMuxerMemory>(_mem_muxer);
        if (mem_muxer_ptr) {
            auto buf = mem_muxer_ptr->getMemoryBlock();
            on_data(buf);
        }
    }
    // close file before run on_close callback because function on_close excute rename both old and new file
    closeFile();
    on_close();
}

bool TimeRecorder::beginRebuildCapture(const string &file, uint64_t &snapshot_size) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_full_path != file || !_muxer) {
        return false;
    }
    if (!_mem_muxer) {
        _mem_muxer = std::make_shared<TimeMuxerMemory>();
        TraceL << "Enable rebuild capture for time file: " << file;
    }
    snapshot_size = File::fileSize(file);
    return true;
}

bool TimeRecorder::drainRebuildCaptureAndStartMirror(const string &file,
                                                     const Ptr &tmp_recorder,
                                                     const function<void(const string &buf)> &on_data) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_full_path != file || !_muxer || !tmp_recorder) {
        return false;
    }
    if (_mem_muxer) {
        auto mem_muxer_ptr = dynamic_pointer_cast<TimeMuxerMemory>(_mem_muxer);
        if (mem_muxer_ptr) {
            auto buf = mem_muxer_ptr->getMemoryBlock();
            on_data(buf);
        }
        _mem_muxer = nullptr;
    }
    _rebuild_mirror_recorder = tmp_recorder;
    TraceL << "Mirror live time blocks to tmp recorder for: " << file;
    return true;
}

void TimeRecorder::finishRebuildSwap(const string &file, const Ptr &tmp_recorder, const function<void()> &on_ready) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_rebuild_mirror_recorder == tmp_recorder) {
        _rebuild_mirror_recorder = nullptr;
    }
    if (_mem_muxer) {
        _mem_muxer = nullptr;
    }
    if (_full_path == file && _muxer) {
        TraceL << "Close active time file before rebuild swap: " << file;
        _muxer->closeFile();
        _muxer = nullptr;
    }
    if (tmp_recorder) {
        tmp_recorder->closeNow();
    }
    on_ready();
}

} // namespace managerkit
