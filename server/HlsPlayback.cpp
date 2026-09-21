#include "HlsPlayback.h"
#include <sstream>
#include <vector>
#include <memory>
#include <sys/stat.h>
#include <ctime>
#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Manager.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Common/strCoding.h"
#include "Http/HttpSession.h"
#include "Network/Socket.h"
#include "Util/util.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Util/base64.h"
#include "ApiWorkDispatcher.h"
#include "Record/MPEG.h"
#include "Record/MP4Demuxer.h"
#include "Local/StatisticRecorder.h"
#include "Local/TimeQuery.h"
#include "ext-codec/H264.h"
#include "ext-codec/H265.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

#if defined(ENABLE_MP4) && defined(ENABLE_HLS)

class FrameWrap : public Frame {
public:
    using Ptr = std::shared_ptr<FrameWrap>;

    FrameWrap(const Frame::Ptr &frame, uint64_t relative_dts, uint64_t relative_pts)
        : _frame(frame), _dts(relative_dts), _pts(relative_pts) {
        setIndex(frame->getIndex());
    }

    ~FrameWrap() override = default;

    // Buffer methods:
    char *data() const override { return _frame->data(); }
    size_t size() const override { return _frame->size(); }
    std::string toString() const override { return _frame->toString(); }
    size_t getCapacity() const override { return _frame->getCapacity(); }

    // CodecInfo methods:
    CodecId getCodecId() const override { return _frame->getCodecId(); }

    // Frame methods:
    uint64_t dts() const override { return _dts; }
    uint64_t pts() const override { return _pts; }
    size_t prefixSize() const override { return _frame->prefixSize(); }
    bool keyFrame() const override { return _frame->keyFrame(); }
    bool configFrame() const override { return _frame->configFrame(); }
    bool cacheAble() const override { return _frame->cacheAble(); }
    bool dropAble() const override { return _frame->dropAble(); }
    bool decodeAble() const override { return _frame->decodeAble(); }

private:
    Frame::Ptr _frame;
    uint64_t _dts;
    uint64_t _pts;
};

class TSMuxer : public MpegMuxer {
public:
    TSMuxer() : MpegMuxer(false) {}
    ~TSMuxer() override { MpegMuxer::flush(); }

    const string& getTsData() const { return _ts_data; }

protected:
    void onWrite(std::shared_ptr<toolkit::Buffer> buffer, uint64_t timestamp, bool key_pos) override {
        if (buffer) {
            _ts_data.append(buffer->data(), buffer->size());
        }
    }

private:
    string _ts_data;
};

struct SegmentItem {
    string file_path;
    uint64_t offset_ms;
    uint64_t duration_ms;
    double duration_sec_double;
    bool is_hi;
    uint64_t absolute_start_ms;
};

static SegmentItem getKeyframeAlignedSegmentSingle(const string &file_path, uint64_t start_offset_ms, uint64_t max_end_offset_ms, uint64_t block_start_time, bool is_hi) {
    // TODO(merge-repo): khi port record encryption, đổi lại thành
    // TierEncryptionManager::Instance().materializeReadablePathForFile(...)
    string abs_file_path = decodeBase64(file_path);
    struct stat st;
    if (stat(abs_file_path.c_str(), &st) != 0) {
        SegmentItem item;
        item.file_path = file_path;
        item.offset_ms = start_offset_ms;
        uint64_t dur = max_end_offset_ms - start_offset_ms;
        item.duration_ms = dur;
        item.duration_sec_double = (double)dur / 1000.0;
        item.is_hi = is_hi;
        item.absolute_start_ms = block_start_time * 1000 + start_offset_ms;
        return item;
    }

    try {
        MP4Demuxer demuxer;
        demuxer.openMP4(abs_file_path);

        int64_t actual_start = demuxer.seekTo(start_offset_ms);
        if (actual_start < 0) {
            actual_start = start_offset_ms;
        }

        uint64_t target_next = actual_start + 10000;
        int64_t actual_next = -1;
        if (target_next < max_end_offset_ms) {
            actual_next = demuxer.seekTo(target_next);
        }

        if (actual_next <= actual_start) {
            uint64_t step = 10000;
            while (true) {
                target_next += step;
                if (target_next >= max_end_offset_ms) {
                    actual_next = max_end_offset_ms;
                    break;
                }
                actual_next = demuxer.seekTo(target_next);
                if (actual_next > actual_start) {
                    break;
                }
            }
        }

        if (actual_next > (int64_t)max_end_offset_ms) {
            actual_next = max_end_offset_ms;
        }

        uint64_t duration = actual_next - actual_start;
        demuxer.closeMP4();

        SegmentItem item;
        item.file_path = file_path;
        item.offset_ms = actual_start;
        item.duration_ms = duration;
        item.duration_sec_double = (double)duration / 1000.0;
        item.is_hi = is_hi;
        item.absolute_start_ms = block_start_time * 1000 + actual_start;
        return item;
    } catch (...) {
        SegmentItem item;
        item.file_path = file_path;
        item.offset_ms = start_offset_ms;
        uint64_t dur = max_end_offset_ms - start_offset_ms;
        item.duration_ms = dur;
        item.duration_sec_double = (double)dur / 1000.0;
        item.is_hi = is_hi;
        item.absolute_start_ms = block_start_time * 1000 + start_offset_ms;
        return item;
    }
}

