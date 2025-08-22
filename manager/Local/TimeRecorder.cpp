#include <cmath>
#include <ctime>
#include "Common/config.h"
#include "Common/Parser.h"
#include "Record/Recorder.h"
#include "Thread/WorkThreadPool.h"
#include "TimeRecorder.h"
#include "TimeQuery.h"
#include "Camera/GenericRtspCameraImp.h"

using namespace std;
using namespace toolkit;
using namespace managerkit;

namespace mediakit {

static onceToken token(
    []() { 
        GOOGLE_PROTOBUF_VERIFY_VERSION;
    },
    []() { google::protobuf::ShutdownProtobufLibrary(); }
);

INSTANCE_IMP(TimeRecorder)

TimeRecorder::TimeRecorder(const string &path) {
    _path = path;
    if (_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kTimeSavePath);
        _path = File::absolutePath("", recordPath);
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
    if (File::is_dir(_path)) {
        File::scanDir(_path, [&](const string &path, bool is_dir) { 
            if (!is_dir && path.find("/" + mediaServerId) != string::npos && end_with(path, ".s3db")) {
                _full_path = path;
            }
            return true;
        });
    } else {
        _full_path = _path;
    }
    if (_full_path.empty()) {
        auto full_name = mediaServerId + "--" + to_string(_file_index) +".s3db";
        _full_path =  _path + "/" + full_name;
    } else {
        string parent_path = File::parentDir(_full_path);
        string file_index = findSubString(_full_path.data() + parent_path.size(), "--", ".s3db");
        _file_index = stoi(file_index);
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

void TimeRecorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
    if (_mem_muxer) {
        _mem_muxer->flush();
    }
}

bool TimeRecorder::inputBlock(const TimeBlock &block) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_mem_muxer) {
        flush();

        _mem_muxer = std::make_shared<TimeMuxerMemory>();
        TraceL << "Enable record time block in memory";
        return true;
    }
    return false;
}

void TimeRecorder::getMemoryBlockAndRefresh(const std::function<void(const string &buf)> &on_data, const std::function<void()> &on_close) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    flush();
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

////////////////////////////////// TimeRebuilder ////////////////////////////////////////

TimeRebuilder::TimeRebuilder(TimeRecorder::Ptr &recorder) : _weak_recorder(recorder) {}

static uint32_t findFileIndex(const string &filename) {
    string file_index = findSubString(filename.data(), "--", ".s3db");
    return file_index.empty() ? 0 : stoi(file_index);
}

void TimeRebuilder::createTempFile() {
    auto strong_recorder = _weak_recorder.lock();
    if (!strong_recorder) {
        return;
    }
    _src_path = strong_recorder->getFilePath();
    if (_src_path.empty()) {
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        GET_CONFIG(string, recordPath, Protocol::kTimeSavePath)
        recordPath = File::absolutePath("", recordPath);
        File::scanDir(recordPath, [&](const string &path, bool is_dir) { 
            if (!is_dir && path.find("/" + mediaServerId) != string::npos && end_with(path, ".s3db")) {
                _src_path = path;
            }
            return true;
        });
    }
    
    if (_src_path.empty() || File::fileSize(_src_path) == 0) {
        // string _src_path empty or file time source empty cause error
        throw std::runtime_error("Time file has not been created");
    }
    TraceL << "Time source file: " << _src_path;

    string parent_path = File::parentDir(_src_path);
    auto src_index = findFileIndex(_src_path);
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    string file_name = (StrPrinter << mediaServerId << "--" << ++src_index << ".s3db");

    auto full_path = parent_path + file_name;
    auto full_path_tmp = parent_path + "." + file_name;

    // delete tmp file
    if (File::fileExist(full_path_tmp)) {
        TraceL << "Remove old template file: " << full_path_tmp;
        File::delete_file(full_path_tmp);
    }

    try {
        _writer = std::make_shared<TimeRecorder>(full_path_tmp);
        TraceL << "Open tmp time file: " << full_path_tmp;
        _full_path_tmp = full_path_tmp;
        _full_path = full_path;
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

static void removeSrcFile(const string &src_path) {
    TraceL << "Remove time source file: " << src_path;
    File::delete_file(src_path);
    string maker_path(src_path);
    replace(maker_path, ".s3db", ".idx");
    TraceL << "Remove time maker file: " << maker_path;
    File::delete_file(maker_path);
}

void TimeRebuilder::closeTempFile() {
    // close writer to close tmp file
    _writer.reset();
}

static void renameMakerFile(string &db_path_tmp, string &db_path) {
    string maker_path_tmp(db_path_tmp);
    replace(maker_path_tmp, ".s3db", ".idx");
    string maker_path(db_path);
    replace(maker_path, ".s3db", ".idx");
    // rename file
    TraceL << "Rename time maker file: " << maker_path_tmp << "-->" << maker_path;
    rename(maker_path_tmp.data(), maker_path.data());
}

void TimeRebuilder::commitTempFile() {
    // rename tmp file to offical in order for time recorder instance to write to this
    TraceL << "Rename time destination file: " << _full_path_tmp << "-->" << _full_path;
    rename(_full_path_tmp.data(), _full_path.data());
    // rename tmp marker file
    renameMakerFile(_full_path_tmp, _full_path);
    // delete old time file for time recorder instance to record
    removeSrcFile(_src_path);
}

using RecordProfiles = unordered_map<string /*camera_id/stream_id*/, pair<uint64_t /*min_value*/, uint64_t /*max_value*/>>;

static RecordProfiles getRecordProfiles() {
    RecordProfiles profiles;
    time_t current_time = time(nullptr);
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) {
        auto ptr = dynamic_pointer_cast<GenericRtspCameraImp>(src);
        if (ptr) {
            auto option = ptr->getCameraOption();
            uint64_t min_value = 0;
            uint64_t max_value = 0;
            if (option.keepArchivedMinForAuto) {
                min_value = current_time;
            } else {
                min_value = current_time - option.keepArchivedMinFor;
            }

            if (option.keepArchivedMaxForAuto) {
                //todo: get first block from any stream proxy
            } else {
                max_value = current_time - option.keepArchivedMaxFor;
            }

            if (ptr->hasPrimaryStream()) {
                auto tuple = ptr->getStreamTuple(PrimaryStream);
                string key_primary = (StrPrinter << tuple.device_id << "/" << tuple.stream_id);
                profiles.emplace(key_primary, make_pair(min_value, max_value));
            }
            if (ptr->hasSecondaryStream()) {
                auto tuple = ptr->getStreamTuple(SecondaryStream);
                string key_second = (StrPrinter << tuple.device_id << "/" << tuple.stream_id);
                profiles.emplace(key_second, make_pair(min_value, max_value));
            }
        }
    });
    return profiles;
}

