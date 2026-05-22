#if defined(ENABLE_MOTION)

#include "MotionSearch.h"

#include "Motion/MotionBitmap.h"
#include "Common/config.h"
#include "Common/StrUtil.h"
#include "StatisticRecorder.h"
#include "Util/logger.h"
#include "Util/util.h"

#include <algorithm>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

MotionSearch::MotionSearch(const MediaTuple &tuple, const string &base_path, bool use_statistic)
    : _tuple(tuple), _use_statistic(use_statistic) {
    if (!base_path.empty()) {
        _base_path = base_path;
    } else {
        GET_CONFIG(string, record_path, Protocol::kMP4SavePath);
        GET_CONFIG(string, app_name, Record::kAppName);
        auto _record_path = File::absolutePath(app_name, record_path);
        GET_CONFIG(string, archive_name, Record::kArchiveName);
        GET_CONFIG(bool, enable_vhost, General::kEnableVhost);
        if (enable_vhost) {
            _base_path = _record_path + '/' + tuple.vhost + '/' + tuple.app + '/' + archive_name + "/motion/";
        } else {
            _base_path = _record_path + '/' + tuple.app + '/' + archive_name + "/motion/";
        }
    }

    _demuxer = make_shared<MultiMotionDemuxer>();
    _demuxer->openDir(_base_path);
}

void MotionSearch::query(uint64_t start_ms, uint64_t end_ms,
                          const function<void(uint64_t, uint64_t)> &cb) {
    if (!_demuxer || _demuxer->isEmpty()) return;

    if (_use_statistic) {
        auto norm = StatisticRecorder::Instance().normalizeMotionTimeRange(
            _tuple.app, start_ms / 1000, end_ms / 1000);
        if (!norm.isValid()) return;
        start_ms = norm.start * 1000;
        end_ms   = norm.end   * 1000;
        DebugL << "Normalized motion time range: " << getTimeStr("%Y-%m-%d %H:%M:%S", norm.start) << " - " << getTimeStr("%Y-%m-%d %H:%M:%S", norm.end);
    }

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

bool MotionSearch::matchesRoi(const MotionInterval &iv, const string &roi_mask) {
    if (roi_mask.empty() || iv.bitmap.empty()) return true;
    const int cells = iv.rows * iv.cols;
    if (cells == 0) return true;

    // Convert roi_mask string ('0'/'1' per cell) into a bit-packed MotionBitmap
    std::vector<uint8_t> roi_cells(cells, 0);
    for (int i = 0; i < cells && i < static_cast<int>(roi_mask.size()); ++i)
        roi_cells[i] = (roi_mask[i] == '1') ? 1u : 0u;
    auto roi_bmp = mediakit::MotionBitmapHelper::createMotionBitmap(iv.rows, iv.cols, roi_cells.data());
    if (!roi_bmp) return true;

    // Wrap iv.bitmap (raw bit-packed bytes) as a MotionBitmap for intersection check
    const size_t expected_bytes = static_cast<size_t>((cells + 7) / 8);
    if (iv.bitmap.size() < expected_bytes) return true; // malformed data, allow through

    std::vector<uint8_t> buf(sizeof(mediakit::MotionBitmap) + iv.bitmap.size(), 0);
    mediakit::MotionBitmap *motion_bmp = reinterpret_cast<mediakit::MotionBitmap *>(buf.data());
    motion_bmp->rows         = iv.rows;
    motion_bmp->cols         = iv.cols;
    motion_bmp->active_cells = iv.active_cells;
    std::memcpy(motion_bmp->bitmap, iv.bitmap.data(), iv.bitmap.size());

    return mediakit::MotionBitmapHelper::intersectMotionBitmaps(motion_bmp, roi_bmp.get());
}

void MotionSearch::getMotionTimePeriodByRoi(uint64_t start_time, uint64_t end_time,
    const string &roi_mask,
    const function<void(vector<MotionTimeRange> &)> &cb) {
    lock_guard<recursive_mutex> lck(_mtx);

    vector<MotionTimeRange> result;
    if (!_demuxer || _demuxer->isEmpty()) {
        cb(result);
        return;
    }

    try {
        auto intervals = _demuxer->getMotionIntervals(start_time * 1000, end_time * 1000);
        for (const auto &iv : intervals) {
            if (!matchesRoi(iv, roi_mask)) continue;
            uint64_t iv_start = max(iv.start_ms, start_time * 1000) / 1000;
            uint64_t iv_end   = min(iv.end_ms,   end_time   * 1000) / 1000;
            if (iv_start >= iv_end) continue;
            mergeInto(result, iv_start, iv_end);
        }
    } catch (...) {}

    cb(result);
}

} // namespace managerkit

#endif // ENABLE_MOTION
