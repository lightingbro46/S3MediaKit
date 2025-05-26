#include <sstream>
#include <cmath>
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Common/Parser.h"
#include "Record/Recorder.h"
#include "Util/File.h"
#include "TimeRecorder.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

static uint64_t getStartOfDay(uint64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&t);

    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;

    return static_cast<uint64_t>(std::mktime(tm));
}

static uint64_t getStartOfHour(uint64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&t);

    tm->tm_min = 0;
    tm->tm_sec = 0;

    return static_cast<uint64_t>(std::mktime(tm));
}

static uint64_t getStartOfMinute(uint64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&t);

    tm->tm_sec = 0;

    return static_cast<uint64_t>(std::mktime(tm));
}

//////////////////////////////TimeRecorder//////////////////////////////////////

INSTANCE_IMP(TimeRecorder)

TimeRecorder::TimeRecorder(size_t max_batch, size_t flush_threshold) : _max_batch(max_batch), _flush_threshold(flush_threshold) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, recordAppName, Record::kAppName);
    _output_dir = File::absolutePath(recordAppName, recordPath);
    _file_index_map = getListDataIndex(_output_dir);
    if (!_file_index_map.empty()) {
        _current_file_index = *_file_index_map.rbegin();
    }
    
    rolateFile();
}

TimeRecorder::~TimeRecorder() {
    DebugL;

    flush(false);

    if (_data_stream.is_open()) _data_stream.close();
    if (_meta_stream.is_open()) _meta_stream.close();

    google::protobuf::ShutdownProtobufLibrary();
}

set<uint32_t> TimeRecorder::getListDataIndex(const string &dir_path) {
    set<uint32_t> ret;
    if (File::is_dir(dir_path)) {
        File::scanDir(dir_path, [&](const string &path, bool is_dir) {
            if (!is_dir && end_with(path, ".s3db")) {
                auto index_str = findSubString(path.data(), "--", ".");
                auto index = (uint32_t)(atof(index_str.data()));
                ret.emplace(index);
            }
            return true;
        });
    }
    return ret;
}

uint32_t TimeRecorder::getBlockListSize(const string &file_path) {
    if (!File::fileExist(file_path)) {
        return 0;
    }

    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("can not open file: " + file_path);
    }   
    
    size_t ret = 0;
    while (in.peek() != EOF) {
        uint32_t size;
        in.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (in.gcount() != sizeof(size)) break;

        std::string buffer(size, '\0');
        in.read(&buffer[0], size);
        if (in.gcount() != size) break;

        TimeBlockList list;
        if (list.ParseFromString(buffer)) {
            ret++;
        }
    }
    in.close();
    return ret;
}

std::string TimeRecorder::indexToDbFilePath(uint32_t index) {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    string file_name = mediaServerId + "--" + to_string(index) + ".s3db";
    return _output_dir + "/" + file_name;
}

std::string TimeRecorder::indexToMetaFilePath(uint32_t index) {
    string file_name = "meta--" + to_string(index) + ".idx";
    return _output_dir + "/" + file_name;
}
 
void TimeRecorder::rolateFile() {
    if (_data_stream.is_open()) _data_stream.close();
    if (_meta_stream.is_open()) _meta_stream.close();

    ++_current_file_index;
    _current_offset = 0;
    _current_block_size = 0;

    string datafile_path = indexToDbFilePath(_current_file_index);
    _data_stream.open(datafile_path, std::ios::binary | std::ios::app);
    if (!_data_stream.is_open()) {
        throw std::runtime_error("can not open next data file: " + datafile_path);
    }

    string metafile_path = indexToMetaFilePath(_current_file_index);
    _meta_stream.open(metafile_path, std::ios::binary | std::ios::app);
    if (!_data_stream.is_open()) {
        throw std::runtime_error("can not open next meta file: " + metafile_path);
    }

    _file_index_map.emplace(_current_file_index);
}

void TimeRecorder::flush(bool open_next_file) {
    if (_pending_list.blocks_size() == 0) {
        return;
    }
    _pending_list.set_created_at(_current_minute);
    writeList(_pending_list);
    _pending_list.Clear();

    if (open_next_file && _current_block_size >= _flush_threshold) {
        rolateFile();
    }
}

void TimeRecorder::writeList(const TimeBlockList& list) {
    if (list.blocks_size() == 0) {
        return;
    }

    uint32_t size = list.ByteSizeLong();
    _data_stream.write(reinterpret_cast<const char*>(&size), sizeof(uint32_t));

    list.SerializeToOstream(&_data_stream);
    _data_stream.flush();

    // write meta index
    BlockListIndexEntry entry;
    entry.file_index = _current_file_index;
    entry.start_time = _current_minute;
    entry.offset = _current_offset;
    entry.count = list.blocks_size();
    _meta_stream.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    _meta_stream.flush();

    _current_offset += sizeof(uint32_t) + size;
    ++_current_block_size;
}

