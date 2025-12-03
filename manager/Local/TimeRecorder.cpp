#include <cmath>
#include <ctime>
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

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
        _next_open_time = getStartOfDay(now) + 86400; // close file after one day
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

} // namespace mediakit
