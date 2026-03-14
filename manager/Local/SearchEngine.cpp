#include "SearchEngine.h"

#include "Common/config.h"
#include "Util/logger.h"

#include <set>
#include <vector>
#include <unordered_map>

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

void SearchEngine::findTimePeriod(
    const MediaTuple &tuple,
    uint64_t start_time, uint64_t end_time,
    int period_type, int detail, bool include_motion,
    const function<void(const SockException &, const Value &)> &cb)
{
    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    Value result;

    TimeQuery::Ptr query;
    try {
        query = std::make_shared<TimeQuery>(tuple);
    } catch (const std::exception &ex) {
        WarnL << "TimeQuery init failed: " << ex.what();
    }

    if (period_type == 1) {
        if (detail == 0) {
            result["cameraId"] = tuple.app;
            result["periods"] = arrayValue;
            if (query) {
                query->getRecordedTimePeriod(start_time, end_time, [&](vector<TimeRange> &ret) {
                    for (auto const &p : ret) {
                        Value period;
                        period["startTime"] = p.startTime;
                        period["duration"] = p.duration;
                        period["mediaServerId"] = mediaServerId;
                        result["periods"].append(period);
                    }
                });
            }
            if (include_motion) {
                result["motionPeriods"] = arrayValue;
                try {
                    MotionSearch ms(tuple);
                    ms.getMotionTimePeriod(start_time, end_time, [&](vector<MotionTimeRange> &ret) {
                        for (auto const &p : ret) {
                            Value period;
                            period["startTime"] = (Json::UInt64)p.startTime;
                            period["duration"] = p.duration;
                            result["motionPeriods"].append(period);
                        }
                    });
                } catch (...) {}
            }
        } else {
            result["cameraId"] = tuple.app;
            result["streams"] = arrayValue;
            if (query) {
                query->getRecordedTimePeriod(start_time, end_time,
                    [&](unordered_map<string, vector<TimeRange>> &ret) {
                        for (auto const &it : ret) {
                            Value stream;
                            stream["streamId"] = it.first;
                            stream["periods"] = arrayValue;
                            for (auto const &p : it.second) {
                                Value period;
                                period["startTime"] = p.startTime;
                                period["duration"] = p.duration;
                                period["mediaServerId"] = mediaServerId;
                                stream["periods"].append(period);
                            }
                            result["streams"].append(stream);
                        }
                    });
            }
            if (include_motion) {
                result["motionPeriods"] = arrayValue;
                try {
                    MotionSearch ms(tuple);
                    ms.getMotionTimePeriod(start_time, end_time, [&](vector<MotionTimeRange> &ret) {
                        for (auto const &p : ret) {
                            Value period;
                            period["startTime"] = (Json::UInt64)p.startTime;
                            period["duration"] = p.duration;
                            result["motionPeriods"].append(period);
                        }
                    });
                } catch (...) {}
            }
        }
    } else if (period_type == 2) {
        if (detail == 0) {
            result["cameraId"] = tuple.app;
            result["periods"] = objectValue;
            if (query) {
                query->getRecordedTimePeriod(start_time, end_time,
                    [&](unordered_map<string, set<int>> &ret) {
                        for (auto const &it : ret) {
                            const string &date_str = it.first;
                            const auto &hour_set = it.second;
                            result["periods"][date_str] = arrayValue;
                            for (int i = 0; i < 24; i++) {
                                result["periods"][date_str].append(
                                    hour_set.count(i) ? 1 : 0);
                            }
                        }
                    });
            }
            if (include_motion) {
                result["motionPeriods"] = objectValue;
                try {
                    MotionSearch ms(tuple);
                    ms.getMotionTimePeriod(start_time, end_time,
                        [&](unordered_map<string, set<int>> &ret) {
                            for (auto const &it : ret) {
                                const string &date_str = it.first;
                                const auto &hour_set = it.second;
                                result["motionPeriods"][date_str] = arrayValue;
                                for (int i = 0; i < 24; i++) {
                                    result["motionPeriods"][date_str].append(
                                        hour_set.count(i) ? 1 : 0);
                                }
                            }
                        });
                } catch (...) {}
            }
        } else {
            result["cameraId"] = tuple.app;
            result["streams"] = arrayValue;
            if (query) {
                query->getRecordedTimePeriod(start_time, end_time,
                    [&](unordered_map<string, unordered_map<string, unordered_map<int, vector<TimeRange>>>> &ret) {
                        for (auto const &it_stream : ret) {
                            Value stream;
                            stream["streamId"] = it_stream.first;
                            for (auto const &it_date : it_stream.second) {
                                const string &date_str = it_date.first;
                                const auto &hour_map = it_date.second;
                                Value date;
                                for (int i = 0; i < 24; i++) {
                                    Value hour = arrayValue;
                                    auto it_hour = hour_map.find(i);
                                    if (it_hour != hour_map.end()) {
                                        for (auto const &p : it_hour->second) {
                                            Value period;
                                            period["startTime"] = p.startTime;
                                            period["duration"] = p.duration;
                                            period["mediaServerId"] = mediaServerId;
                                            hour.append(period);
                                        }
                                    }
                                    date.append(hour);
                                }
                                stream["dates"][date_str] = date;
                            }
                            result["streams"].append(stream);
                        }
                    });
            }
            if (include_motion) {
                result["motionPeriods"] = objectValue;
                try {
                    MotionSearch ms(tuple);
                    ms.getMotionTimePeriod(start_time, end_time,
                        [&](unordered_map<string, unordered_map<int, vector<MotionTimeRange>>> &ret) {
                            for (auto const &it_date : ret) {
                                const string &date_str = it_date.first;
                                const auto &hour_map = it_date.second;
                                Value date;
                                for (int i = 0; i < 24; i++) {
                                    Value hour = arrayValue;
                                    auto it_hour = hour_map.find(i);
                                    if (it_hour != hour_map.end()) {
                                        for (auto const &p : it_hour->second) {
                                            Value period;
                                            period["startTime"] = (Json::UInt64)p.startTime;
                                            period["duration"] = p.duration;
                                            hour.append(period);
                                        }
                                    }
                                    date.append(hour);
                                }
                                result["motionPeriods"][date_str] = date;
                            }
                        });
                } catch (...) {}
            }
        }
    } else if (period_type == 0) {
        result["periods"] = arrayValue;
        if (query) {
            query->getRecordedTimePeriod(start_time, end_time, [&](vector<TimeBlock> &ret) {
                for (auto const &p : ret) {
                    Value period;
                    period["cameraId"] = p.app();
                    period["streamId"] = p.stream();
                    period["startTime"] = p.start_time();
                    period["timeLen"] = p.time_len();
                    period["mediaServerId"] = mediaServerId;
                    result["periods"].append(period);
                }
            });
        }
        if (include_motion) {
            result["motionPeriods"] = arrayValue;
            try {
                MotionSearch ms(tuple);
                ms.getMotionTimePeriod(start_time, end_time, [&](vector<MotionTimeRange> &ret) {
                    for (auto const &p : ret) {
                        Value period;
                        period["startTime"] = (Json::UInt64)p.startTime;
                        period["duration"] = p.duration;
                        result["motionPeriods"].append(period);
                    }
                });
            } catch (...) {}
        }
    }

    return cb(SockException(Err_success), result);
}

} // namespace managerkit