size_t TimeRebuilder::rebuildTimeLine(KeepTimeMap &map, size_t space_reclaim) {
    auto strong_recorder = _weak_recorder.lock();
    if (!strong_recorder) {
        return 0;
    }
    Ticker ticket;
    size_t removed_bytes = 0;
    double keep_percent = 100.0;
    auto record_profiles = getRecordProfiles();

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
    
    while (keep_percent > 0 && (space_reclaim == 0 || removed_bytes < space_reclaim)) {
        // step 1: estimate threshold with new keep_percent value
        for (const auto &p : record_profiles) {
            auto keep_pair = p.second;
            map[p.first] = keep_pair.first - round((keep_pair.first - keep_pair.second) * keep_percent / 100);
        }

        // step 2: delete old template file and create writer
        createTempFile();

        // step 3: flush time recorder and start recording time block to both file and memory
        strong_recorder->enableMemoryMuxer();

        // step 4: read all block in current file with filter and write them into new one 
        MediaTuple tuple;
        auto query = std::make_shared<TimeQuery>(tuple);
        uint64_t start_time = 0;
        uint64_t last_time = time(nullptr);
        query->query(start_time, last_time, [this, keep_block](const TimeBlock &block) {
            if (keep_block(block)) {
                _writer->inputBlock(block);
            }
        });

        // step 3: enable atomic flag, read all block in memory and write them into new file
        auto writeMemoryBlock = [&](const string &buf) {
            auto demuxer = std::make_shared<TimeMemoryDemuxer>(buf);
            bool eof = false;
            while (!eof) {
                TimeBlockList list;
                demuxer->readBlockList(list, eof);
                if (eof) {
                    break;
                }
                for (const auto& block : list.blocks()) {
                    if (keep_block(block)) {
                        _writer->inputBlock(block);
                    }
                }
            }
        };

        // step 4: close file and rename filename if if removed_bytes is enough
        auto afterClose = [&]() { 
            closeTempFile(); 
            if (removed_bytes >= space_reclaim) {
                commitTempFile();
            }
        };

        strong_recorder->getMemoryBlockAndRefresh(writeMemoryBlock, afterClose);

        // step 5: decrease keep_percent to estimate removed bytes again in next loop if removed_bytes is not enough
        if (removed_bytes < space_reclaim) {
            // todo: auto select keep_percent by read/write speed
            keep_percent += (-5.0);
            TraceL << "Decrease keep percent: " << format_double_2f(keep_percent) << "%";
        }
        // only enforce storage policy once if space_reclaim equal 0 byte
        if (space_reclaim == 0) {
            break;
        }
    }
    DebugL << "Recreated time file. Keep percent: " << format_double_2f(keep_percent) << "%. Removed bytes: " << format_bytes_human_readable(removed_bytes)
           << ". Elapsed: " << formatDuration(ticket.elapsedTime());
    return removed_bytes;
}

} // namespace mediakit
