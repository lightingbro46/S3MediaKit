#include <sstream>
#include "Common/config.h"
#include "Common/Parser.h"
#include "Util/File.h"
#include "TimePeriodRecorder.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

//////////////////////////TimeBlockWriter///////////////////////////////

INSTANCE_IMP(TimeBlockWriter)

TimeBlockWriter::TimeBlockWriter(size_t max_batch, size_t flush_threshold) 
    : _max_batch(max_batch), _flush_threshold(flush_threshold) {

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, recordAppName, Record::kAppName);

    _output_dir = File::absolutePath(recordAppName, recordPath);
    string meta_file_path = _output_dir + "/meta.idx";
    _meta_stream.open(meta_file_path, std::ios::binary | std::ios::app);
    if (!_meta_stream.is_open()) {
        //todo: throw error
        throw std::runtime_error("Cannot open meta.idx");
    }

    getCurrentIndex();

    rolateFile();
}

TimeBlockWriter::~TimeBlockWriter() {
    flush();
    if (_data_stream.is_open()) _data_stream.close();
    if (_meta_stream.is_open()) _meta_stream.close();
    google::protobuf::ShutdownProtobufLibrary();
}

void TimeBlockWriter::rolateFile() {
    if (_data_stream.is_open()) {
        _data_stream.close();
    } 
    ++_current_file_index;
    _current_offset = 0;

    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    string next_filename = mediaServerId + "--" + to_string(_current_file_index) + ".s3db";
    std::string data_file_path = _output_dir + "/" + next_filename;

    _data_stream.open(data_file_path, std::ios::binary | std::ios::app);
    if (!_data_stream.is_open()) {
        //todo: throw error
        throw std::runtime_error("Failed to open file: " + data_file_path);
    }
}

void TimeBlockWriter::addBlock(TimeBlock& block) {
    std::lock_guard<std::recursive_mutex> lock(_mtx_time);

    int64_t block_minute = static_cast<int64_t>(block.start_time() / 60);
    if (_current_minute == -1) {
        _current_minute = block_minute;
    }

    // Flush if current block is greater than old block
    if (block_minute != _current_minute) {
        flush();
        _current_minute = block_minute;
    }

    *_pending_list.add_blocks() = block;

    if (_pending_list.blocks_size() >= static_cast<int>(_max_batch)) {
        flush();
    }

    //todo: flush when reach _flush_threshold
}

void TimeBlockWriter::flush() {
    std::lock_guard<std::recursive_mutex> lock(_mtx_time);
    if (_pending_list.blocks_size() == 0) return;
    _pending_list.set_created_at(_current_minute);
    writeList(_pending_list);
    _pending_list.Clear();
}

void TimeBlockWriter::writeList(const TimeBlockList& list) {
    if (list.blocks_size() == 0) return;
    
    uint32_t size = list.ByteSizeLong();
    _data_stream.write(reinterpret_cast<const char*>(&size), sizeof(uint32_t));
    list.SerializeToOstream(&_data_stream);
    _data_stream.flush();

    // Ghi chỉ mục
    BlockListIndexEntry entry;
    entry.file_index = _current_file_index;
    entry.start_time = _current_minute;
    entry.offset = _current_offset;
    entry.count = list.blocks_size();
    _meta_stream.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    _meta_stream.flush();

    _current_offset += sizeof(uint32_t) + size;
}

void TimeBlockWriter::getCurrentIndex() {
    File::scanDir(_output_dir, [&](const string &path, bool isDir) -> bool {
        if (!isDir && end_with(path, ".s3db")) {
            auto strIndex = findSubString(path.data(), "--", ".");
            auto index = (uint32_t)(atof(strIndex.data()));
            if (index > _current_file_index) {
                _current_file_index = index;
            }
        }
        return true;
    });
}

//////////////////////////TimeBlockReader///////////////////////////////

INSTANCE_IMP(TimeBlockReader);

