#include "Common/config.h"
#include "Record/Recorder.h"
#include "TimeQuery.h"
#include "Common/StrUtil.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

TimeQuery::TimeQuery(const MediaTuple &tuple, const string &path) {
    _file_path = path;
    if (_file_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kMP4SavePath)
        GET_CONFIG(string, appName, Record::kAppName)
        _file_path = File::absolutePath(appName, recordPath);
        CHECK(!tuple.app.empty(), "Device id empty!");
        _file_path += "/" + tuple.app;
    }

    _demuxer = std::make_shared<MultiTimeDemuxer>();
    _demuxer->openFile(_file_path);

    _tuple = tuple;
};

TimeQuery::~TimeQuery() {
    if (_demuxer) {
        _demuxer->closeFile();
        _demuxer = nullptr;
    }
}

bool TimeQuery::seekTo(uint64_t seek_stamp) {
    GET_CONFIG(uint64_t, max_second, Protocol::kMP4MaxSecond);
    auto efficient_seek_stamp = seek_stamp > (max_second * 2) ? seek_stamp - (max_second * 2) : 0;
    auto pos_time = _demuxer->seekTo(efficient_seek_stamp);
    if (pos_time < 0) {
        return false;
    }
    // Set the current timestamp
    setCurrentStamp(pos_time);
    return true;
}

bool TimeQuery::readBlockList(uint64_t &start_stamp, uint64_t &end_stamp, const TimeBlockImp &cb) {
    bool eof = false;
    GET_CONFIG(uint64_t, max_second, Protocol::kMP4MaxSecond);
    auto efficient_end_stamp = end_stamp + (max_second * 2);
    while (!eof && efficient_end_stamp > getCurrentStamp()) {
        TimeBlock block;
        _demuxer->readBlock(block, eof);
        if (!eof) {
            if (_tuple.app.empty() || _tuple.app == block.app()) {
                if (_tuple.stream.empty() || _tuple.stream == block.stream()) {
                    // Set the current timestamp
                    setCurrentStamp(block.start_time());
                    if (block.start_time() + block.time_len() > start_stamp &&
                        block.start_time() < end_stamp) {
                        cb(block);
                    }
                }
            }
        }
    }
    return !eof;
}

