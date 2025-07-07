#include "Common/config.h"
#include "Record/Recorder.h"
#include "TimeQuery.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

TimeQuery::TimeQuery(const MediaTuple &tuple, const string path) {
    _file_path = path;
    if (_file_path.empty()) {
        GET_CONFIG(string, recordPath, Protocol::kTimeSavePath);
        GET_CONFIG(bool, enableVhost, General::kEnableVhost);
        if (enableVhost) {
            _file_path = tuple.vhost;
        }
        _file_path = File::absolutePath(_file_path, recordPath);
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
    auto pos_time = _demuxer->seekTo(seek_stamp);
    if (pos_time < 0) {
        return false;
    }
    // Set the current timestamp
    setCurrentStamp(pos_time);
    return true;
}

bool TimeQuery::readBlockList(uint64_t &start_stamp, uint64_t &end_stamp, const function<void(const TimeBlock &block)> &cb) {
    bool eof = false;
    while (!eof && end_stamp > getCurrentStamp()) {
        TimeBlockList list;
        _demuxer->readBlockList(list, eof);
        if (!eof) {
            // Set the current timestamp
            setCurrentStamp(list.created_at());
            for (const auto &block : list.blocks()) {
                if (block.start_time() + block.time_len() > start_stamp &&
                    block.start_time() <= end_stamp && 
                    block.app() == _tuple.app) {
                    if (_tuple.stream.empty() || _tuple.stream == block.stream()) {
                        cb(block);
                    }
                }
            };
        }
    }
    return !eof;
}

void TimeQuery::query(uint64_t &start_time, uint64_t &end_time, const std::function<void(const TimeBlock &block)> &cb) {
    if (_demuxer) {
        lock_guard<recursive_mutex> lck(_mtx);
        if (!seekTo(start_time)) {
            return;
        }
        readBlockList(start_time, end_time, cb);
    }
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(vector<TimeRange> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);
        
        vector<TimeRange> result;
        query(start_time, end_time, [&](const TimeBlock &block) {
            if (!result.empty()) {
                TimeRange &last = result.back();
                uint64_t last_end = last.startTime + last.duration;

                if (block.start_time() <= last_end + 1) {
                    uint64_t new_end = MAX(last_end, block.start_time() + block.time_len());
                    last.duration = static_cast<int>(new_end - last.startTime);
                    return;
                }
            }
            result.push_back({ block.start_time(), static_cast<uint32_t>(block.time_len()) });
        });
        cb(result);
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string /*stream*/, vector<TimeRange>> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);
        
        unordered_map<string /*stream*/, vector<TimeRange>> result;
        query(start_time, end_time, [&](const TimeBlock &block) {
            auto &range_map = result[block.stream()];

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
        cb(result);
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string /*date*/, set<int/*hour*/>> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);

        unordered_map<string /*date*/, set<int /*hour*/>> result;
        query(start_time, end_time, [&](const TimeBlock &block) {
            string date_str = getTimeStr("%Y-%m-%d", block.start_time());
            string hour_str = getTimeStr("%H", block.start_time());

            auto &hour_map = result[date_str];
            hour_map.emplace(static_cast<int>(atoi(hour_str.data())));

            auto block_end_time = block.start_time() + block.time_len();
            if (block_end_time < end_time && getStartOfHour(block.start_time()) < getStartOfHour(block_end_time)) {
                string next_date_str = getTimeStr("%Y-%m-%d", block_end_time);
                string next_hour_str = getTimeStr("%H", block_end_time);
                auto &next_hour_map = result[next_date_str];
                next_hour_map.emplace(static_cast<int>(atoi(next_hour_str.data())));
            }
        });
        cb(result);
}

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string /*stream_id*/, unordered_map<string /*date*/, unordered_map<int, std::vector<TimeRange>>>> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);

        unordered_map<string /*stream_id*/, unordered_map<string /*date*/, unordered_map<int, std::vector<TimeRange>>>> result;
        query(start_time, end_time, [&](const TimeBlock &block) {
            string stream_id = block.stream();
            uint64_t current = block.start_time();
            uint32_t remaining = block.time_len();

            while(remaining > 0) {
                string date_str = getTimeStr("%Y-%m-%d", current);
                string hour_str = getTimeStr("%H", current);
                string minute_str = getTimeStr("%M", current);
                string second_str = getTimeStr("%S", current);

                int64_t start_of_hour = getStartOfHour(current);
                uint32_t offset_in_hour = static_cast<uint32_t>(current - start_of_hour);
                uint32_t seconds_left_in_hour = 3600 - offset_in_hour;
                uint32_t chunk = MIN(remaining, seconds_left_in_hour);
                
                auto &range_map = result[stream_id][date_str][static_cast<int>(atoi(hour_str.data()))];
                auto chunk_start = current;
                if (!range_map.empty() && range_map.back().startTime + range_map.back().duration + 1 >= chunk_start) {
                    range_map.back().duration += chunk;
                } else {
                    range_map.push_back({chunk_start, chunk});
                }
                current += chunk;
                remaining -= chunk;
            }
        });
        cb(result);
    }

void TimeQuery::getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(vector<TimeBlock> &data)> &cb) {
        lock_guard<recursive_mutex> lck(_mtx);

        vector<TimeBlock> result;
        query(start_time, end_time, [&](const TimeBlock &block) {
            result.push_back(block); 
        });
        cb(result);
}

int64_t TimeQuery::getOffsetOfDate(uint64_t pos_time) {
    lock_guard<recursive_mutex> lck(_mtx);
    int64_t ret = 0;
    auto start_of_date = getStartOfDay(pos_time);
    query(start_of_date, pos_time, [&](const TimeBlock &block) {
        if (block.start_time() < start_of_date && block.start_time() + block.time_len() > start_of_date) {
            ret +=  block.start_time() + block.time_len() - start_of_date;
        } else if (block.start_time() >= start_of_date && block.start_time() + block.time_len() <= pos_time){ 
            ret += block.time_len();
        }  if (block.start_time() <= pos_time && block.start_time() + block.time_len() > pos_time) {
            ret += pos_time - block.start_time();
        }
    });
    return ret;
}

std::shared_ptr<TimeBlock> TimeQuery::getLastBlock(uint32_t interval_sec) {
    lock_guard<recursive_mutex> lck(_mtx);
    uint64_t pos = time(nullptr);
    bool has_block = false;
    std::shared_ptr<TimeBlock> last_block;
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
    return has_block ? last_block : nullptr;
}

} // namespace mediakit