TimeBlockReader::TimeBlockReader() {
    GET_CONFIG(string, recordPath, Protocol::kMP4SavePath);
    GET_CONFIG(string, recordAppName, Record::kAppName);

    _output_dir = File::absolutePath(recordAppName, recordPath);
}

void TimeBlockReader::loadIndex() {
    std::lock_guard<std::recursive_mutex> lock(_mtx_time);

    _index.clear();
    std::ifstream meta(_output_dir + "/meta.idx", std::ios::binary);
    if (!meta.is_open()) {
        //todo: throw error
        throw std::runtime_error("Cannot open meta.idx");
    }

    BlockListIndexEntry entry;
    while (meta.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        _index.push_back(entry);
    }
    meta.close();
}

void TimeBlockReader::reloadIndex() {
    int64_t block_minute = static_cast<int64_t>(time(nullptr) / 60);
    if (block_minute != _last_block_minute) {
        loadIndex();
        _last_block_minute = block_minute;
    }
}

std::string TimeBlockReader::indexToFilename(uint32_t index) {
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    string file_name = mediaServerId + "--" + to_string(index) + ".s3db";
    return _output_dir + "/" + file_name;
}

TimeBlockList TimeBlockReader::readList(uint32_t file_index, uint64_t offset) {
    std::ifstream fin(indexToFilename(file_index), std::ios::binary);
    if (!fin.is_open()) {
        //todo: throw error
        throw std::runtime_error("Cannot open data file " + indexToFilename(file_index));
    }
    fin.seekg(offset);

    uint32_t size;
    fin.read(reinterpret_cast<char*>(&size), sizeof(uint32_t));

    std::string buffer(size, '\0');
    fin.read(&buffer[0], size);

    TimeBlockList list;
    list.ParseFromString(buffer);
    return list;
}

std::vector<TimeBlock> TimeBlockReader::query(uint64_t start_time, uint64_t end_time, const string &camera_id) {
    std::lock_guard<std::recursive_mutex> lock(_mtx_time);

    reloadIndex();

    std::vector<TimeBlock> result;

    for (const auto& entry : _index) {
        if (entry.start_time > end_time) continue;
        if (entry.start_time + 60 < start_time) continue;  // each list last 1 minute

        auto list = readList(entry.file_index, entry.offset);
        for (const auto& block : list.blocks()) {
            if (block.start_time() >= start_time &&
                block.start_time() <= end_time &&
                block.app() == camera_id) {
                result.push_back(block);
            }
        }
    }

    return result;
}

void TimeBlockReader::query(uint64_t start_time, uint64_t end_time, const string &camera_id, const function<void(const TimeBlock &block)> &cb) {
    std::lock_guard<std::recursive_mutex> lock(_mtx_time);

    reloadIndex();
    for (const auto& entry : _index) {
        if (entry.start_time > end_time) continue;
        if (entry.start_time + 60 < start_time) continue;  // each list last 1 minute

        auto list = readList(entry.file_index, entry.offset);
        for (const auto& block : list.blocks()) {
            if (block.start_time() >= start_time &&
                block.start_time() <= end_time &&
                block.app() == camera_id) {
                cb(block);
            }
        }
    }
}

void TimeBlockReader::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time, const string &camera_id, int period_type, int detail,
    const function<void(const SockException &ex, const Json::Value &data)> &cb) {
    Json::Value result;
    result["camera_id"] = camera_id;

    if (period_type == 1) {
        struct TimeRange {
            uint64_t startTime;
            uint32_t duration;
        };

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
            struct TimeRange {
                uint64_t startTime;
                uint32_t duration;
            };

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
        auto blocks = query(start_time, end_time, camera_id);
        for (auto const &p : blocks) {
            Json::Value json_period;
            json_period["cameraId"] = p.app();
            json_period["streamId"] = p.stream();
            json_period["startTime"] = p.start_time();
            json_period["timeLen"] = p.time_len();
            result.append(json_period);
        }
    }

    return cb(SockException(Err_success), result);
}