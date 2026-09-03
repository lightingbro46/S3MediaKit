#include "StatusGapSearch.h"
#include "Extension/Resource.h"

#include <algorithm>
#include <map>
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace {

static std::map<int, std::string> getStreamQuality(const mediakit::MediaTuple &tuple) {
    std::map<int, std::string> quality_map;
    Broadcast::StreamQualityInvoker invoker = [&](const std::map<int, std::string> &streams) {
        quality_map = streams;
    };
    NOTICE_EMIT(BroadcastGetStreamQualityArgs, Broadcast::kBroadcastGetStreamQuality, tuple.vhost, tuple.app, invoker);
    return quality_map;
}

}

static std::vector<LocalResourceStatusGap> getStatusGaps(const mediakit::MediaTuple &tuple,
                                                         uint64_t start_time,
                                                         uint64_t end_time) {
    if (start_time >= end_time) return {};
    std::vector<std::string> resource_ids;
    for (const auto &entry : getStreamQuality(tuple)) {
        if (!entry.second.empty()) resource_ids.emplace_back(entry.second);
    }
    return ResourceStatusManager::Instance().getResourceStatusGaps(resource_ids, start_time, end_time);
}

static bool clipGap(const LocalResourceStatusGap &row, uint64_t start_time, uint64_t end_time,
                    uint64_t &start, uint64_t &end) {
    if (row.state != static_cast<int>(ResourceStatus::OFFLINE) || row.disconnect_time == 0) return false;
    start = std::max(start_time, row.disconnect_time);
    end = row.reconnect_time == 0 ? end_time : std::min(end_time, row.reconnect_time);
    return start < end;
}

void StatusGapSearch::mergeInto(std::vector<StatusGap> &list, const std::string &stream_id,
                                uint64_t start_time, uint64_t end_time,
                                 int status, const std::string &reason) {
    if (start_time >= end_time) return;
    if (!list.empty()) {
        auto &last = list.back();
        const uint64_t last_end = last.startTime + last.duration;
        if (start_time <= last_end + 1 && last.stream_id == stream_id &&
            last.status == status && last.reason == reason) {
            last.duration = static_cast<uint32_t>(std::max(last_end, end_time) - last.startTime);
            return;
        }
    }
    StatusGap gap;
    gap.stream_id = stream_id;
    gap.startTime = start_time;
    gap.duration = static_cast<uint32_t>(end_time - start_time);
    gap.status = status;
    gap.reason = reason;
    list.push_back(gap);
}

void StatusGapSearch::getRecordedStatusGaps(uint64_t start_time, uint64_t end_time,
    const std::function<void(std::vector<StatusGap> &)> &cb) {
    std::vector<StatusGap> result;
    for (const auto &row : getStatusGaps(_tuple, start_time, end_time)) {
        uint64_t start = 0, end = 0;
        if (clipGap(row, start_time, end_time, start, end)) {
            mergeInto(result, row.resource_id, start, end, row.state, row.disconnect_reason);
        }
    }
    cb(result);
}

void StatusGapSearch::getRecordedStatusGaps(uint64_t start_time, uint64_t end_time,
    const std::function<void(std::unordered_map<std::string, std::vector<StatusGap>> &)> &cb) {
    std::unordered_map<std::string, std::vector<StatusGap>> result;
    for (const auto &row : getStatusGaps(_tuple, start_time, end_time)) {
        uint64_t start = 0, end = 0;
        if (clipGap(row, start_time, end_time, start, end)) {
            mergeInto(result[row.resource_id], row.resource_id, start, end, row.state, row.disconnect_reason);
        }
    }
    cb(result);
}

void StatusGapSearch::getRecordedStatusGaps(uint64_t start_time, uint64_t end_time,
    const std::function<void(std::unordered_map<std::string, std::unordered_map<std::string,
                             std::unordered_map<int, std::vector<StatusGap>>>> &)> &cb) {
    std::unordered_map<std::string, std::unordered_map<std::string,
        std::unordered_map<int, std::vector<StatusGap>>>> result;
    for (const auto &row : getStatusGaps(_tuple, start_time, end_time)) {
        uint64_t start = 0, end = 0;
        if (!clipGap(row, start_time, end_time, start, end)) continue;
        uint64_t current = start;
        while (current < end) {
            const uint64_t hour_end = std::min(end, StampUtils::getStartOfHour(current) + 3600);
            const std::string date = getTimeStr("%Y-%m-%d", static_cast<time_t>(current));
            const int hour = atoi(getTimeStr("%H", static_cast<time_t>(current)).c_str());
            mergeInto(result[row.resource_id][date][hour], row.resource_id, current, hour_end,
                      row.state, row.disconnect_reason);
            current = hour_end;
        }
    }
    cb(result);
}

} // namespace managerkit