void TimeQuery::query(uint64_t &start_time, uint64_t &end_time, const TimeBlockImp &cb) {
    lock_guard<recursive_mutex> lck(_mtx);
    if (_demuxer) {
        if (!seekTo(start_time)) {
            return;
        }
        // Blocks from multiple streams arrive interleaved (not sorted by start_time).
        // Collect all, sort by start_time, then invoke cb so merge logic in callers
        // (which only looks at the last element) always works correctly.
        vector<TimeBlock> collected;
        readBlockList(start_time, end_time, [&](const TimeBlock &block) {
            collected.push_back(block);
        });
        sort(collected.begin(), collected.end(), [](const TimeBlock &a, const TimeBlock &b) {
            return a.start_time() < b.start_time();
        });
        for (auto &block : collected) {
            cb(block);
        }
    }
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(vector<TimeRange> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);
        
        vector<TimeRange> result;
        try {
            query(start_time, end_time, [&](const TimeBlock &block) {
                auto block_start_time = block.start_time();
                if (block_start_time < start_time) {
                    block_start_time = start_time;
                }
                auto block_end_time = block.start_time() + block.time_len();
                if (block_end_time > end_time) {
                    block_end_time = end_time;
                }
                if (!result.empty()) {
                    TimeRange &last = result.back();
                    uint64_t last_start = last.startTime;
                    uint64_t last_end = last.startTime + last.duration;

                    if (block_start_time <= last_end + 1) {
                        uint64_t new_start = MIN(last_start, block_start_time);
                        uint64_t new_end = MAX(last_end, block_end_time);
                        last.startTime = new_start;
                        last.duration = static_cast<int>(new_end - last.startTime);
                        return;
                    }
                }
                result.push_back({ block_start_time, static_cast<uint32_t>(block_end_time - block_start_time) });
            });
        } catch(...) {}
        
        cb(result);
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string /*stream*/, vector<TimeRange>> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);
        
        unordered_map<string /*stream*/, vector<TimeRange>> result;
        try {
            query(start_time, end_time, [&](const TimeBlock &block) {
                auto block_start_time = block.start_time();
                if (block_start_time < start_time) {
                    block_start_time = start_time;
                }
                auto block_end_time = block.start_time() + block.time_len();
                if (block_end_time > end_time) {
                    block_end_time = end_time;
                }
                auto &range_map = result[block.stream()];

                if (!range_map.empty()) {
                    TimeRange &last = range_map.back();
                    uint64_t last_start = last.startTime;
                    uint64_t last_end = last.startTime + last.duration;

                    if (block_start_time <= last_end + 1) {
                        uint64_t new_start = MIN(last_start, block_start_time);
                        uint64_t new_end = MAX(last_end, block_end_time);
                        last.startTime = new_start;
                        last.duration = static_cast<int>(new_end - last.startTime);
                        return;
                    }
                }
                range_map.push_back({ block_start_time, static_cast<uint32_t>(block_end_time - block_start_time)});
            });
        } catch (...) {}
       
        cb(result);
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string /*date*/, set<int/*hour*/>> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);

        unordered_map<string /*date*/, set<int /*hour*/>> result;
        auto start_date = StampUtils::getStartOfDay(start_time);
        auto date = start_date;
        while (date < end_time) {
            string date_str = getTimeStr("%Y-%m-%d", date);
            result[date_str];
            date += 86400; // start time of next date
        }

        try {
            query(start_time, end_time, [&](const TimeBlock &block) {
                auto block_start_time = block.start_time();
                if (block_start_time < start_time) {
                    block_start_time = start_time;
                }
                string date_str = getTimeStr("%Y-%m-%d", block_start_time);
                string hour_str = getTimeStr("%H", block_start_time);

                auto &hour_map = result[date_str];
                hour_map.emplace(static_cast<int>(atoi(hour_str.data())));

                auto block_end_time = block.start_time() + block.time_len();
                if (block_end_time > end_time) {
                    block_end_time = end_time;
                }
                if (StampUtils::getStartOfHour(block_start_time) < StampUtils::getStartOfHour(block_end_time) && StampUtils::getStartOfHour(block_end_time) < end_time) {
                    string next_date_str = getTimeStr("%Y-%m-%d", block_end_time);
                    string next_hour_str = getTimeStr("%H", block_end_time);
                    auto &next_hour_map = result[next_date_str];
                    next_hour_map.emplace(static_cast<int>(atoi(next_hour_str.data())));
                }
            });
        } catch (...) {}

        cb(result);
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string /*stream_id*/, unordered_map<string /*date*/, unordered_map<int, std::vector<TimeRange>>>> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);

        unordered_map<string /*stream_id*/, unordered_map<string /*date*/, unordered_map<int, std::vector<TimeRange>>>> result;
        try {
            query(start_time, end_time, [&](const TimeBlock &block) {
                string stream_id = block.stream();
                if (result.find(stream_id) == result.end()) {
                    auto start_date = StampUtils::getStartOfDay(start_time);
                    auto date = start_date;
                    while (date < end_time) {
                        string date_str = getTimeStr("%Y-%m-%d", date);
                        result[stream_id][date_str];
                        date += 86400; // start time of next date
                    }
                }
                
                auto block_start_time = block.start_time();
                if (block_start_time < start_time) {
                    block_start_time = start_time;
                }

                auto block_end_time = block.start_time() + block.time_len();
                if (block_end_time > end_time) {
                    block_end_time = end_time;
                }

                uint64_t current = block_start_time;
                uint32_t remaining = block_end_time - block_start_time;

                while(remaining > 0) {
                    string date_str = getTimeStr("%Y-%m-%d", current);
                    string hour_str = getTimeStr("%H", current);
                    string minute_str = getTimeStr("%M", current);
                    string second_str = getTimeStr("%S", current);

                    int64_t start_of_hour = StampUtils::getStartOfHour(current);
                    uint32_t offset_in_hour = static_cast<uint32_t>(current - start_of_hour);
                    uint32_t seconds_left_in_hour = 3600 - offset_in_hour;
                    uint32_t chunk = MIN(remaining, seconds_left_in_hour);
                    
                    auto &range_map = result[stream_id][date_str][static_cast<int>(atoi(hour_str.data()))];
                    auto chunk_start = current;
                    if (!range_map.empty() && range_map.back().startTime + range_map.back().duration + 1 >= chunk_start) {
                        uint64_t new_start = MIN(range_map.back().startTime, chunk_start);
                        uint64_t new_end = MAX(range_map.back().startTime + range_map.back().duration, chunk_start + chunk);
                        range_map.back().startTime = new_start;
                        range_map.back().duration = static_cast<uint32_t>(new_end - range_map.back().startTime);
                    } else {
                        range_map.push_back({chunk_start, chunk});
                    }
                    current += chunk;
                    remaining -= chunk;
                }
            });
        } catch (...) {}
        
        cb(result);
    }

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(vector<TimeBlock> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);

        vector<TimeBlock> result;
        try {
            query(start_time, end_time, [&](const TimeBlock &block) {
                result.push_back(block); 
            });
        } catch (...) {}
        
        cb(result);
}