struct HlsSessionInfo {
    string token_or_secret;
    uint64_t expire_time;
};

class HlsSessionTokenManager {
public:
    static HlsSessionTokenManager &Instance() {
        static HlsSessionTokenManager instance;
        return instance;
    }

    string createSession(const string &token_or_secret, uint64_t duration_sec = 86400) {
        lock_guard<mutex> lock(_mutex);
        cleanExpiredSessions();
        string session_token = "sess_" + toolkit::makeRandStr(32);
        uint64_t expire_time = time(nullptr) + duration_sec;
        _sessions[session_token] = {token_or_secret, expire_time};
        return session_token;
    }

    bool verifySession(const string &session_token, string &out_token_or_secret) {
        lock_guard<mutex> lock(_mutex);
        auto it = _sessions.find(session_token);
        if (it == _sessions.end()) {
            return false;
        }
        if (time(nullptr) > it->second.expire_time) {
            _sessions.erase(it);
            return false;
        }
        out_token_or_secret = it->second.token_or_secret;
        return true;
    }

private:
    HlsSessionTokenManager() = default;
    ~HlsSessionTokenManager() = default;

    void cleanExpiredSessions() {
        uint64_t now = time(nullptr);
        for (auto it = _sessions.begin(); it != _sessions.end();) {
            if (now > it->second.expire_time) {
                it = _sessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    mutex _mutex;
    unordered_map<string, HlsSessionInfo> _sessions;
};

#endif // defined(ENABLE_MP4) && defined(ENABLE_HLS)

void installHlsPlaybackApi() {
    // Register API to check HLS Playback support status
    // GET /media/mserver/support
    api_regist("/media/mserver/support", [](API_ARGS_MAP_ASYNC) {
        GET_CONFIG(string, api_secret, API::kSecret);

        // Clean up parameter values polluted by ?token= or ?secret= due to client URL formatting typos
        string query_token;
        string query_secret;
        for (auto &pr : allArgs.args) {
            size_t pos = pr.second.find("?token=");
            if (pos != string::npos) {
                query_token = pr.second.substr(pos + 7);
                pr.second = pr.second.substr(0, pos);
            }
            pos = pr.second.find("?secret=");
            if (pos != string::npos) {
                query_secret = pr.second.substr(pos + 8);
                pr.second = pr.second.substr(0, pos);
            }
        }
        if (!query_token.empty()) {
            allArgs.args["token"] = query_token;
        }
        if (!query_secret.empty()) {
            allArgs.args["secret"] = query_secret;
        }

        // Extract secret if it was appended using a typo ?secret=
        if (allArgs["secret"].empty()) {
            for (const auto &pr : allArgs.getParser().getUrlArgs()) {
                size_t pos = pr.second.find("?secret=");
                if (pos != string::npos) {
                    allArgs.args["secret"] = pr.second.substr(pos + 8);
                    break;
                }
            }
        }

        bool is_secret_valid = false;
        try {
            CHECK_SECRET();
            is_secret_valid = true;
        } catch (...) {}

        if (!is_secret_valid) {
            string token;
            if (!allArgs["token"].empty()) {
                token = (string)allArgs["token"];
            } else {
                for (const auto &pr : allArgs.getParser().getUrlArgs()) {
                    size_t pos = pr.second.find("?token=");
                    if (pos != string::npos) {
                        token = pr.second.substr(pos + 7);
                        break;
                    }
                }
            }

            if (!token.empty() && allArgs["Authorization"].empty()) {
                allArgs.args["Authorization"] = "Bearer " + token;
            }
        }

        auto on_authorized = [invoker, headerOut]() mutable {
            Json::Value res;
            res["code"] = 0;
            res["msg"] = "success";
#if defined(ENABLE_MP4) && defined(ENABLE_HLS)
            res["supported"] = true;
            res["hls"] = true;
#else
            res["supported"] = false;
            res["hls"] = false;
#endif

#if defined(ENABLE_MOTION)
            res["motion"] = true;
#else
            res["motion"] = false;
#endif
            headerOut["Content-Type"] = "application/json";
            invoker(200, headerOut, res.toStyledString());
        };

        if (is_secret_valid) {
            on_authorized();
        } else {
            try {
                CHECK_AUTH_TOKEN();
                CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
                
                // If cameraId is provided, we can optionally check device access
                if (!allArgs["cameraId"].empty()) {
                    string cameraId = allArgs["cameraId"];
                    CHECK_USER_DEVICE_AUTHOR_ASYNC(cameraId, on_authorized);
                } else {
                    on_authorized();
                }
            } catch (const std::exception &ex) {
                Json::Value val;
                val["code"] = API::AuthFailed;
                val["msg"] = ex.what();
                invoker(401, headerOut, val.toStyledString());
            }
        }
    });

#if defined(ENABLE_MP4) && defined(ENABLE_HLS)
    // Register Dynamic HLS Playlist generator
    // GET /media/mserver/playback/hls/playlist?cameraId=xxx&startTime=xxx&endTime=xxx&secret=xxx
    api_regist("/media/mserver/playback/hls/playlist", [](API_ARGS_MAP_ASYNC) {
        GET_CONFIG(string, api_secret, API::kSecret);

        // Clean up parameter values polluted by ?token= or ?secret= due to client URL formatting typos
        string query_token;
        string query_secret;
        for (auto &pr : allArgs.args) {
            size_t pos = pr.second.find("?token=");
            if (pos != string::npos) {
                query_token = pr.second.substr(pos + 7);
                pr.second = pr.second.substr(0, pos);
            }
            pos = pr.second.find("?secret=");
            if (pos != string::npos) {
                query_secret = pr.second.substr(pos + 8);
                pr.second = pr.second.substr(0, pos);
            }
        }
        if (!query_token.empty()) {
            allArgs.args["token"] = query_token;
        }
        if (!query_secret.empty()) {
            allArgs.args["secret"] = query_secret;
        }

        CHECK_ARGS("cameraId", "startTime");
        string cameraId = allArgs["cameraId"];
        uint64_t startTime = std::stoull(allArgs["startTime"]);
        uint64_t endTime = 0;
        uint64_t now = ::time(nullptr);
        if (!allArgs["endTime"].empty()) {
            endTime = std::stoull(allArgs["endTime"]);
        } else {
            // Limit to the end of the day of startTime (local time)
            time_t start_time_t = (time_t)startTime;
            struct tm tm_start;
            if (localtime_r(&start_time_t, &tm_start)) {
                tm_start.tm_hour = 23;
                tm_start.tm_min = 59;
                tm_start.tm_sec = 59;
                time_t end_of_day = mktime(&tm_start);
                endTime = std::min((uint64_t)end_of_day, now);
            } else {
                endTime = now;
            }
        }

        // Limit maximum duration to 2 hours (7200 seconds)
        if (endTime > startTime + 7200) {
            endTime = startTime + 7200;
        }

        // Avoid invalid range
        if (endTime < startTime) {
            endTime = startTime;
        }
        // Extract secret if it was appended using a typo ?secret=
        if (allArgs["secret"].empty()) {
            for (const auto &pr : allArgs.getParser().getUrlArgs()) {
                size_t pos = pr.second.find("?secret=");
                if (pos != string::npos) {
                    allArgs.args["secret"] = pr.second.substr(pos + 8);
                    break;
                }
            }
        }

        bool is_secret_valid = false;
        try {
            CHECK_SECRET();
            is_secret_valid = true;
        } catch (...) {}

        if (!is_secret_valid) {
            string token;
            if (!allArgs["token"].empty()) {
                token = (string)allArgs["token"];
            } else {
                for (const auto &pr : allArgs.getParser().getUrlArgs()) {
                    size_t pos = pr.second.find("?token=");
                    if (pos != string::npos) {
                        token = pr.second.substr(pos + 7);
                        break;
                    }
                }
            }

            if (!token.empty() && allArgs["Authorization"].empty()) {
                allArgs.args["Authorization"] = "Bearer " + token;
            }
        }

        auto on_authorized = [allArgs, val, invoker, headerOut, is_secret_valid, api_secret, cameraId, startTime, endTime]() mutable {
            // Determine mode and quality
            enum PlaybackMode {
                Mode_Low = 0,
                Mode_High = 1,
                Mode_AutoHigh = 2, // equivalent to adaptive
                Mode_AutoLow = 3   // equivalent to auto
            };

            PlaybackMode play_mode = Mode_Low; // Default is low quality
            if (!allArgs["mode"].empty()) {
                string mode_str = allArgs["mode"];
                if (mode_str == "auto" || mode_str == "auto_low") {
                    play_mode = Mode_AutoLow;
                } else if (mode_str == "low") {
                    play_mode = Mode_Low;
                } else if (mode_str == "high") {
                    play_mode = Mode_High;
                } else if (mode_str == "adaptive" || mode_str == "auto_high") {
                    play_mode = Mode_AutoHigh;
                }
            } else if (!allArgs["adaptive"].empty()) {
                if (allArgs["adaptive"] == "1" || allArgs["adaptive"] == "true") {
                    play_mode = Mode_AutoHigh;
                }
            }

            // Query stream quality map
            string hi_stream_id;
            string lo_stream_id;
            auto recorder = StatisticRecorder::Instance().getRecorder(cameraId);
            if (recorder) {
                auto params = recorder->getParams();
                auto hi_it = params.stream_map.find(managerkit::PrimaryStream);
                if (hi_it != params.stream_map.end()) {
                    hi_stream_id = hi_it->second.stream_id;
                }
                auto lo_it = params.stream_map.find(managerkit::SecondaryStream);
                if (lo_it != params.stream_map.end()) {
                    lo_stream_id = lo_it->second.stream_id;
                }
            }
            if (hi_stream_id.empty()) {
                hi_stream_id = cameraId;
            }
            if (lo_stream_id.empty()) {
                lo_stream_id = hi_stream_id;
            }

            vector<SegmentItem> segments;

            if (play_mode == Mode_AutoHigh || play_mode == Mode_AutoLow) {
                // Multi-stream modes: Merging Hi and Lo streams
                MediaTuple hi_tuple = { DEFAULT_VHOST, cameraId, hi_stream_id, "" };
                MediaTuple lo_tuple = { DEFAULT_VHOST, cameraId, lo_stream_id, "" };

                vector<TimeBlock> hi_blocks;
                vector<TimeBlock> lo_blocks;

                try {
                    TimeQuery hi_query(hi_tuple);
                    hi_query.getRecordedTimePeriod(startTime, endTime, [&](vector<TimeBlock> &data) {
                        hi_blocks = data;
                    });
                } catch (...) {}

                try {
                    TimeQuery lo_query(lo_tuple);
                    lo_query.getRecordedTimePeriod(startTime, endTime, [&](vector<TimeBlock> &data) {
                        lo_blocks = data;
                    });
                } catch (...) {}

                // Adjust startTime if there is no playback data at the requested time
                // but there is data within the 10-second neighborhood.
                bool covered = false;
                for (const auto &block : hi_blocks) {
                    if (block.start_time() <= startTime && (block.start_time() + block.time_len()) > startTime) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    for (const auto &block : lo_blocks) {
                        if (block.start_time() <= startTime && (block.start_time() + block.time_len()) > startTime) {
                            covered = true;
                            break;
                        }
                    }
                }

                if (!covered) {
                    const TimeBlock* closest_block = nullptr;
                    uint64_t min_dist = 99999999;
                    bool is_left = false;

                    auto check_closest = [&](const vector<TimeBlock> &blocks_list) {
                        for (const auto &block : blocks_list) {
                            uint64_t block_start = block.start_time();
                            uint64_t block_end = block_start + block.time_len();
                            if (block_end <= startTime) {
                                uint64_t dist = startTime - block_end;
                                if (dist < min_dist) {
                                    min_dist = dist;
                                    closest_block = &block;
                                    is_left = true;
                                }
                            } else if (block_start >= startTime) {
                                uint64_t dist = block_start - startTime;
                                if (dist < min_dist) {
                                    min_dist = dist;
                                    closest_block = &block;
                                    is_left = false;
                                }
                            }
                        }
                    };

                    check_closest(hi_blocks);
                    check_closest(lo_blocks);

                    if (closest_block && min_dist <= 10) {
                        if (is_left) {
                            uint64_t block_start = closest_block->start_time();
                            uint64_t block_end = block_start + closest_block->time_len();
                            startTime = (block_end > 10) ? std::max(block_start, block_end - 10) : block_start;
                        } else {
                            startTime = closest_block->start_time();
                        }
                    } else {
                        startTime = endTime;
                    }
                }

                uint64_t t = startTime;
                while (t < endTime) {
                    bool is_hi = false;
                    const TimeBlock* selected_block = nullptr;

                    if (play_mode == Mode_AutoHigh) {
                        // 1. Search for a Hi block covering t
                        for (const auto &block : hi_blocks) {
                            if (block.start_time() <= t && (block.start_time() + block.time_len()) > t) {
                                selected_block = &block;
                                is_hi = true;
                                break;
                            }
                        }
                        // 2. If not found in Hi, search for a Lo block covering t
                        if (!selected_block) {
                            for (const auto &block : lo_blocks) {
                                if (block.start_time() <= t && (block.start_time() + block.time_len()) > t) {
                                    selected_block = &block;
                                    is_hi = false;
                                    break;
                                }
                            }
                        }
                    } else { // Mode_AutoLow (prefers low quality, falls back to high quality)
                        // 1. Search for a Lo block covering t
                        for (const auto &block : lo_blocks) {
                            if (block.start_time() <= t && (block.start_time() + block.time_len()) > t) {
                                selected_block = &block;
                                is_hi = false;
                                break;
                            }
                        }
                        // 2. If not found in Lo, search for a Hi block covering t
                        if (!selected_block) {
                            for (const auto &block : hi_blocks) {
                                if (block.start_time() <= t && (block.start_time() + block.time_len()) > t) {
                                    selected_block = &block;
                                    is_hi = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (selected_block) {
                        uint64_t block_start = selected_block->start_time();
                        uint64_t block_len = selected_block->time_len();
                        uint64_t block_end = block_start + block_len;

                        uint64_t offset_sec = t - block_start;
                        uint64_t max_dur_sec = std::min(block_end - t, endTime - t);
                        uint64_t max_dur_sec_limited = std::min(max_dur_sec, (uint64_t)10);

                        SegmentItem item = getKeyframeAlignedSegmentSingle(
                            selected_block->file_path(),
                            offset_sec * 1000,
                            (offset_sec + max_dur_sec_limited) * 1000,
                            block_start,
                            is_hi
                        );
                        segments.push_back(item);

                        uint64_t actual_dur_sec = item.duration_ms / 1000;
                        if (actual_dur_sec == 0) {
                            actual_dur_sec = 1;
                        }
                        t += actual_dur_sec;
                    } else {
                        // No block covers t. Let's find the nearest next block after t.
                        uint64_t next_t = 0;
                        for (const auto &block : hi_blocks) {
                            if (block.start_time() > t) {
                                if (next_t == 0 || block.start_time() < next_t) {
                                    next_t = block.start_time();
                                }
                            }
                        }
                        for (const auto &block : lo_blocks) {
                            if (block.start_time() > t) {
                                if (next_t == 0 || block.start_time() < next_t) {
                                    next_t = block.start_time();
                                }
                            }
                        }

                        if (next_t > 0 && next_t < endTime) {
                            t = next_t;
                        } else {
                            break;
                        }
                    }
                }
            } else {
                // Single-stream modes: play_mode == Mode_Low || play_mode == Mode_High
                string selected_stream_id = (play_mode == Mode_High) ? hi_stream_id : lo_stream_id;

                MediaTuple single_tuple = { DEFAULT_VHOST, cameraId, selected_stream_id, "" };
                vector<TimeBlock> blocks;
                try {
                    TimeQuery query(single_tuple);
                    query.getRecordedTimePeriod(startTime, endTime, [&](vector<TimeBlock> &data) {
                        blocks = data;
                    });
                } catch (...) {}

                // Adjust startTime if there is no playback data at the requested time
                // but there is data within the 10-second neighborhood.
                bool covered = false;
                for (const auto &block : blocks) {
                    if (block.start_time() <= startTime && (block.start_time() + block.time_len()) > startTime) {
                        covered = true;
                        break;
                    }
                }

                if (!covered) {
                    const TimeBlock* closest_block = nullptr;
                    uint64_t min_dist = 99999999;
                    bool is_left = false;

                    for (const auto &block : blocks) {
                        uint64_t block_start = block.start_time();
                        uint64_t block_end = block_start + block.time_len();
                        if (block_end <= startTime) {
                            uint64_t dist = startTime - block_end;
                            if (dist < min_dist) {
                                min_dist = dist;
                                closest_block = &block;
                                is_left = true;
                            }
                        } else if (block_start >= startTime) {
                            uint64_t dist = block_start - startTime;
                            if (dist < min_dist) {
                                min_dist = dist;
                                closest_block = &block;
                                is_left = false;
                            }
                        }
                    }

                    if (closest_block && min_dist <= 10) {
                        if (is_left) {
                            uint64_t block_start = closest_block->start_time();
                            uint64_t block_end = block_start + closest_block->time_len();
                            startTime = (block_end > 10) ? std::max(block_start, block_end - 10) : block_start;
                        } else {
                            startTime = closest_block->start_time();
                        }
                    } else {
                        startTime = endTime;
                    }
                }

                uint64_t t = startTime;
                while (t < endTime) {
                    const TimeBlock* selected_block = nullptr;
                    for (const auto &block : blocks) {
                        if (block.start_time() <= t && (block.start_time() + block.time_len()) > t) {
                            selected_block = &block;
                            break;
                        }
                    }

                    if (selected_block) {
                        uint64_t block_start = selected_block->start_time();
                        uint64_t block_len = selected_block->time_len();
                        uint64_t block_end = block_start + block_len;

                        uint64_t offset_sec = t - block_start;
                        uint64_t max_dur_sec = std::min(block_end - t, endTime - t);
                        uint64_t max_dur_sec_limited = std::min(max_dur_sec, (uint64_t)10);

                        SegmentItem item = getKeyframeAlignedSegmentSingle(
                            selected_block->file_path(),
                            offset_sec * 1000,
                            (offset_sec + max_dur_sec_limited) * 1000,
                            block_start,
                            (selected_stream_id == hi_stream_id)
                        );
                        segments.push_back(item);

                        uint64_t actual_dur_sec = item.duration_ms / 1000;
                        if (actual_dur_sec == 0) {
                            actual_dur_sec = 1;
                        }
                        t += actual_dur_sec;
                    } else {
                        // No block covers t. Find nearest next block.
                        uint64_t next_t = 0;
                        for (const auto &block : blocks) {
                            if (block.start_time() > t) {
                                if (next_t == 0 || block.start_time() < next_t) {
                                    next_t = block.start_time();
                                }
                            }
                        }

                        if (next_t > 0 && next_t < endTime) {
                            t = next_t;
                        } else {
                            break;
                        }
                    }
                }
            }

            // Build M3U8 string
            stringstream ss;
            ss << "#EXTM3U\n";
            ss << "#EXT-X-VERSION:3\n";
            ss << "#EXT-X-TARGETDURATION:10\n";
            ss << "#EXT-X-MEDIA-SEQUENCE:0\n";
            ss << "#EXT-X-PLAYLIST-TYPE:VOD\n";

            bool last_is_hi = false;
            string last_file_path;
            bool has_last = false;
            uint64_t last_absolute_end_ms = 0;
            uint64_t current_playlist_dts = 0;

            // Build Auth Parameter for Segment URLs using Session Token
            string credential;
            if (is_secret_valid) {
                credential = "secret=" + api_secret;
            } else {
                string bearer_token = allArgs["Authorization"];
                if (bearer_token.empty()) {
                    bearer_token = allArgs["token"];
                }
                if (!bearer_token.empty() && bearer_token.find("Bearer") == string::npos) {
                    bearer_token = "Bearer " + bearer_token;
                }
                credential = bearer_token;
            }

            string session_token = HlsSessionTokenManager::Instance().createSession(credential, 86400);
            string auth_param = "token=" + session_token;

            for (const auto &seg : segments) {
                // Discontinuity checks
                if (has_last) {
                    int64_t gap = (int64_t)seg.absolute_start_ms - (int64_t)last_absolute_end_ms;
                    if (seg.is_hi != last_is_hi || seg.file_path != last_file_path || std::abs(gap) > 1000) {
                        ss << "#EXT-X-DISCONTINUITY\n";
                    }
                }
                last_is_hi = seg.is_hi;
                last_file_path = seg.file_path;
                last_absolute_end_ms = seg.absolute_start_ms + seg.duration_ms;
                has_last = true;

                ss << "#EXTINF:" << seg.duration_sec_double << ",\n";
                ss << "/media/mserver/playback/hls/segment.ts?file=" << seg.file_path
                   << "&offset_ms=" << seg.offset_ms
                   << "&duration_ms=" << seg.duration_ms
                   << "&dts_offset_ms=" << current_playlist_dts
                   << "&" << auth_param << "\n";

                current_playlist_dts += seg.duration_ms;
            }
            ss << "#EXT-X-ENDLIST\n";

            headerOut["Content-Type"] = "application/vnd.apple.mpegurl";
            invoker(200, headerOut, ss.str());
        };

        if (is_secret_valid) {
            on_authorized();
        } else {
            try {
                CHECK_AUTH_TOKEN();
                CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
                CHECK_USER_DEVICE_AUTHOR_ASYNC(cameraId, on_authorized);
            } catch (const std::exception &ex) {
                val["code"] = API::AuthFailed;
                val["msg"] = ex.what();
                invoker(401, headerOut, val.toStyledString());
            }
        }
    });

    // Register Dynamic TS Segment generator
    // GET /media/mserver/playback/hls/segment?file=xxx&offset_ms=xxx&duration_ms=xxx&secret=xxx
    auto segment_handler = [](API_ARGS_MAP_ASYNC) {
        GET_CONFIG(string, api_secret, API::kSecret);

        // Clean up parameter values polluted by ?token= or ?secret= due to client URL formatting typos
        string query_token;
        string query_secret;
        for (auto &pr : allArgs.args) {
            size_t pos = pr.second.find("?token=");
            if (pos != string::npos) {
                query_token = pr.second.substr(pos + 7);
                pr.second = pr.second.substr(0, pos);
            }
            pos = pr.second.find("?secret=");
            if (pos != string::npos) {
                query_secret = pr.second.substr(pos + 8);
                pr.second = pr.second.substr(0, pos);
            }
        }
        if (!query_token.empty()) {
            allArgs.args["token"] = query_token;
        }
        if (!query_secret.empty()) {
            allArgs.args["secret"] = query_secret;
        }

        CHECK_ARGS("file", "offset_ms", "duration_ms");

        // Extract token
        string token;
        if (!allArgs["token"].empty()) {
            token = (string)allArgs["token"];
        } else {
            for (const auto &pr : allArgs.getParser().getUrlArgs()) {
                size_t pos = pr.second.find("?token=");
                if (pos != string::npos) {
                    token = pr.second.substr(pos + 7);
                    break;
                }
            }
        }

        // If it is a session token, resolve the real credential
        if (token.rfind("sess_", 0) == 0) {
            string real_credential;
            if (HlsSessionTokenManager::Instance().verifySession(token, real_credential)) {
                if (real_credential.rfind("secret=", 0) == 0) {
                    string secret_val = real_credential.substr(7);
                    allArgs.args["secret"] = secret_val;
                } else if (real_credential.rfind("Bearer ", 0) == 0) {
                    allArgs.args["Authorization"] = real_credential;
                }
            }
        }

        // Extract secret if it was appended using a typo ?secret=
        if (allArgs["secret"].empty()) {
            for (const auto &pr : allArgs.getParser().getUrlArgs()) {
                size_t pos = pr.second.find("?secret=");
                if (pos != string::npos) {
                    allArgs.args["secret"] = pr.second.substr(pos + 8);
                    break;
                }
            }
        }

        bool is_secret_valid = false;
        try {
            CHECK_SECRET();
            is_secret_valid = true;
        } catch (...) {}

        if (!is_secret_valid) {
            if (!token.empty() && allArgs["Authorization"].empty()) {
                allArgs.args["Authorization"] = "Bearer " + token;
            }
        }

        // Demux + remux is blocking disk work: it must not run on the request EventPoller.
        auto req_poller = apiRequestPoller(sender);
        auto req_session = apiRequestSession(sender);

        auto on_authorized = [allArgs, invoker, headerOut, req_poller, req_session]() mutable {
            // TODO(merge-repo): khi port record encryption, đổi lại thành
            // TierEncryptionManager::Instance().materializeReadablePathForFile(...)
            string file = decodeBase64(allArgs["file"]);
            uint64_t offset_ms = std::stoull(allArgs["offset_ms"]);
            uint64_t duration_ms = std::stoull(allArgs["duration_ms"]);
            uint64_t dts_offset_ms = offset_ms;
            if (!allArgs["dts_offset_ms"].empty()) {
                dts_offset_ms = std::stoull(allArgs["dts_offset_ms"]);
            }

            auto invoker_copy = invoker;
            auto headerOut_copy = headerOut;
            auto ts_data = std::make_shared<string>();

            runApiWork(req_poller, req_session,
                [file, offset_ms, duration_ms, dts_offset_ms, ts_data]() {
                    struct stat st;
                    if (stat(file.c_str(), &st) != 0) {
                        throw std::runtime_error("File not found: " + file);
                    }

                    MP4Demuxer demuxer;
                    demuxer.openMP4(file);

                    TSMuxer tsMuxer;
                    auto tracks = demuxer.getTracks(true);
                    if (tracks.empty()) {
                        throw std::runtime_error("No tracks found in mp4 file: " + file);
                    }

                    // Sort tracks: Video first, then Audio
                    std::sort(tracks.begin(), tracks.end(), [](const Track::Ptr &a, const Track::Ptr &b) {
                        return a->getTrackType() < b->getTrackType();
                    });

                    int64_t first_dts = -1;
                    vector<pair<Track::Ptr, FrameWriterInterface*>> delegate_cleanup;

                    for (auto &track : tracks) {
                        tsMuxer.addTrack(track);
                        auto delegate_ptr = track->addDelegate([&tsMuxer, dts_offset_ms, &first_dts, duration_ms](const Frame::Ptr &frame) {
                            if (first_dts == -1) {
                                    first_dts = frame->dts();
                            }
                            int64_t rel_dts = (int64_t)frame->dts() - first_dts;
                            if (rel_dts >= (int64_t)duration_ms) {
                                return true;
                            }

                            // Discard SEI NAL units to prevent browser decode errors on malformed camera SEI packets
                            if (frame->getCodecId() == CodecH264) {
                                int type = H264_TYPE(frame->data()[frame->prefixSize()]);
                                if (type == H264Frame::NAL_SEI) {
                                    return true;
                                }
                            } else if (frame->getCodecId() == CodecH265) {
                                int type = H265_TYPE(frame->data()[frame->prefixSize()]);
                                if (type == H265Frame::NAL_SEI_PREFIX || type == H265Frame::NAL_SEI_SUFFIX) {
                                    return true;
                                }
                            }

                            int64_t rel_pts = (int64_t)frame->pts() - first_dts;

                            uint64_t relative_dts = std::max((int64_t)0, rel_dts + (int64_t)dts_offset_ms);
                            uint64_t relative_pts = std::max((int64_t)0, rel_pts + (int64_t)dts_offset_ms);
                            auto wrapped = std::make_shared<FrameWrap>(frame, relative_dts, relative_pts);

                            tsMuxer.inputFrame(wrapped);
                            return true;
                        });
                        delegate_cleanup.push_back({track, delegate_ptr});
                    }

                    int64_t seek_res = demuxer.seekTo(offset_ms);
                    if (seek_res < 0) {
                        // Seek failed
                    }

                    bool keyFrame = false;
                    bool eof = false;

                    while (true) {
                        auto frame = demuxer.readFrame(keyFrame, eof);
                        if (eof || !frame) {
                            break;
                        }
                        if (first_dts == -1) {
                            first_dts = frame->dts();
                        }
                        
                        int64_t rel_dts = (int64_t)frame->dts() - first_dts;
                        if (rel_dts >= (int64_t)duration_ms) {
                            break;
                        }
                    }

                    for (auto &pair : delegate_cleanup) {
                        pair.first->delDelegate(pair.second);
                    }

                    tsMuxer.flush();
                    demuxer.closeMP4();

                    *ts_data = tsMuxer.getTsData();
                },
                [ts_data, invoker_copy, headerOut_copy, req_session](const std::exception_ptr &error) mutable {
                    if (error) {
                        try {
                            std::rethrow_exception(error);
                        } catch (const std::exception &ex) {
                            WarnL << "Failed to slice TS segment: " << ex.what();
                        }
                        responseApiException(error, invoker_copy, req_session);
                        return;
                    }
                    headerOut_copy["Content-Type"] = "video/mp2t";
                    invoker_copy(200, headerOut_copy, *ts_data);
                });
        };

        if (is_secret_valid) {
            on_authorized();
        } else {
            try {
                CHECK_AUTH_TOKEN();
                CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
                on_authorized();
            } catch (const std::exception &ex) {
                Json::Value err;
                err["code"] = API::AuthFailed;
                err["msg"] = ex.what();
                headerOut["Content-Type"] = "application/json";
                invoker(401, headerOut, err.toStyledString());
            }
        }
    };
    api_regist("/media/mserver/playback/hls/segment", segment_handler);
    api_regist("/media/mserver/playback/hls/segment.ts", segment_handler);
#endif // defined(ENABLE_MP4) && defined(ENABLE_HLS)
}
