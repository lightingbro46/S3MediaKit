#include "MotionSearch.h"

#include "Common/config.h"
#include "Common/StrUtil.h"
#include "Util/logger.h"
#include "Util/util.h"

#include <algorithm>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

MotionSearch::MotionSearch(const MediaTuple &tuple, const string &base_path)
    : _tuple(tuple) {
    if (!base_path.empty()) {
        _base_path = base_path;
    } else {
        GET_CONFIG(string, record_path, Protocol::kMP4SavePath);
        GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
        if (enable_vhost) {
            _base_path = record_path + "/motion/" + tuple.vhost + '/' + tuple.app + '/';
        } else {
            _base_path = record_path + "/motion/" + tuple.app + '/';
        }
    }

    _demuxer = make_shared<MultiMotionDemuxer>();
    _demuxer->openDirectory(_base_path);
}

void MotionSearch::query(uint64_t start_ms, uint64_t end_ms,
                          const function<void(uint64_t, uint64_t)> &cb) {
    if (!_demuxer || _demuxer->isEmpty()) return;

    auto intervals = _demuxer->getMotionIntervals(start_ms, end_ms);
    for (const auto &iv : intervals) {
        uint64_t iv_start = max(iv.start_ms, start_ms) / 1000;
        uint64_t iv_end   = min(iv.end_ms,   end_ms)   / 1000;
        if (iv_start >= iv_end) continue;
        cb(iv_start, iv_end);
    }
}

void MotionSearch::mergeInto(vector<MotionTimeRange> &list,
                               uint64_t seg_start, uint64_t seg_end) {
    if (!list.empty()) {
        MotionTimeRange &last      = list.back();
        const uint64_t   last_end  = last.startTime + last.duration;
        if (seg_start <= last_end + 1) {
            last.duration = static_cast<uint32_t>(max(last_end, seg_end) - last.startTime);
            return;
        }
    }
    list.push_back({seg_start, static_cast<uint32_t>(seg_end - seg_start)});
}

void MotionSearch::getMotionTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(vector<MotionTimeRange> &)> &cb) {
    lock_guard<recursive_mutex> lck(_mtx);

    vector<MotionTimeRange> result;
    try {
        query(start_time * 1000, end_time * 1000,
              [&](uint64_t iv_start, uint64_t iv_end) {
                  mergeInto(result, iv_start, iv_end);
              });
    } catch (...) {}

    cb(result);
}

void MotionSearch::getMotionTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string, set<int>> &)> &cb) {
    lock_guard<recursive_mutex> lck(_mtx);

    unordered_map<string, set<int>> result;

    // Pre-populate every date key in the range so callers get a full calendar.
    uint64_t date = StampUtils::getStartOfDay(start_time);
    while (date < end_time) {
        result[getTimeStr("%Y-%m-%d", static_cast<time_t>(date))];
        date += 86400;
    }

    try {
        query(start_time * 1000, end_time * 1000,
              [&](uint64_t iv_start, uint64_t iv_end) {
                  const string date_str = getTimeStr("%Y-%m-%d", static_cast<time_t>(iv_start));
                  const string hour_str = getTimeStr("%H",       static_cast<time_t>(iv_start));
                  result[date_str].emplace(atoi(hour_str.c_str()));

                  if (StampUtils::getStartOfHour(iv_start) < StampUtils::getStartOfHour(iv_end) &&
                      StampUtils::getStartOfHour(iv_end) < end_time) {
                      const string end_date_str = getTimeStr("%Y-%m-%d", static_cast<time_t>(iv_end));
                      const string end_hour_str = getTimeStr("%H",       static_cast<time_t>(iv_end));
                      result[end_date_str].emplace(atoi(end_hour_str.c_str()));
                  }
              });
    } catch (...) {}

    cb(result);
}

void MotionSearch::getMotionTimePeriod(uint64_t start_time, uint64_t end_time,
    const function<void(unordered_map<string, unordered_map<int, vector<MotionTimeRange>>> &)> &cb) {
    lock_guard<recursive_mutex> lck(_mtx);

    unordered_map<string, unordered_map<int, vector<MotionTimeRange>>> result;

    try {
        query(start_time * 1000, end_time * 1000,
              [&](uint64_t iv_start, uint64_t iv_end) {
                  uint64_t current   = iv_start;
                  uint32_t remaining = static_cast<uint32_t>(iv_end - iv_start);

                  while (remaining > 0) {
                      const string   date_str       = getTimeStr("%Y-%m-%d", static_cast<time_t>(current));
                      const string   hour_str       = getTimeStr("%H",       static_cast<time_t>(current));
                      const int      hour           = atoi(hour_str.c_str());
                      const uint32_t offset_in_hour = static_cast<uint32_t>(
                          current - StampUtils::getStartOfHour(current));
                      const uint32_t left_in_hour   = 3600 - offset_in_hour;
                      const uint32_t chunk          = min(remaining, left_in_hour);

                      mergeInto(result[date_str][hour], current, current + chunk);

                      current   += chunk;
                      remaining -= chunk;
                  }
              });
    } catch (...) {}

    cb(result);
}

} // namespace managerkit