void TimeRecorder::addBlock(const TimeBlock &block) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    int64_t block_minute = getStartOfMinute(block.start_time());
    if (_current_minute == -1) {
        _current_minute = block_minute;
    }

    // Flush if current block is greater than old block
    if (block_minute != _current_minute) {
        flush(true);
        _current_minute = block_minute;
    }

    *_pending_list.add_blocks() = block;

    if (_pending_list.blocks_size() >= static_cast<int>(_max_batch)) {
        flush(true);
    }
}

vector<BlockListIndexEntry> TimeRecorder::readMetaList(uint32_t file_index) {
    string metafile_path = indexToMetaFilePath(file_index);
    std::ifstream meta(metafile_path, std::ios::binary);
    if (!meta.is_open()) {
        throw std::runtime_error("can not open meta file: " + metafile_path);
    }
    vector<BlockListIndexEntry> list;

    BlockListIndexEntry entry;
    while (meta.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        list.push_back(entry);
    }
    meta.close();

    return list;
}

TimeBlockList TimeRecorder::readBlockList(uint32_t file_index, uint64_t offset) {
    string datafile_path = indexToDbFilePath(file_index);
    std::ifstream in(indexToDbFilePath(file_index), std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("can not open data file: " + datafile_path);
    }
    in.seekg(offset);

    uint32_t size;
    in.read(reinterpret_cast<char*>(&size), sizeof(uint32_t));

    std::string buffer(size, '\0');
    in.read(&buffer[0], size);

    TimeBlockList list;
    list.ParseFromString(buffer);

    in.close();
    return list;
}

void TimeRecorder::query(uint64_t start_time, uint64_t end_time, const std::string &camera_id, const std::function<void(const TimeBlock &block)> &cb) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    for (uint32_t idx : _file_index_map) {
        auto meta_list = readMetaList(idx);
        for (const auto& entry : meta_list) {
            if (entry.start_time > end_time) continue;
            if (entry.start_time + 60 < start_time) continue;  // each list last maximum one minute

            auto list = readBlockList(entry.file_index, entry.offset);
            for (const auto& block : list.blocks()) {
                if (block.start_time() >= start_time &&
                    block.start_time() <= end_time &&
                    block.app() == camera_id) {
                    cb(block);
                }
            }
        }
    }
}

