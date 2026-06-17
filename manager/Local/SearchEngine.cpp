#include "SearchEngine.h"

#include "Common/config.h"
#include "Util/logger.h"
#include "Util/NoticeCenter.h"
#include "server/WebApiErrCode.h"
#include "Storage/VmsResourceAssignment.h"
#include "Storage/BookmarkIndex.h"
#include "Storage/Bookmark.h"
#include "Storage/UserEntity.h"
#include "Server/ClusterManager.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// ---------------------------------------------------------------------------
// Internal helper: resolve owner peer from VmsResourceAssignment.
// Returns {peer_id, peer_url}; peer_url is empty when owner is the local node.
// ---------------------------------------------------------------------------
static std::pair<std::string, std::string> resolveOwner(
    const std::string &camera_id,
    const std::vector<VmsResourceAssignment> &assignments)
{
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    if (assignments.empty()) return {"", ""};
    // Pick the most recently started assignment.
    const VmsResourceAssignment *best = &assignments[0];
    for (const auto &a : assignments) {
        if (a.assigned_at > best->assigned_at) best = &a;
    }
    if (best->owner_peer_id == mediaServerId || best->owner_peer_id.empty())
        return {"", ""};
    string peer_url = ClusterManager::Instance().getPeerUrl(best->owner_peer_id);
    return {best->owner_peer_id, peer_url};
}

std::pair<std::string, std::string> SearchEngine::findOwnerNodeForBookmark(
    const std::string &bookmark_guid)
{
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);
    BookmarkIndexImp idx_imp;
    auto entries = idx_imp.findByBookmarkGuid(bookmark_guid);
    if (entries.empty()) return {"", ""};
    const string &owner_id = entries[0].owner_peer_id;
    if (owner_id == mediaServerId || owner_id.empty()) return {"", ""};
    string peer_url = ClusterManager::Instance().getPeerUrl(owner_id);
    return {owner_id, peer_url};
}

std::pair<std::string, std::string> SearchEngine::findOwnerNodeAtTime(
    const std::string &camera_id, int64_t pos_time)
{
    VmsResourceAssignmentImp assign_imp;
    auto assignments = assign_imp.findAssignmentsOverlappingRange(camera_id, pos_time, pos_time);
    return resolveOwner(camera_id, assignments);
}

std::pair<std::string, std::string> SearchEngine::findCurrentOwnerNode(
    const std::string &camera_id)
{
    VmsResourceAssignmentImp assign_imp;
    auto assignments = assign_imp.findCurrentAssignment(camera_id);
    return resolveOwner(camera_id, assignments);
}

// ---------------------------------------------------------------------------
// Helpers for overlap-resolution + consecutive-merge of recorded periods.
//
// Overlap rule: when two periods from different servers overlap, the one with
// the LATER startTime wins the overlapping portion (it represents more recent
// recording that took over from the earlier server).  After resolving overlaps,
// consecutive periods that share the same mediaServerId are coalesced.
// ---------------------------------------------------------------------------
struct PeriodEntry {
    uint64_t    start;
    uint64_t    end;        // exclusive (start + duration / start + timeLen)
    std::string serverId;
    // type-0 only
    std::string cameraId;
    std::string streamId;
};

struct MotionPeriodEntry {
    uint64_t start;
    uint64_t end;           // exclusive
};

// Resolve overlaps (later startTime wins) then merge consecutive same-server periods.
static std::vector<PeriodEntry> resolvePeriods(std::vector<PeriodEntry> inp) {
    if (inp.empty()) return {};
    std::sort(inp.begin(), inp.end(), [](const PeriodEntry &a, const PeriodEntry &b) {
        return a.start < b.start;
    });

    std::vector<PeriodEntry> result;
    for (size_t i = 0; i < inp.size(); ++i) {
        PeriodEntry cur = inp[i];
        if (cur.start >= cur.end) continue;

        if (!result.empty() && result.back().end > cur.start) {
            // cur starts inside the last result period → cur wins the overlap.
            PeriodEntry prev_copy = result.back();
            uint64_t    prev_old_end = prev_copy.end;

            result.back().end = cur.start;          // trim prev up to cur.start
            if (result.back().end <= result.back().start)
                result.pop_back();                  // prev became zero-duration

            result.push_back(cur);

            // If prev extended beyond cur.end, re-inject its tail so it is
            // processed in the correct sorted position.
            if (prev_old_end > cur.end) {
                PeriodEntry tail  = prev_copy;
                tail.start        = cur.end;
                tail.end          = prev_old_end;
                size_t ins        = i + 1;
                while (ins < inp.size() && inp[ins].start < tail.start) ++ins;
                inp.insert(inp.begin() + ins, tail);
            }
        } else {
            result.push_back(cur);
        }
    }

    // Merge consecutive periods that share the same server.
    std::vector<PeriodEntry> merged;
    for (const auto &p : result) {
        if (!merged.empty() &&
            merged.back().serverId == p.serverId &&
            merged.back().end      == p.start) {
            merged.back().end = p.end;
        } else {
            merged.push_back(p);
        }
    }
    return merged;
}