int64_t TimeQuery::getOffsetOfDate(uint64_t pos_time) {
    lock_guard<recursive_mutex> lck(_mtx);
    int64_t ret = 0;
    try {
        auto start_of_date = StampUtils::getStartOfDay(pos_time);
        query(start_of_date, pos_time, [&](const TimeBlock &block) {
            auto block_start_time = block.start_time();
            if (block_start_time < start_of_date) {
                block_start_time = start_of_date;
            }
            auto block_end_time = block.start_time() + block.time_len();
            if (block_end_time > pos_time) {
                block_end_time = pos_time;
            }

            ret += (block_end_time - block_start_time);
        });
    } catch (...) {}
    
    return ret;
}

std::shared_ptr<TimeBlock> TimeQuery::getLastBlock(uint64_t last_archived_time, uint32_t interval_sec) {
    lock_guard<recursive_mutex> lck(_mtx);
    uint64_t pos = last_archived_time ? last_archived_time : time(nullptr);
    bool has_block = false;
    std::shared_ptr<TimeBlock> last_block;
    try {
        uint64_t first_time = _demuxer->getFirstStamp();
        while (!has_block && pos > first_time) {
            auto start_pos = pos - interval_sec;
            query(start_pos, pos, [&](const TimeBlock &block) { 
                has_block = true;
                last_block = std::make_shared<TimeBlock>(block);
            });
            if (!has_block) {
                pos = start_pos;
            }
        }
    } catch (...) {}
    
    return has_block ? last_block : nullptr;
}

std::shared_ptr<TimeBlock> TimeQuery::getFirstBlock(uint64_t first_archived_time, uint32_t interval_sec) {
    lock_guard<recursive_mutex> lck(_mtx);
    uint64_t last_time = time(nullptr);
    bool has_block = false;
    std::shared_ptr<TimeBlock> first_block;
    try {
        uint64_t pos = first_archived_time ? first_archived_time : _demuxer->getFirstStamp();
        while (!has_block && pos > last_time) {
            auto end_pos = pos + interval_sec;
            query(pos, end_pos, [&](const TimeBlock &block) {
                if (!has_block) {
                    has_block = true;
                    first_block = std::make_shared<TimeBlock>(block);
                }
            });
            if (!has_block) {
                pos = end_pos;
            }
        }
    } catch (...) {}
    
    return has_block ? first_block : nullptr;
}

} // namespace managerkit