void TimeRecorder::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time, const std::string &camera_id, int period_type, int detail,
    const std::function<void(const toolkit::SockException &ex, const Json::Value &data)> &cb) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    Json::Value result;
    result["camera_id"] = camera_id;

    struct TimeRange {
        uint64_t startTime;
        uint32_t duration;
    };

    if (period_type == 1) {
        if (detail == 0) {
            vector<TimeRange> _result;

            query(start_time, end_time, camera_id, [&](const TimeBlock &block) mutable {
                if (!_result.empty()) {
                    TimeRange &last = _result.back();
                    uint64_t last_end = last.startTime + last.duration;

                    if (block.start_time() <= last_end + 1) {
                        uint64_t new_end = MAX(last_end, block.start_time() + block.time_len());
                        last.duration = static_cast<int>(new_end - last.startTime);
                        return;
                    }
                }
                _result.push_back({ block.start_time(), static_cast<uint32_t>(block.time_len()) });
            });

            result["periods"] = Json::arrayValue;

            for (auto const &p : _result) {
                Json::Value json_period;
                json_period["startTime"] = p.startTime;
                json_period["duration"] = p.duration;
                result["periods"].append(json_period);
            }

        } else {
            unordered_map<string /*stream_id*/, vector<TimeRange>> _result;

            query(start_time, end_time, camera_id, [&](const TimeBlock &block) mutable {
                auto &range_map = _result[block.stream()];

                if (!range_map.empty()) {
                    TimeRange &last = range_map.back();
                    uint64_t last_end = last.startTime + last.duration;

                    if (block.start_time() <= last_end + 1) {
                        uint64_t new_end = MAX(last_end, block.start_time() + block.time_len());
                        last.duration = static_cast<int>(new_end - last.startTime);
                        return;
                    }
                }

                range_map.push_back({ block.start_time(), static_cast<uint32_t>(block.time_len())});
            });

            result["streams"] = Json::arrayValue;

            for (auto const &it : _result) {
                Json::Value json_stream;
                json_stream["streamId"] = it.first;
                json_stream["periods"] = Json::arrayValue;

                for (auto const &p : it.second) {
                    Json::Value json_period;
                    json_period["startTime"] = p.startTime;
                    json_period["duration"] = p.duration;
                    json_stream["periods"].append(json_period);
                }
                result["streams"].append(json_stream);
            }
        }
    } else if (period_type == 2) {
        if (detail == 0) {
            unordered_map<string /*date*/, set<int/*hour*/>> _result;

            query(start_time, end_time, camera_id, [&](const TimeBlock &block) mutable {
                string date_str = getTimeStr("%Y-%m-%d", block.start_time());
                string hour_str = getTimeStr("%H", block.start_time());

                auto &hour_map = _result[date_str];
                hour_map.emplace(static_cast<int>(atoi(hour_str.data())));
            });

            for (auto const &it : _result) {
                string date_str = it.first;
                auto &hour_set = it.second;
                result["periods"][date_str] = Json::arrayValue;

                for (int i = 0; i < 24; i++) {
                    auto it_hour = hour_set.find(i);
                    result["periods"][date_str].append(it_hour != hour_set.end() ? 1 : 0);
                }
            }

        } else {
            unordered_map<string /*stream_id*/, unordered_map<string /*date*/, unordered_map<int, std::vector<TimeRange>>>> _result;
            
            query(start_time, end_time, camera_id, [&](const TimeBlock &block) mutable {
                string stream_id = block.stream();
                uint64_t current = block.start_time();
                uint32_t remaining = block.time_len();

                while(remaining > 0) {
                    int64_t t = current;
                    string date_str = getTimeStr("%Y-%m-%d", current);
                    string hour_str = getTimeStr("%H", current);
                    string minute_str = getTimeStr("%M", current);
                    string second_str = getTimeStr("%S", current);
                    
                    int64_t start_of_hour = current - atoi(minute_str.data()) * 60 - atoi(second_str.data());
                    uint32_t start_offset_in_hour = static_cast<int>(current - start_of_hour);
                    uint32_t seconds_left_in_hour = 3600 - start_offset_in_hour;
                    uint32_t chunk = MIN(remaining, seconds_left_in_hour);
                    
                    auto &range_map = _result[stream_id][date_str][static_cast<int>(atoi(hour_str.data()))];
                    uint64_t chunk_start = current;
                    if (!range_map.empty() && range_map.back().startTime + range_map.back().duration + 1 >= chunk_start) {
                        range_map.back().duration += chunk;
                    } else {
                        range_map.push_back({chunk_start, chunk});
                    }
                    current += chunk;
                    remaining -= chunk;
                }
            });

            result["streams"] = Json::arrayValue; 
            for (auto const &it_stream : _result) {
                Json::Value json_stream;
                json_stream["streamId"] = it_stream.first;

                for (auto const &it_date : it_stream.second) {
                    string date_string = it_date.first;
                    auto hour_map = it_date.second;
                    Json::Value json_date;
                    for (int i = 0; i < 24; i++) {
                        Json::Value json_hour = Json::arrayValue;
                        auto it_hour = hour_map.find(i);
                        if (it_hour != hour_map.end()) {
                            for (auto const &it : it_hour->second) {
                                Json::Value json_period;
                                json_period["startTime"] = it.startTime;
                                json_period["duration"] = it.duration;
                                json_hour.append(json_period);
                            }
                        }
                        json_date.append(json_hour);
                    }
                    json_stream["dates"][date_string] = json_date;
                }
                result["streams"].append(json_stream);
            }
        }
    } else if (period_type == 0) {
        vector<TimeBlock> blocks;
        query(start_time, end_time, camera_id, [&blocks](const TimeBlock &block) { 
            blocks.push_back(block); 
        });

        result["periods"] = Json::arrayValue; 
        for (auto const &p : blocks) {
            Json::Value json_period;
            json_period["cameraId"] = p.app();
            json_period["streamId"] = p.stream();
            json_period["startTime"] = p.start_time();
            json_period["timeLen"] = p.time_len();
            result["periods"].append(json_period);
        }
    }

    return cb(SockException(Err_success), result);
}

int64_t TimeRecorder::getOffsetOfDate(uint64_t pos_time, const std::string &camera_id, const std::string &stream_id) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    int64_t ret = 0;

    auto start_of_day = getStartOfDay(pos_time);
    
    for (uint32_t idx : _file_index_map) {
        auto meta_list = readMetaList(idx);
        for (const auto& entry : meta_list) {
            if (entry.start_time > pos_time) break;
            if (entry.start_time + 60 < start_of_day) continue;  // each list last maximum one minute

            auto list = readBlockList(entry.file_index, entry.offset);
            for (const auto& block : list.blocks()) {
                if (block.app() == camera_id && block.stream() == stream_id) {
                    if (block.start_time() < start_of_day && block.start_time() + block.time_len() > start_of_day) {
                        ret += start_of_day - block.start_time() + block.time_len();
                    } else if (block.start_time() >= start_of_day && block.start_time() + block.time_len() <= pos_time){
                        ret += block.time_len();
                    } else if (block.start_time() <= pos_time && block.start_time() + block.time_len() > pos_time) {
                        ret += pos_time - block.start_time();
                    }
                }
            }
        }
    }
    return ret;
}

static void *time_recorder_tag = nullptr;

static onceToken token([]() {
#ifdef ENABLE_MP4
NoticeCenter::Instance().addListener(&time_recorder_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        TraceL << "Record mp4 file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.url;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(std::round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);
        
        TimeRecorder::Instance().addBlock(block);
    });

    NoticeCenter::Instance().addListener(&time_recorder_tag, Broadcast::kBroadcastMediaSeeked, [](BroadcastMediaSeekedArgs) {
        auto tuple = split(args.stream, "/");
        int64_t offset = TimeRecorder::Instance().getOffsetOfDate(stamp, tuple[0], tuple[1]);
        invoker(offset);
    });
#endif // ENABLE_MP4

}, []() {
    NoticeCenter::Instance().delListener(&time_recorder_tag);
});