// Merge overlapping / adjacent motion periods (no server concept).
static std::vector<MotionPeriodEntry> resolveMotionPeriods(std::vector<MotionPeriodEntry> inp) {
    if (inp.empty()) return {};
    std::sort(inp.begin(), inp.end(), [](const MotionPeriodEntry &a, const MotionPeriodEntry &b) {
        return a.start < b.start;
    });
    std::vector<MotionPeriodEntry> result;
    result.push_back(inp[0]);
    for (size_t i = 1; i < inp.size(); ++i) {
        if (inp[i].start <= result.back().end) {
            result.back().end = std::max(result.back().end, inp[i].end);
        } else {
            result.push_back(inp[i]);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Merge a remote findTimePeriod JSON response into the local result (in-place).
// Consecutive periods sharing the same mediaServerId are coalesced; overlapping
// periods from different servers are resolved so the later-starting server wins.
// ---------------------------------------------------------------------------
static void mergeTimePeriodResult(Value &dst, const Value &src, int period_type, int detail) {
    if (src.isNull() || !src.isObject()) return;

    // ── helper lambdas ──────────────────────────────────────────────────────

    // Extract flat {startTime, duration, mediaServerId} periods into a vector.
    auto extractPeriods = [](const Value &arr, std::vector<PeriodEntry> &out) {
        if (!arr.isArray()) return;
        for (const auto &p : arr) {
            PeriodEntry e;
            e.start    = p["startTime"].asUInt64();
            e.end      = e.start + p["duration"].asUInt64();
            e.serverId = p["mediaServerId"].asString();
            out.push_back(e);
        }
    };

    // Rebuild a Json array from a resolved PeriodEntry vector (duration variant).
    auto buildPeriodArray = [](const std::vector<PeriodEntry> &v) {
        Value arr = Json::arrayValue;
        for (const auto &e : v) {
            Value p;
            p["startTime"]     = (Json::UInt64)e.start;
            p["duration"]      = (Json::UInt64)(e.end - e.start);
            p["mediaServerId"] = e.serverId;
            arr.append(p);
        }
        return arr;
    };

    // ── period_type == 0 ────────────────────────────────────────────────────
    if (period_type == 0) {
        std::vector<PeriodEntry> periods;
        if (dst.isMember("periods") && dst["periods"].isArray()) {
            for (const auto &p : dst["periods"]) {
                PeriodEntry e;
                e.start    = p["startTime"].asUInt64();
                e.end      = e.start + p["timeLen"].asUInt64();
                e.serverId = p["mediaServerId"].asString();
                e.cameraId = p["cameraId"].asString();
                e.streamId = p["streamId"].asString();
                periods.push_back(e);
            }
        }
        if (src.isMember("periods") && src["periods"].isArray()) {
            for (const auto &p : src["periods"]) {
                PeriodEntry e;
                e.start    = p["startTime"].asUInt64();
                e.end      = e.start + p["timeLen"].asUInt64();
                e.serverId = p["mediaServerId"].asString();
                e.cameraId = p["cameraId"].asString();
                e.streamId = p["streamId"].asString();
                periods.push_back(e);
            }
        }
        auto resolved = resolvePeriods(periods);
        dst["periods"] = Json::arrayValue;
        for (const auto &e : resolved) {
            Value p;
            p["cameraId"]      = e.cameraId;
            p["streamId"]      = e.streamId;
            p["startTime"]     = (Json::UInt64)e.start;
            p["timeLen"]       = (Json::UInt64)(e.end - e.start);
            p["mediaServerId"] = e.serverId;
            dst["periods"].append(p);
        }

    // ── period_type == 1 ────────────────────────────────────────────────────
    } else if (period_type == 1) {
        if (detail == 0) {
            std::vector<PeriodEntry> periods;
            extractPeriods(dst["periods"], periods);
            if (src.isMember("periods")) extractPeriods(src["periods"], periods);
            dst["periods"] = buildPeriodArray(resolvePeriods(periods));
        } else {
            if (src.isMember("streams") && src["streams"].isArray()) {
                for (const auto &src_stream : src["streams"]) {
                    string sid = src_stream["streamId"].asString();
                    Value *dst_stream_ptr = nullptr;
                    for (auto &s : dst["streams"]) {
                        if (s["streamId"].asString() == sid) {
                            dst_stream_ptr = &s;
                            break;
                        }
                    }
                    if (!dst_stream_ptr) {
                        dst["streams"].append(src_stream);
                    } else {
                        std::vector<PeriodEntry> periods;
                        extractPeriods((*dst_stream_ptr)["periods"], periods);
                        if (src_stream.isMember("periods"))
                            extractPeriods(src_stream["periods"], periods);
                        (*dst_stream_ptr)["periods"] = buildPeriodArray(resolvePeriods(periods));
                    }
                }
            }
        }

    // ── period_type == 2 ────────────────────────────────────────────────────
    } else if (period_type == 2) {
        if (detail == 0) {
            // Hour-bitmap representation – OR the bitmaps; no per-period overlap logic.
            if (src.isMember("periods") && src["periods"].isObject()) {
                for (const auto &date_key : src["periods"].getMemberNames()) {
                    const auto &src_hours = src["periods"][date_key];
                    if (!dst["periods"].isMember(date_key)) {
                        dst["periods"][date_key] = src_hours;
                    } else {
                        auto &dst_hours = dst["periods"][date_key];
                        for (int h = 0; h < 24 && h < (int)src_hours.size(); ++h) {
                            if (src_hours[h].asInt() > 0) dst_hours[h] = 1;
                        }
                    }
                }
            }
        } else {
            if (src.isMember("streams") && src["streams"].isArray()) {
                for (const auto &src_stream : src["streams"]) {
                    string sid = src_stream["streamId"].asString();
                    Value *dst_stream_ptr = nullptr;
                    for (auto &s : dst["streams"]) {
                        if (s["streamId"].asString() == sid) {
                            dst_stream_ptr = &s;
                            break;
                        }
                    }
                    if (!dst_stream_ptr) {
                        dst["streams"].append(src_stream);
                    } else if (src_stream.isMember("dates") && src_stream["dates"].isObject()) {
                        for (const auto &date_key : src_stream["dates"].getMemberNames()) {
                            const auto &src_hours = src_stream["dates"][date_key];
                            if (!(*dst_stream_ptr)["dates"].isMember(date_key)) {
                                (*dst_stream_ptr)["dates"][date_key] = src_hours;
                            } else {
                                auto &dst_hours = (*dst_stream_ptr)["dates"][date_key];
                                for (int h = 0; h < 24 && h < (int)src_hours.size(); ++h) {
                                    if (!src_hours[h].isArray()) continue;
                                    std::vector<PeriodEntry> periods;
                                    if (dst_hours[h].isArray())
                                        extractPeriods(dst_hours[h], periods);
                                    extractPeriods(src_hours[h], periods);
                                    dst_hours[h] = buildPeriodArray(resolvePeriods(periods));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── motionPeriods ───────────────────────────────────────────────────────
    if (src.isMember("motionPeriods")) {
        const auto &src_motion = src["motionPeriods"];
        if (src_motion.isArray()) {
            std::vector<MotionPeriodEntry> motions;
            auto extractMotions = [&](const Value &arr) {
                if (!arr.isArray()) return;
                for (const auto &p : arr) {
                    MotionPeriodEntry e;
                    e.start = p["startTime"].asUInt64();
                    e.end   = e.start + p["duration"].asUInt64();
                    motions.push_back(e);
                }
            };
            if (dst.isMember("motionPeriods")) extractMotions(dst["motionPeriods"]);
            extractMotions(src_motion);
            auto resolved = resolveMotionPeriods(motions);
            dst["motionPeriods"] = Json::arrayValue;
            for (const auto &e : resolved) {
                Value p;
                p["startTime"] = (Json::UInt64)e.start;
                p["duration"]  = (Json::UInt64)(e.end - e.start);
                dst["motionPeriods"].append(p);
            }
        } else if (src_motion.isObject()) {
            // Hour-bitmap representation – OR the bitmaps.
            for (const auto &date_key : src_motion.getMemberNames()) {
                const auto &src_hours = src_motion[date_key];
                if (!dst["motionPeriods"].isMember(date_key)) {
                    dst["motionPeriods"][date_key] = src_hours;
                } else {
                    auto &dst_hours = dst["motionPeriods"][date_key];
                    for (int h = 0; h < 24 && h < (int)src_hours.size(); ++h) {
                        if (src_hours[h].asInt() > 0) dst_hours[h] = 1;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Local-only query (all period_types / detail modes).  Returns result via cb.
// ---------------------------------------------------------------------------
static void findTimePeriodLocal(
    const MediaTuple &tuple,
    uint64_t start_time, uint64_t end_time,
    int period_type, int detail, bool include_motion,
    const function<void(const SockException &, const Value &)> &cb)
{
    DebugL << "Find time period for device: " << tuple.shortUrl() << ". Time range: [" << getTimeStr("%Y-%m-%d %H:%M:%S", start_time) 
            << " - " << getTimeStr("%Y-%m-%d %H:%M:%S", end_time) << "]. Period type: " << period_type 
            << ". Detail: " << detail << ". Include motion: " << include_motion;
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
#if defined(ENABLE_MOTION)
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
#else
                WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
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
#if defined(ENABLE_MOTION)
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
#else
                WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
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
#if defined(ENABLE_MOTION)
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
#else
                WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
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
#if defined(ENABLE_MOTION)
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
#else
                WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
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
#if defined(ENABLE_MOTION)
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
#else
            WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
#endif // ENABLE_MOTION
        }
    }

    return cb(SockException(Err_success), result);
}

// ---------------------------------------------------------------------------
// Public API: VmsResourceAssignment-driven sub-range dispatch.
//   1. Query VmsResourceAssignment to find per-peer ownership windows.
//   2. If query start falls before the oldest assignment, query local timefiles
//      for that pre-assignment gap.
//   3. For each assignment window: local timefile read (sync) or remote API
//      call (async) depending on owner_peer_id.
//   4. Merge all results and invoke cb.
// ---------------------------------------------------------------------------
void SearchEngine::findTimePeriod(
    const MediaTuple &tuple,
    uint64_t start_time, uint64_t end_time,
    int period_type, int detail, bool include_motion,
    const string &jwt_token,
    bool edge,
    const function<void(const SockException &, const Value &)> &cb)
{
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);

    // 1. Query VmsResourceAssignment for all assignments overlapping [start_time, end_time].
    VmsResourceAssignmentImp assign_imp;
    auto assignments = assign_imp.findAssignmentsOverlappingRange(tuple.app, (int64_t)start_time, (int64_t)end_time);

    // Sort by assigned_at ascending to detect the pre-assignment gap.
    sort(assignments.begin(), assignments.end(),
        [](const VmsResourceAssignment &a, const VmsResourceAssignment &b) {
            return a.assigned_at < b.assigned_at;
        });

    // 2. Build sub-range lists.
    //    local_ranges : time windows to query via findTimePeriodLocal.
    //    remote_ranges: peer_id → time windows to fetch via remote API.
    vector<pair<uint64_t, uint64_t>> local_ranges;
    map<string, vector<pair<uint64_t, uint64_t>>> remote_ranges;

    if (assignments.empty()) {
        // No assignment records at all – query local for the full range.
        local_ranges.push_back({start_time, end_time});
    } else {
        // Pre-assignment gap: [start_time, oldest_assigned_at)
        uint64_t oldest = (uint64_t)assignments.front().assigned_at;
        if (start_time < oldest) {
            local_ranges.push_back({start_time, oldest - 1});
        }

        for (const auto &a : assignments) {
            uint64_t from = MAX(start_time, (uint64_t)a.assigned_at);
            uint64_t to   = (a.released_at == 0) ? end_time : MIN(end_time, (uint64_t)a.released_at);
            if (from > to) continue;

            if (a.owner_peer_id == mediaServerId) {
                local_ranges.push_back({from, to});
            } else {
                string peer_url = ClusterManager::Instance().getPeerUrl(a.owner_peer_id);
                if (!peer_url.empty()) {
                    remote_ranges[a.owner_peer_id].push_back({from, to});
                } else {
                    WarnL << "Peer " << a.owner_peer_id << " has no registered URL, skipping sub-range [" << from << "," << to << "]";
                }
            }
        }
    }

    // 3. Run all local sub-range queries synchronously and merge.
    auto merged = make_shared<Value>(Json::objectValue);
    (*merged)["cameraId"] = tuple.app;

    for (const auto &lr : local_ranges) {
        findTimePeriodLocal(tuple, lr.first, lr.second, period_type, detail, include_motion,
            [&](const SockException &ex, const Value &data) {
                if (!ex) {
                    mergeTimePeriodResult(*merged, data, period_type, detail);
                } else {
                    WarnL << "findTimePeriodLocal failed for [" << lr.first << "," << lr.second << "]: " << ex.what();
                }
            });
    }

    // 4. No remote peers or edge mode – reply immediately.
    if (remote_ranges.empty() || edge) {
        return cb(SockException(Err_success), *merged);
    }

    // 5. Fan-out via kBroadcastSyncTimeline — one emit per peer per sub-range.
    //    The webhook listener (WebHook.cpp) resolves origin_url and calls the
    //    remote /media/esc/recordedTimePeriod endpoint, returning the data JSON.
    int total = 0;
    for (const auto &kv : remote_ranges) total += (int)kv.second.size();

    auto pending   = make_shared<atomic<int>>(total);
    auto merge_mtx = make_shared<mutex>();

    for (const auto &kv : remote_ranges) {
        const string &peer_id = kv.first;
        string base_url = ClusterManager::Instance().getPeerUrl(peer_id);

        for (const auto &sr : kv.second) {
            uint64_t sr_start = sr.first;
            uint64_t sr_end   = sr.second;

            Broadcast::OnResInvoker res_invoker =
                [peer_id, merged, merge_mtx, pending, period_type, detail, cb]
                (const string &err, const int &code, const Value &data) mutable {
                    if (err.empty() && code == 0 && !data.isNull()) {
                        lock_guard<mutex> lk(*merge_mtx);
                        mergeTimePeriodResult(*merged, data, period_type, detail);
                    } else if (!err.empty()) {
                        WarnL << "recordedTimePeriod from peer " << peer_id << " failed: " << err;
                    }

                    if (--(*pending) == 0) {
                        Value result_copy;
                        {
                            lock_guard<mutex> lk(*merge_mtx);
                            result_copy = *merged;
                        }
                        cb(SockException(Err_success), result_copy);
                    }
                };

            auto flag = NOTICE_EMIT(BroadcastSyncTimelineArgs, Broadcast::kBroadcastSyncTimeline,
                base_url, tuple.app, sr_start, sr_end, period_type, detail, include_motion, jwt_token, res_invoker);
            // If no webhook listener is installed yet, fire the invoker ourselves
            // so pending is decremented and cb is eventually called.
            if (!flag) {
                WarnL << "kBroadcastSyncTimeline has no listeners — skipping remote peer " << peer_id;
                res_invoker("no webhook listener", -1, Json::nullValue);
            }
        }
    }
}

void SearchEngine::findBookmarks(
    const string &camera_id,
    int64_t start_time, int64_t end_time,
    const string &search,
    const string &user_id,
    int page, int size, const string &sort,
    const string &jwt_token,
    bool edge,
    const function<void(const SockException &, const Value &)> &cb)
{
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);

    // ── 1. Query the cluster-wide index (ESC DB, available on every node) ──
    BookmarkIndexImp idx_imp;
    int total   = idx_imp.count(start_time, end_time, camera_id, user_id, search);
    int offset  = page * size;
    auto index_page = idx_imp.search(start_time, end_time, camera_id, user_id, search, page, size, sort);

    Value result;
    result["data"]        = Json::arrayValue;
    result["currentPage"] = page;
    result["totalItems"]  = total;
    result["totalPages"]  = (size > 0) ? static_cast<int>(std::ceil(static_cast<double>(total) / size)) : 0;
    result["partial"]     = false;

    if (index_page.empty()) {
        return cb(SockException(Err_success), result);
    }

    // ── 2. Partition index entries by owner node ──
    // local_guids  : bookmark GUIDs whose full detail lives on this node
    // remote_groups: peer_id → list of bookmark GUIDs on that peer
    vector<string>                   local_guids;
    map<string, vector<string>>      remote_groups;
    // Preserve original order from sorted index page
    vector<string>                   ordered_guids;

    for (const auto &idx : index_page) {
        ordered_guids.push_back(idx.bookmark_guid);
        if (idx.owner_peer_id == mediaServerId) {
            local_guids.push_back(idx.bookmark_guid);
        } else {
            remote_groups[idx.owner_peer_id].push_back(idx.bookmark_guid);
        }
    }

    // ── 3. Fetch local detail ──
    // detail_map: bookmark_guid → JSON object (assembled below then merged in order)
    auto detail_map  = make_shared<unordered_map<string, Value>>();
    auto partial_flag = make_shared<bool>(false);
    auto merge_mtx   = make_shared<mutex>();

    {
        BookmarkImp bm_imp;
        UserEntityImp user_imp;
        auto local_bms = bm_imp.findByGuids(local_guids);
        // Build a map for O(1) lookup
        std::unordered_map<std::string, Bookmark> bm_map;
        for (const auto &b : local_bms) bm_map[b.guid] = b;

        for (const auto &guid : local_guids) {
            auto it = bm_map.find(guid);
            if (it == bm_map.end()) continue;
            const Bookmark &b = it->second;

            Value bj;
            bj["id"]           = b.guid;
            bj["camera_id"]    = b.camera_guid;
            bj["start_time"]   = static_cast<Json::Int64>(b.start_time);
            bj["duration"]     = b.duration;
            bj["name"]         = b.name        ? b.name.value()        : "";
            bj["end_time"]     = b.end_time    ? static_cast<Json::Int64>(b.end_time.value()) : -1;
            bj["description"]  = b.description ? b.description.value() : "";
            bj["creator_guid"] = b.creator_guid ? b.creator_guid.value() : "";
            bj["created"]      = b.created ? static_cast<Json::Int64>(b.created.value()) : -1;
            bj["owner_peer_id"] = mediaServerId;

            string username;
            if (b.creator_guid) {
                auto users = user_imp.findById(b.creator_guid.value());
                if (!users.empty())
                    username = users[0].userName ? users[0].userName.value() : "";
            }
            bj["creator"] = username;
            bj["tags"]    = bm_imp.findTagsByBookmark(b.guid);

            (*detail_map)[b.guid] = std::move(bj);
        }
    }

    // ── 4. No remote peers or edge mode – assemble and reply ──
    auto assemble_and_reply = [=]() mutable {
        Value data = Json::arrayValue;
        for (const auto &guid : ordered_guids) {
            auto it = detail_map->find(guid);
            if (it == detail_map->end()) continue;
            data.append(it->second);
        }
        Value res = result;
        res["data"]    = data;
        res["partial"] = *partial_flag;
        cb(SockException(Err_success), res);
    };

    if (remote_groups.empty() || edge) {
        return assemble_and_reply();
    }

    // ── 5. Fan-out to remote peers via broadcast (one emit per distinct owner node) ──
    int total_remote = (int)remote_groups.size();
    auto pending     = make_shared<atomic<int>>(total_remote);

    for (const auto &kv : remote_groups) {
        const string &peer_id    = kv.first;
        const auto   &peer_guids = kv.second;
        string base_url = ClusterManager::Instance().getPeerUrl(peer_id);

        if (base_url.empty()) {
            WarnL << "findBookmarks: no URL for peer " << peer_id << ", skipping " << peer_guids.size() << " bookmarks";
            {
                lock_guard<mutex> lk(*merge_mtx);
                *partial_flag = true;
            }
            if (--(*pending) == 0) assemble_and_reply();
            continue;
        }

        // Copy guid list into a vector for the broadcast args
        vector<string> guid_vec(peer_guids.begin(), peer_guids.end());

        Broadcast::OnResInvoker on_res = [peer_id, detail_map, merge_mtx, partial_flag, pending, assemble_and_reply]
            (const string &err, const int &idx, const Json::Value &data) mutable {
                if (err.empty() && data.isMember("data") && data["data"].isArray()) {
                    lock_guard<mutex> lk(*merge_mtx);
                    for (const auto &bj : data["data"])
                        (*detail_map)[bj["id"].asString()] = bj;
                } else {
                    WarnL << "findBookmarks: peer " << peer_id
                          << (err.empty() ? " returned no data field" : (" error: " + err));
                    lock_guard<mutex> lk(*merge_mtx);
                    *partial_flag = true;
                }
                if (--(*pending) == 0) assemble_and_reply();
            };

        auto flag = NOTICE_EMIT(BroadcastSyncBookmarkIndexArgs, Broadcast::kBroadcastSyncBookmarkIndex, base_url, guid_vec, jwt_token, on_res);
        if (!flag) {
            WarnL << "findBookmarks: no listener for kBroadcastSyncBookmarkIndex, peer=" << peer_id;
            {
                lock_guard<mutex> lk(*merge_mtx);
                *partial_flag = true;
            }
            if (--(*pending) == 0) assemble_and_reply();
        }
    }
}

void SearchEngine::findRecentBookmarks(
    const string &camera_id,
    const string &user_id,
    int size,
    const string &sort,
    const string &jwt_token,
    bool edge,
    const function<void(const SockException &, const Value &)> &cb)
{
    GET_CONFIG(string, mediaServerId, General::kMediaServerId);

    // ── 1. Query the cluster-wide index (ESC DB, available on every node) ──
    BookmarkIndexImp idx_imp;
    auto index_page = idx_imp.findRecentByCameraGuid(camera_id, user_id, size, sort);

    Value result;
    result["data"]        = Json::arrayValue;
    result["partial"]     = false;

    if (index_page.empty()) {
        return cb(SockException(Err_success), result);
    }

    // ── 2. Partition index entries by owner node ──
    // local_guids  : bookmark GUIDs whose full detail lives on this node
    // remote_groups: peer_id → list of bookmark GUIDs on that peer
    vector<string>                   local_guids;
    map<string, vector<string>>      remote_groups;
    // Preserve original order from sorted index page
    vector<string>                   ordered_guids;

    for (const auto &idx : index_page) {
        ordered_guids.push_back(idx.bookmark_guid);
        if (idx.owner_peer_id == mediaServerId) {
            local_guids.push_back(idx.bookmark_guid);
        } else {
            remote_groups[idx.owner_peer_id].push_back(idx.bookmark_guid);
        }
    }

    // ── 3. Fetch local detail ──
    // detail_map: bookmark_guid → JSON object (assembled below then merged in order)
    auto detail_map  = make_shared<unordered_map<string, Value>>();
    auto partial_flag = make_shared<bool>(false);
    auto merge_mtx   = make_shared<mutex>();

    {
        BookmarkImp bm_imp;
        UserEntityImp user_imp;
        auto local_bms = bm_imp.findByGuids(local_guids);
        // Build a map for O(1) lookup
        std::unordered_map<std::string, Bookmark> bm_map;
        for (const auto &b : local_bms) bm_map[b.guid] = b;

        for (const auto &guid : local_guids) {
            auto it = bm_map.find(guid);
            if (it == bm_map.end()) continue;
            const Bookmark &b = it->second;

            Value bj;
            bj["id"]           = b.guid;
            bj["camera_id"]    = b.camera_guid;
            bj["start_time"]   = static_cast<Json::Int64>(b.start_time);
            bj["duration"]     = b.duration;
            bj["name"]         = b.name        ? b.name.value()        : "";
            bj["end_time"]     = b.end_time    ? static_cast<Json::Int64>(b.end_time.value()) : -1;
            bj["description"]  = b.description ? b.description.value() : "";
            bj["creator_guid"] = b.creator_guid ? b.creator_guid.value() : "";
            bj["created"]      = b.created ? static_cast<Json::Int64>(b.created.value()) : -1;
            bj["owner_peer_id"] = mediaServerId;

            string username;
            if (b.creator_guid) {
                auto users = user_imp.findById(b.creator_guid.value());
                if (!users.empty())
                    username = users[0].userName ? users[0].userName.value() : "";
            }
            bj["creator"] = username;
            bj["tags"]    = bm_imp.findTagsByBookmark(b.guid);

            (*detail_map)[b.guid] = std::move(bj);
        }
    }

    // ── 4. No remote peers or edge mode – assemble and reply ──
    auto assemble_and_reply = [=]() mutable {
        Value data = Json::arrayValue;
        for (const auto &guid : ordered_guids) {
            auto it = detail_map->find(guid);
            if (it == detail_map->end()) continue;
            data.append(it->second);
        }
        Value res = result;
        res["data"]    = data;
        res["partial"] = *partial_flag;
        cb(SockException(Err_success), res);
    };

    if (remote_groups.empty() || edge) {
        return assemble_and_reply();
    }

    // ── 5. Fan-out to remote peers via broadcast (one emit per distinct owner node) ──
    int total_remote = (int)remote_groups.size();
    auto pending     = make_shared<atomic<int>>(total_remote);

    for (const auto &kv : remote_groups) {
        const string &peer_id    = kv.first;
        const auto   &peer_guids = kv.second;
        string base_url = ClusterManager::Instance().getPeerUrl(peer_id);

        if (base_url.empty()) {
            WarnL << "findBookmarks: no URL for peer " << peer_id << ", skipping " << peer_guids.size() << " bookmarks";
            {
                lock_guard<mutex> lk(*merge_mtx);
                *partial_flag = true;
            }
            if (--(*pending) == 0) assemble_and_reply();
            continue;
        }

        // Copy guid list into a vector for the broadcast args
        vector<string> guid_vec(peer_guids.begin(), peer_guids.end());

        Broadcast::OnResInvoker on_res = [peer_id, detail_map, merge_mtx, partial_flag, pending, assemble_and_reply]
            (const string &err, const int &idx, const Json::Value &data) mutable {
                if (err.empty() && data.isMember("data") && data["data"].isArray()) {
                    lock_guard<mutex> lk(*merge_mtx);
                    for (const auto &bj : data["data"])
                        (*detail_map)[bj["id"].asString()] = bj;
                } else {
                    WarnL << "findBookmarks: peer " << peer_id
                          << (err.empty() ? " returned no data field" : (" error: " + err));
                    lock_guard<mutex> lk(*merge_mtx);
                    *partial_flag = true;
                }
                if (--(*pending) == 0) assemble_and_reply();
            };

        auto flag = NOTICE_EMIT(BroadcastSyncBookmarkIndexArgs, Broadcast::kBroadcastSyncBookmarkIndex, base_url, guid_vec, jwt_token, on_res);
        if (!flag) {
            WarnL << "findBookmarks: no listener for kBroadcastSyncBookmarkIndex, peer=" << peer_id;
            {
                lock_guard<mutex> lk(*merge_mtx);
                *partial_flag = true;
            }
            if (--(*pending) == 0) assemble_and_reply();
        }
    }
}

void SearchEngine::findMotionPeriodByRoi(
    const MediaTuple &tuple,
    uint64_t start_time, uint64_t end_time,
    const string &roi_mask,
    bool edge,
    const function<void(const SockException &, const Value &)> &cb)
{
    Value result;
#if defined(ENABLE_MOTION)
    result["cameraId"]     = tuple.app;
    result["motionPeriods"] = arrayValue;
    GET_CONFIG(string, mediaServerId, General::kMediaServerId)
    try {
        MotionSearch ms(tuple);
        ms.getMotionTimePeriodByRoi(start_time, end_time, roi_mask,
            [&](vector<MotionTimeRange> &ret) {
                for (auto const &p : ret) {
                    Value period;
                    period["startTime"] = (Json::UInt64)p.startTime;
                    period["duration"]  = p.duration;
                    period["mediaServerId"] = mediaServerId;
                    result["motionPeriods"].append(period);
                }
            });
    } catch (const std::exception &ex) {
        WarnL << "findMotionPeriodByRoi failed: " << ex.what();
    }
    return cb(SockException(Err_success), result);
#else
    WarnL << "Motion is not enabled. Rebuild with ENABLE_MOTION to use this feature.";
    return cb(SockException(Err_other, "Motion feature is not enabled", CODE_FEATURE_NOT_SUPPORTED), result);
#endif // ENABLE_MOTION
}

// ---------------------------------------------------------------------------
// getBookmarkDetail — fetch full bookmark records for a list of local GUIDs.
// ---------------------------------------------------------------------------
void SearchEngine::getBookmarkDetail(
    const std::vector<std::string> &guids,
    bool edge,
    const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb)
{
    BookmarkImp   bm_imp;
    UserEntityImp user_imp;

    // Single batch query instead of N individual findById calls
    auto bms = bm_imp.findByGuids(guids);

    // Index by guid for ordered output
    std::unordered_map<std::string, Bookmark> bm_map;
    for (const auto &b : bms) bm_map[b.guid] = b;

    Value result;
    result["data"] = Json::arrayValue;

    for (const auto &guid : guids) {
        auto it = bm_map.find(guid);
        if (it == bm_map.end()) continue;
        const Bookmark &b = it->second;

        Value bj;
        bj["id"]           = b.guid;
        bj["camera_id"]    = b.camera_guid;
        bj["start_time"]   = static_cast<Json::Int64>(b.start_time);
        bj["duration"]     = b.duration;
        bj["name"]         = b.name        ? b.name.value()        : "";
        bj["end_time"]     = b.end_time    ? static_cast<Json::Int64>(b.end_time.value()) : -1;
        bj["description"]  = b.description ? b.description.value() : "";
        bj["creator_guid"] = b.creator_guid ? b.creator_guid.value() : "";
        bj["created"]      = b.created     ? static_cast<Json::Int64>(b.created.value()) : -1;

        string username;
        if (b.creator_guid) {
            auto users = user_imp.findById(b.creator_guid.value());
            if (!users.empty())
                username = users[0].userName ? users[0].userName.value() : "";
        }
        bj["creator"] = username;
        bj["tags"]    = bm_imp.findTagsByBookmark(b.guid);

        result["data"].append(std::move(bj));
    }

    cb(SockException(Err_success), result);
}

} // namespace managerkit
