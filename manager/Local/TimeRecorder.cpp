#include <cmath>
#include <ctime>
#include "Common/config.h"
#include "Common/Parser.h"
#include "Record/Recorder.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static onceToken token(
    []() { 
        GOOGLE_PROTOBUF_VERIFY_VERSION;
    },
    []() { google::protobuf::ShutdownProtobufLibrary(); }
);

INSTANCE_IMP(TimeRecorder)

TimeRecorder::TimeRecorder(const string &path) {
    _folder_path = path;
    if (_folder_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kTimeSavePath);
        _folder_path = File::absolutePath("", recordPath);
    }
}

TimeRecorder::~TimeRecorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void TimeRecorder::createFile() {
    closeFile();

    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    if (File::is_dir(_folder_path)) {
        File::scanDir(_folder_path, [&](const string &path, bool is_dir) { 
            if (!is_dir && path.find("/" + mediaServerId) != string::npos && end_with(path, ".s3db")) {
                _full_path = path;
            }
            return true;
        });
    }
    if (_full_path.empty()) {
        auto full_name = mediaServerId + "--0.s3db";
        _full_path =  _folder_path + "/" + full_name;
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
}

void TimeRecorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

bool TimeRecorder::inputBlock(const TimeBlock &block) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_muxer) {
        // Generate time file
        createFile();
    }

    if (_muxer) {
        return _muxer->inputBlock(block);
    }
    return false;
}

} // namespace mediakit
