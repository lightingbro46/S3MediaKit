#include <algorithm>
#include "SdCardSyncManager.h"
#include "Util/util.h"
#include "Thread/WorkThreadPool.h"
#include "Local/StatisticRecorder.h"
#include "server/WebApi.h"
#include "RecordPolicy.h"
#include "Common/StrUtil.h"
#include "json/json.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(SdCardSyncManager)

static std::vector<VideoSegment> SplitEnabledRecordRanges(const VideoSegment& seg, const RecordScheduler::RecordScheduleMap& schedule) {
    std::vector<VideoSegment> result;
    time_t start_time = seg.start_time;
    time_t end_time   = seg.end_time;

    if (start_time >= end_time) return result;

    auto isRecordEnabledAt = [&](time_t t) -> bool {
        auto week_time = StrTimeUtils::getWeekTime(t);
        std::string key = (StrPrinter << week_time.day_of_week << "," << week_time.hour);
        auto it = schedule.find(key);
        if (it == schedule.end()) return false;
        return it->second.mode == RecordMode::RecordLowResAndMotion ||
               it->second.mode == RecordMode::RecordOnlyMotion ||
               it->second.mode == RecordMode::RecordAlways;
    };

    time_t cur = start_time;
    bool haveOpenRange = false;
    time_t rangeStart = 0;

    while (cur < end_time) {
        std::tm tmVal{};
#if defined(_WIN32)
        localtime_s(&tmVal, &cur);
#else
        localtime_r(&cur, &tmVal);
#endif
        tmVal.tm_min = 0;
        tmVal.tm_sec = 0;
        time_t hourStart = std::mktime(&tmVal);
        time_t nextHour  = hourStart + 3600;
        time_t segEnd    = std::min(nextHour, end_time);

        bool enabled = isRecordEnabledAt(cur);

        if (enabled && !haveOpenRange) {
            rangeStart = cur;
            haveOpenRange = true;
        } else if (!enabled && haveOpenRange) {
            VideoSegment seg;
            seg.start_time = rangeStart;
            seg.end_time = cur;
            result.push_back(seg);
            haveOpenRange = false;
        }

        cur = segEnd;
    }

    if (haveOpenRange) {
        VideoSegment seg;
        seg.start_time = rangeStart;
        seg.end_time = end_time;
        result.push_back(seg);
    }

    return result;
}

static std::pair<uint64_t, bool> calcProgressPos(const RecordingVideoSegment& seg, float progress) {
    const bool done = (progress > 0.99f);
    const uint64_t pos = done
                    ? static_cast<uint64_t>(seg.end_time)
                    : std::min(static_cast<uint64_t>(seg.end_time),
                    static_cast<uint64_t>(seg.start_time) +
                    static_cast<uint64_t>(progress * (seg.end_time - seg.start_time)));
    return {pos, done};
}

SdCardSyncManager::SdCardSyncManager() {}

bool SdCardSyncManager::hasCamera(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    return _camera_map.find(device_id) != _camera_map.end();
}

void SdCardSyncManager::addCamera(const std::string& device_id, const std::string& username, const std::string& password) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!hasCamera(device_id)) {
        auto tracker = loadConnectionTrackerFromDb(device_id);

        CameraContext ctx;
        ctx.device_id = device_id;
        ctx.username  = username;
        ctx.password  = password;
        ctx.tracker   = std::move(tracker);
        _camera_map[device_id] = std::move(ctx);
    } else {
        _camera_map[device_id].username = username;
        _camera_map[device_id].password = password;
    }
}

void SdCardSyncManager::onManager(const std::string &device_id, const std::string &username,
                                   const std::string &password, const OnvifControl::Ptr &onvif,
                                   const SdCardSyncConfig &sdSyncConfig, const bool &ready) {
    
    if (!onvif || !onvif->getSDCardInfo().isSDSupport()) {
        InfoL << "Skip SD sync for device: " << device_id
            << " reason: " << (!onvif ? "onvif controller is null" : "SD card not supported");
        return;
    }

    addCamera(device_id, username, password);
    if (!sdSyncConfig.sdCardSyncEnabled || !sdSyncConfig.sdCardSyncAutoSyncEnabled || !ready) {
        stopSession(device_id);
        return;
    }

    std::weak_ptr<SdCardSyncManager> weak_self = shared_from_this();
    uint64_t disconnectTime = 0, reconnectTime = 0;
    int min_segment_sec = sdSyncConfig.sdCardSyncMinSegmentGapSec;
    bool shouldSubmitTask = false;

    {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        _retry_count = sdSyncConfig.sdCardSyncRetryCount;

        auto& tracker = _camera_map[device_id].tracker;
        auto* session = tracker.getInitializingSession();
        if (session) {
            disconnectTime  = session->disconnect_time;
            reconnectTime   = session->reconnect_time;

            if (reconnectTime - disconnectTime < static_cast<uint64_t>(min_segment_sec)) {
                tracker.removeSession(*session);
            } else {
                shouldSubmitTask = true;
            }
        }
    }

    if (shouldSubmitTask) {
        WorkThreadPool::Instance().getExecutor()->async([weak_self, onvif, disconnectTime, reconnectTime, device_id, min_segment_sec]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) return;

            auto segments = onvif->findVideoSegments(disconnectTime, reconnectTime, min_segment_sec);
            auto rec_infos = onvif->getRecordingInformation();
            strong_self->handleReplayInformation(device_id, segments, rec_infos);
        });
    }

    {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (sdSyncConfig.sdCardSyncAutoSyncEnabled) {
            onManageCamera(_camera_map[device_id]);
        }
        syncConnectionTrackerToDb(device_id, _camera_map[device_id].tracker);
    }
}

void SdCardSyncManager::onManageCamera(CameraContext& ctx) {
    DisconnectSession* session = ctx.tracker.getProcessingSession();
    if (session) {
        handleProcessingSession(ctx, *session);
        return;
    }

    DisconnectSession* pending = ctx.tracker.getNextPendingSession();
    if (!pending) return;

    startPendingSession(ctx, *pending);
}

void SdCardSyncManager::handleProcessingSession(CameraContext& ctx, DisconnectSession& session) {
    InfoL << "Handling processing session: device=" << ctx.device_id
          << " disconnect=" << session.disconnect_time
          << " reconnect=" << session.reconnect_time;

    for (auto& info : session.stream_handle_infos) {
        if (info.handle_state != ConnectionHandlingState::Processing) continue;
        auto stream_id = info.stream_id;
        auto ptr = getReplayStreamProxy(ctx.shortUrl(stream_id));
        if (ptr) {
            auto progress = ptr->MediaPlayer::getProgress();
            auto cur_seg = ctx.tracker.getCurrentSegment(stream_id);
            if (cur_seg && cur_seg->state == ConnectionHandlingState::Processing) {
                auto result = calcProgressPos(*cur_seg, progress);
                ctx.tracker.updateSegmentProgress(stream_id, result.first);
                if (result.second) {
                    // Ensure the stream proxy is deleted after segment processing completes
                    delReplayStreamProxy(ctx.shortUrl(stream_id));
                }
            }
        } else {
            std::string url = ctx.tracker.getReplayURL(stream_id);
            uint64_t earliest_record_time = ctx.tracker.getEarliestRecordTime(stream_id);
            auto cur_seg = ctx.tracker.getCurrentSegment(stream_id);
            if (cur_seg && cur_seg->state == ConnectionHandlingState::Processing && cur_seg->processed_up_to < static_cast<uint64_t>(cur_seg->end_time)) {
                start(ctx, stream_id, url, earliest_record_time, cur_seg->processed_up_to, cur_seg->end_time);
            } else {
                auto next_seg = ctx.tracker.getNextPendingSegment(stream_id);
                if (!next_seg) continue;

                uint64_t startTime = next_seg->processed_up_to > 0 ? next_seg->processed_up_to : next_seg->start_time;
                start(ctx, stream_id, url, earliest_record_time, startTime, cur_seg->end_time);
                ctx.tracker.updateSegmentProgress(stream_id, startTime, next_seg->segment_index);
            }
        }
    }
}

void SdCardSyncManager::startPendingSession(CameraContext& ctx, DisconnectSession& session) {
    InfoL << "Starting pending session: device=" << ctx.device_id
          << " disconnect=" << session.disconnect_time
          << " reconnect=" << session.reconnect_time;

    ctx.tracker.markNextPendingProcessing();
    DisconnectSession* processing = ctx.tracker.getProcessingSession();
    if (!processing) return;

    for (auto& info : session.stream_handle_infos) {
        auto segment = ctx.tracker.getCurrentSegment(info.stream_id);
        if (!segment) continue;

        std::string url = ctx.tracker.getReplayURL(info.stream_id);
        uint64_t earliest_record_time = ctx.tracker.getEarliestRecordTime(info.stream_id);
        uint64_t startTime = segment->processed_up_to > 0 ? segment->processed_up_to : segment->start_time;
        start(ctx, info.stream_id, url, earliest_record_time, startTime, segment->end_time);
        ctx.tracker.updateSegmentProgress(info.stream_id, startTime, segment->segment_index);
    }
}

bool SdCardSyncManager::tryMakeStreamInformation(const std::unordered_map<std::string, VideoEncoderConfig>& encoderConfigMap, const std::vector<RecordingInformation>& recordingInformations, std::vector<StreamInfo>& streams) {
    streams.clear();

    if (encoderConfigMap.empty() || recordingInformations.empty()) {
        return false;
    }

    auto hasValidConfig = [](const VideoEncoderConfig& cfg) {
        return !cfg.vcodec.empty();
    };

    auto score = [](const VideoEncoderConfig& lhs, const VideoEncoderConfig& rhs) -> double {
        double result = 0.0;

        if (lhs.vcodec == rhs.vcodec)
            result += 30.0;

        if (lhs.width == rhs.width &&
            lhs.height == rhs.height) {
            result += 40.0;
        }

        if (lhs.bitrate > 0 &&
            rhs.bitrate > 0) {
            const double ratio = static_cast<double>(std::min(lhs.bitrate, rhs.bitrate)) / std::max(lhs.bitrate, rhs.bitrate);
            result += ratio * 20.0;
        }

        if (lhs.fps > 0.0f &&
            rhs.fps > 0.0f) {
            const double ratio = static_cast<double>(std::min(lhs.fps, rhs.fps)) / std::max(lhs.fps, rhs.fps);
            result += ratio * 10.0;
        }

        return result;
    };

    std::vector<std::pair<std::string, VideoEncoderConfig> > encoderConfigs(encoderConfigMap.begin(), encoderConfigMap.end());

    auto makeStreamInfo = [](const std::string& streamId, const RecordingInformation& rec) {
        StreamInfo info;
        info.stream_id = streamId;
        info.replay_uri = rec.uri;
        info.earliest_record_time = static_cast<uint64_t>(rec.earliestRecording);
        info.latest_record_time = static_cast<uint64_t>(rec.latestRecording);
        return info;
    };

    const size_t streamCount = encoderConfigs.size();
    const size_t recordCount = recordingInformations.size();

    bool canCompare = true;

    for (size_t i = 0; i < recordCount; ++i) {
        if (!hasValidConfig(recordingInformations[i].vEncoder)) {
            canCompare = false;
            break;
        }
    }

    if (canCompare) {
        for (size_t i = 0; i < streamCount; ++i) {
            if (!hasValidConfig(encoderConfigs[i].second)) {
                canCompare = false;
                break;
            }
        }
    }

    // 1 stream - 1 recording
    if (streamCount == 1 && recordCount == 1) {
        streams.push_back(makeStreamInfo(encoderConfigs[0].first, recordingInformations[0]));
        return true;
    }

    if (!canCompare) {
        return false;
    }

    // 2 stream - 1 recording
    if (streamCount == 2 && recordCount == 1) {
        const double s0 = score(recordingInformations[0].vEncoder, encoderConfigs[0].second);
        const double s1 = score(recordingInformations[0].vEncoder, encoderConfigs[1].second);

        const size_t idx = (s1 > s0) ? 1 : 0;

        if (std::max(s0, s1) < 90.0)
            return false;

        streams.push_back(makeStreamInfo(encoderConfigs[idx].first, recordingInformations[0]));

        return true;
    }

    // 1 stream - 2 recording
    if (streamCount == 1 && recordCount == 2) {
        const double s0 = score(recordingInformations[0].vEncoder, encoderConfigs[0].second);
        const double s1 = score(recordingInformations[1].vEncoder, encoderConfigs[0].second);

        const size_t idx = (s1 > s0) ? 1 : 0;

        if (std::max(s0, s1) < 90.0)
            return false;

        streams.push_back(makeStreamInfo(encoderConfigs[0].first, recordingInformations[idx]));

        return true;
    }

    // 2 stream - 2 recording
    if (streamCount == 2 && recordCount == 2) {
        const double mappingA = score(recordingInformations[0].vEncoder, encoderConfigs[0].second) + score(recordingInformations[1].vEncoder, encoderConfigs[1].second);
        const double mappingB = score(recordingInformations[0].vEncoder, encoderConfigs[1].second) + score(recordingInformations[1].vEncoder, encoderConfigs[0].second);

        if (std::max(mappingA, mappingB) < 180.0)
            return false;

        if (mappingA >= mappingB) {
            streams.push_back(makeStreamInfo(encoderConfigs[0].first, recordingInformations[0]));
            streams.push_back(makeStreamInfo(encoderConfigs[1].first, recordingInformations[1]));
        }
        else {
            streams.push_back(makeStreamInfo(encoderConfigs[1].first, recordingInformations[0]));
            streams.push_back(makeStreamInfo(encoderConfigs[0].first, recordingInformations[1]));
        }

        return true;
    }

    return false;
}

void SdCardSyncManager::handleReplayInformation(const std::string device_id,
    const std::unordered_map<int, std::vector<VideoSegment>>& segments,
    const std::vector<RecordingInformation>& rec_infos)
{

    if (!hasCamera(device_id)) return;

    auto getCameraStats = [](const std::string& device_id) -> CameraStatisticImp::Ptr {
        DeviceTuple tuple;
        tuple.vhost     = DEFAULT_VHOST;
        tuple.device_id = device_id;
        auto ret = DeviceSource::find(tuple.vhost, tuple.device_id);
        if (!ret) return nullptr;

        auto weak_listener = ret->getListener();
        if (auto strong_listener = weak_listener.lock()) {
            auto ptr = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
            if (ptr) return ptr->getCameraStatisticImp();
        }
        return nullptr;
    };

    auto stats_imp = getCameraStats(device_id);
    if (!stats_imp) return;

    auto encoderConfigMap = stats_imp->getStreamVideoEncoderConfigMap();
    std::vector<StreamInfo> streams;
    if (!tryMakeStreamInformation(encoderConfigMap, rec_infos, streams)) {
        streams.clear();
        for (const auto& info : rec_infos) {
            StreamInfo sInfo;
            sInfo.stream_id            = stats_imp->getStreamID(info.streamType);
            sInfo.replay_uri           = info.uri;
            sInfo.earliest_record_time = info.earliestRecording;
            sInfo.latest_record_time   = info.latestRecording;
            streams.push_back(sInfo);
        }
    }

    auto records = RecordScheduler::parseRecordScheduleStr(stats_imp->getParams().option.recordSchedules);
    std::unordered_map<std::string, std::vector<RecordingVideoSegment>> segs;
    int totalSeg = 0;

    for (const auto& item : segments) {
        std::string stream_id = stats_imp->getStreamID(item.first);
        if (stream_id.empty()) continue;

        std::vector<RecordingVideoSegment> outVec;
        for (const auto& seg : item.second) {
            for (const auto& range : SplitEnabledRecordRanges(seg, records)) {
                RecordingVideoSegment seg;
                seg.start_time = static_cast<uint64_t>(range.start_time);
                seg.end_time = static_cast<uint64_t>(range.end_time);
                outVec.push_back(seg);
            }
        }

        if (!outVec.empty()) {
            totalSeg += outVec.size();
            segs[stream_id] = std::move(outVec);
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!hasCamera(device_id)) return;

        auto& tracker = _camera_map[device_id].tracker;
        tracker.setStreams(streams);

        auto* session = tracker.getInitializingSession();
        if (session) {
            if (segs.empty()) {
                WarnL << "No segments in the session match the recording schedule";
                tracker.removeSession(*session);
            } else {
                InfoL << "Detected a new session to synchronize: start=" << session->disconnect_time
                      << ", end=" << session->reconnect_time
                      << ", total segments=" << totalSeg;
                tracker.insertSegments(*session, segs);
            }
        }
        syncConnectionTrackerToDb(device_id, tracker);
    }
}

void SdCardSyncManager::start(CameraContext& ctx, const std::string& stream_id, const std::string& url, const uint64_t& earliestRecord, const uint64_t& start, const uint64_t& end) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!ctx.isValid()) {
        WarnL << "Invalid camera context. deviceId=" << ctx.device_id;
        return;
    }
    std::weak_ptr<SdCardSyncManager> weak_self = shared_from_this();

    std::string device_id = ctx.device_id;
    std::string username = ctx.username;
    std::string password = ctx.password;
    WorkThreadPool::Instance().getExecutor()->async([weak_self, device_id, username, password, stream_id, url, earliestRecord, start, end]() { 
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }

        mediakit::ProtocolOption protocol;
        const std::string app_name = kReplayPrefix + device_id;
        MediaTuple tuple(DEFAULT_VHOST, app_name, stream_id, "");

        auto setup_player = [&](const string &err, const PlayerProxy::Ptr &player) {
            // return if player proxy with same key exist
            if (!err.empty()) {
                WarnL << "Create replay stream player proxy " << tuple.shortUrl() << " failed: " << err;
                return;
            }

            (*player)[Client::kRtpMode] = 1;    //replay mode

            player->setOnReplayRetry([weak_self](const std::string &proxyKey, const float &progress, std::string &newUrl) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return false;
                }
                return strong_self->onSegmentRetry(proxyKey, progress, newUrl);
            });

            player->setOnReplayClose([weak_self](const std::string &proxyKey, const float &progress) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                strong_self->onSegmentShutdown(proxyKey, progress);
            });

            std::string play_url = strong_self->buildReplayUrl(username, password, url, (start - earliestRecord) * 1000, (end - earliestRecord) * 1000);
            if (!play_url.empty()) {
                player->setReplayRecorderTimeFile(start);
                player->play(play_url);
                DebugL << "Created replay stream player proxy " << tuple.shortUrl();
            }
        };

        addReplayStreamProxy(tuple, protocol, setup_player, strong_self->_retry_count);
    });
}

std::string SdCardSyncManager::buildReplayUrl(const std::string& username, const std::string& password, const std::string& replay_uri, uint64_t start_ms, uint64_t end_ms) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    const std::string schema     = "rtsp://";
    const std::string schema_ssl = "rtsps://";

    std::string prefix;
    std::string host_path;

    if (replay_uri.rfind(schema, 0) == 0) {
        prefix    = schema;
        host_path = replay_uri.substr(schema.size());
    } else if (replay_uri.rfind(schema_ssl, 0) == 0) {
        prefix    = schema_ssl;
        host_path = replay_uri.substr(schema_ssl.size());
    } else {
        WarnL << "replay_uri not valid: " << replay_uri;
        return "";
    }

    std::ostringstream oss;
    oss << prefix;

    if (!username.empty()) {
        oss << username;
        if (!password.empty()) oss << ":" << password;
        oss << "@";
    }

    oss << host_path
        << "&starttime=" << start_ms
        << "&endtime="   << end_ms;

    return oss.str();
}

void SdCardSyncManager::onSegmentShutdown(const std::string &proxyKey, const float &progress) {
    size_t first  = proxyKey.find('/');
    size_t second = (first != std::string::npos) ? proxyKey.find('/', first + 1) : std::string::npos;
    if (first == std::string::npos || second == std::string::npos) {
        WarnL << "Invalid proxyKey format: " << proxyKey;
        return;
    }

    std::string device_id = proxyKey.substr(first + 1, second - first - 1);
    std::string stream_id = proxyKey.substr(second + 1);

    if (start_with(device_id, kReplayPrefix)) {
        device_id.erase(0, kReplayPrefix.size());
    }

    TraceL << "Segment shutdown: device=" << device_id
           << " stream=" << stream_id
           << " progress=" << progress;

    {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!hasCamera(device_id)) {
            WarnL << "Camera not found: " << device_id;
            return;
        }

        auto& tracker  = _camera_map[device_id].tracker;
        auto* segment  = tracker.getCurrentSegment(stream_id);
        if (!segment) {
            WarnL << "No current segment for stream: " << stream_id << " device=" << device_id;
            return;
        }

        auto result = calcProgressPos(*segment, progress);
        tracker.updateSegmentProgress(stream_id, result.first, true);
        syncConnectionTrackerToDb(device_id, tracker);
    }

    TraceL << "Removing replay stream proxy: " << proxyKey;
    delReplayStreamProxy(proxyKey);
}

bool SdCardSyncManager::onSegmentRetry(const std::string &proxyKey, const float &progress, std::string &newUrl) {
    size_t first  = proxyKey.find('/');
    size_t second = (first != std::string::npos) ? proxyKey.find('/', first + 1) : std::string::npos;
    if (first == std::string::npos || second == std::string::npos) {
        WarnL << "Invalid proxyKey format: " << proxyKey;
        return false;
    }

    std::string device_id = proxyKey.substr(first + 1, second - first - 1);
    std::string stream_id = proxyKey.substr(second + 1);

    if (start_with(device_id, kReplayPrefix)) {
        device_id.erase(0, kReplayPrefix.size());
    }

    TraceL << "Segment retry: device=" << device_id
           << " stream=" << stream_id
           << " progress=" << progress;

    bool shouldRetry = false;
    {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!hasCamera(device_id)) {
            WarnL << "Camera not found: " << device_id;
            return false;
        }

        auto& context = _camera_map[device_id];
        auto& tracker = context.tracker;
        auto* segment = tracker.getCurrentSegment(stream_id);
        if (!segment) {
            WarnL << "No current segment for stream=" << stream_id << " device=" << device_id;
            return false;
        }

        auto result = calcProgressPos(*segment, progress);
        tracker.updateSegmentProgress(stream_id, result.first, result.second);

        if (!result.second) {
            std::string url = tracker.getReplayURL(stream_id);
            const uint64_t earliest = tracker.getEarliestRecordTime(stream_id);
            newUrl = buildReplayUrl(context.username, context.password, url,
                                    (result.first - earliest) * 1000,
                                    (segment->end_time - earliest) * 1000);
            InfoL << "Built retry URL: device=" << device_id << " stream=" << stream_id << " newUrl=" << newUrl;
            shouldRetry = true;
        }
        syncConnectionTrackerToDb(device_id, tracker);
    }

    if (!shouldRetry) {
        TraceL << "Segment done, removing proxy: " << proxyKey;
        delReplayStreamProxy(proxyKey);
    }

    return shouldRetry;
}

bool SdCardSyncManager::resetFailedSession(const std::string& device_id, const std::string& session_id) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return false;
    }

    auto& tracker = _camera_map[device_id].tracker;
    bool ret = tracker.resetFailedSession(session_id);
    if (ret) {
        InfoL << "Reset failed session: device=" << device_id << " session=" << session_id;
        syncConnectionTrackerToDb(device_id, tracker);
    } else {
        WarnL << "Reset failed session not found: device=" << device_id << " session=" << session_id;
    }
    return ret;
}

bool SdCardSyncManager::resetFailedSegment(const std::string& device_id,
                                            const std::string& session_id,
                                            const std::string& stream_id,
                                            int32_t segment_index) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return false;
    }

    auto& tracker = _camera_map[device_id].tracker;
    bool ret = tracker.resetFailedSegment(session_id, stream_id, segment_index);
    if (ret) {
        InfoL << "Reset failed segment: device=" << device_id
              << " session=" << session_id
              << " stream=" << stream_id
              << " index=" << segment_index;
        syncConnectionTrackerToDb(device_id, tracker);
    } else {
        WarnL << "Reset failed segment not found: device=" << device_id
              << " session=" << session_id
              << " stream=" << stream_id
              << " index=" << segment_index;
    }
    return ret;
}

bool SdCardSyncManager::stopSession(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    bool ret = false;
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return false;
    }

    auto& context = _camera_map[device_id];
    auto& tracker = context.tracker;
    auto session = tracker.getProcessingSession();
    if (!session) {
        TraceL << "No processing session for device: " << device_id;
        return false;
    }

    for (auto& info : session->stream_handle_infos) {
        if (info.handle_state != ConnectionHandlingState::Processing) continue;

        std::string key = context.shortUrl(info.stream_id);
        auto ptr = getReplayStreamProxy(key);
        if (ptr) {
            auto cur_seg = tracker.getCurrentSegment(info.stream_id);
            if (cur_seg && cur_seg->state == ConnectionHandlingState::Processing) {
                auto progress = ptr->MediaPlayer::getProgress();
                auto result = calcProgressPos(*cur_seg, progress);
                tracker.updateSegmentProgress(info.stream_id, result.first);
            }
            delReplayStreamProxy(key);
        }
    }

    ret = tracker.stopSession();
    if (ret) {
        InfoL << "Session stopped: device=" << device_id;
        syncConnectionTrackerToDb(device_id, tracker);
    } else {
        WarnL << "Stop session failed: device=" << device_id;
    }
    return ret;
}

bool SdCardSyncManager::stopSegment(const std::string& device_id, const std::string& stream_id, int32_t segment_index) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    bool ret = false;
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return false;
    }

    auto& context = _camera_map[device_id];
    auto& tracker = context.tracker;
    auto& sessions = tracker.sessions();
    auto session = tracker.getProcessingSession();
    if (!session) {
        WarnL << "No processing session for device: " << device_id;
        return false;
    }

    for (auto& info : session->stream_handle_infos) {
        if (info.stream_id != stream_id) continue;
        if (info.handle_state != ConnectionHandlingState::Processing) {
            WarnL << "Stream not in processing state: device=" << device_id << " stream=" << stream_id;
            return false;
        }

        std::string key = context.shortUrl(stream_id);
        auto ptr = getReplayStreamProxy(key);
        if (ptr) {
            auto progress = ptr->MediaPlayer::getProgress();
            auto cur_seg = tracker.getCurrentSegment(stream_id);
            if (cur_seg && cur_seg->state == ConnectionHandlingState::Processing) {
                auto result = calcProgressPos(*cur_seg, progress);
                tracker.updateSegmentProgress(stream_id, result.first);
            }
            delReplayStreamProxy(key);
        }
    }

    ret = tracker.stopSegment(stream_id, segment_index);
    if (ret) {
        InfoL << "Segment stopped: device=" << device_id << " stream=" << stream_id << " index=" << segment_index;
        syncConnectionTrackerToDb(device_id, tracker);
    } else {
        WarnL << "Stop segment failed: device=" << device_id << " stream=" << stream_id << " index=" << segment_index;
    }
    return ret;
}

DisconnectSessionManager SdCardSyncManager::loadConnectionTrackerFromDb(const std::string& device_id) {
    InfoL << "Loading connection tracker from DB: device=" << device_id;
    DisconnectSessionManager tracker;
    auto streamImp  = std::make_shared<SdSyncStreamImp>();
    auto sessionImp = std::make_shared<SdSyncSessionImp>();

    auto sdStreams = streamImp->findByDevice(device_id);
    TraceL << "Loaded " << sdStreams.size() << " streams from DB: device=" << device_id;

    std::vector<StreamInfo> streams;
    streams.reserve(sdStreams.size());
    for (const auto& sdStream : sdStreams) {
        streams.push_back(StreamInfo::from(sdStream));
    }
    tracker.setStreams(streams, true);

    auto sdSessions = sessionImp->findByDevice(device_id);
    TraceL << "Loaded " << sdSessions.size() << " sessions from DB: device=" << device_id;

    if (sdSessions.empty()) {
        InfoL << "No sessions found in DB: device=" << device_id;
        return tracker;
    }

    std::vector<DisconnectSession> sessions;
    sessions.reserve(sdSessions.size());

    for (const auto& sdSession : sdSessions) {
        auto progresses = sessionImp->findStreamInfoBySession(device_id, sdSession.session_id);
        TraceL << "Loaded " << progresses.size() << " stream progresses"
               << " for session=" << sdSession.session_id;

        std::vector<std::vector<SdSyncSegment>> segmentsPerStream;
        segmentsPerStream.reserve(progresses.size());

        for (const auto& prog : progresses) {
            auto segs = sessionImp->findSegmentByStream(device_id, sdSession.session_id, prog.stream_id);
            TraceL << "Loaded " << segs.size() << " segments"
                   << " for session=" << sdSession.session_id
                   << " stream=" << prog.stream_id;
            segmentsPerStream.push_back(std::move(segs));
        }

        sessions.push_back(DisconnectSession::from(sdSession, progresses, segmentsPerStream));
    }

    tracker.setSessions(sessions);
    InfoL << "Connection tracker loaded: device=" << device_id << " streams=" << streams.size() << " sessions=" << sessions.size();

    return tracker;
}

void SdCardSyncManager::syncConnectionTrackerToDb(const std::string& device_id, DisconnectSessionManager& tracker) {
    TraceL << "Syncing connection tracker to DB: device=" << device_id;
    auto streamImp  = std::make_shared<SdSyncStreamImp>();
    auto sessionImp = std::make_shared<SdSyncSessionImp>();

    int syncedStreams = 0;
    for (auto& stream : tracker.streams()) {
        if (!stream.dirty) continue;
        TraceL << "Syncing stream: device=" << device_id << " stream=" << stream.stream_id;
        streamImp->addOrUpdate(stream.toSdSyncStream(device_id));
        stream.dirty = false;
        ++syncedStreams;
    }
    if (syncedStreams > 0) {
        TraceL << "Synced " << syncedStreams << " streams: device=" << device_id;
    }

    int deletedSessions = 0;
    for (auto& session : tracker.drainDeletedSessions()) {
        TraceL << "Removing session: device=" << device_id << " session=" << session.session_id;
        for (auto& handle : session.stream_handle_infos) {
            for (auto& segment : handle.segments) {
                sessionImp->removeSegment(segment.toSdSyncSegment(device_id, session.session_id, handle.stream_id));
            }
            sessionImp->removeStreamInfo(handle.toSdSyncStreamProgress(device_id, session.session_id));
        }
        sessionImp->remove(session.toSdSyncSession(device_id));
        ++deletedSessions;
    }
    if (deletedSessions > 0) {
        TraceL << "Removed " << deletedSessions << " sessions: device=" << device_id;
    }

    int syncedSessions = 0;
    for (auto& session : tracker.sessions()) {
        if (!session.meta_dirty && !session.hasDirtyChild()) continue;

        if (session.meta_dirty) {
            sessionImp->addOrUpdate(session.toSdSyncSession(device_id));
            session.meta_dirty = false;
            ++syncedSessions;
        }

        for (auto& handle : session.stream_handle_infos) {
            if (!handle.meta_dirty && !handle.hasDirtyChild()) continue;

            if (handle.meta_dirty) {
                sessionImp->addOrUpdateStreamInfo(handle.toSdSyncStreamProgress(device_id, session.session_id));
                handle.meta_dirty = false;
            }

            for (auto& segment : handle.segments) {
                if (!segment.dirty) continue;

                sessionImp->addOrUpdateSegment(segment.toSdSyncSegment(device_id, session.session_id, handle.stream_id));
                segment.dirty = false;
            }
        }
    }
    if (syncedSessions > 0) {
        InfoL << "Synced " << syncedSessions << " sessions: device=" << device_id;
    }

    TraceL << "Sync connection tracker done: device=" << device_id;
}

void SdCardSyncManager::onDeviceStateChanged(const std::string &device_id, const bool &connect) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    if (!hasCamera(device_id)) {
        TraceL << "Camera not found: " << device_id;
        return;
    }

    TraceL << "Device state changed: device=" << device_id
          << " state=" << (connect ? "connected" : "disconnected");

    auto& tracker = _camera_map[device_id].tracker;
    auto current_time = time(nullptr);
    if (connect) {
        if (tracker.onReconnected(current_time)) {
            InfoL << "Tracker updated on reconnect, syncing to DB: device=" << device_id << ", time: " << current_time;
            syncConnectionTrackerToDb(device_id, tracker);
        }
    } else {
        tracker.onDisconnected(current_time);
    }
}

Json::Value SdCardSyncManager::makeRemainingSegmentsJson(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    Json::Value sessions_json = Json::arrayValue;
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return sessions_json;
    }
    for (const auto &session : _camera_map[device_id].tracker.getRemainingSegments()) {
        Json::Value session_json = Json::objectValue;
        session_json["session_id"] = session.session_id;
        session_json["disconnect_time"] = session.disconnect_time;
        session_json["reconnect_time"] = session.reconnect_time;

        sessions_json.append(session_json);
    }
    return sessions_json;
}

Json::Value SdCardSyncManager::makeDisconnectSessionJson(const std::string& device_id, const std::string& session_id) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    Json::Value session_json = Json::objectValue;
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return session_json;
    }
    for (const auto& session : _camera_map[device_id].tracker.sessions()) {
        if (session.session_id != session_id) continue;

        session_json["session_id"] = session.session_id;
        session_json["disconnect_time"] = session.disconnect_time;
        session_json["reconnect_time"] = session.reconnect_time;
        session_json["processing_start_time"] = session.processing_start_time;
        session_json["processing_end_time"] = session.processing_end_time;
        session_json["session_state"] = session.session_state;
        session_json["progress_percent"] = session.progress_percent;

        Json::Value handles_json = Json::arrayValue;
        for (const auto& info : session.stream_handle_infos) {
            Json::Value handle_json = Json::objectValue;
            handle_json["stream_id"] = info.stream_id;
            handle_json["handle_state"] = info.handle_state;

            Json::Value segments_json = Json::arrayValue;
            for (const auto& seg : info.segments) {
                Json::Value seg_json = Json::objectValue;
                seg_json["segment_index"] = seg.segment_index;
                seg_json["start_time"] = seg.start_time;
                seg_json["end_time"] = seg.end_time;
                seg_json["state"] = seg.state;
                seg_json["processed_up_to"] = seg.processed_up_to;
                segments_json.append(seg_json);
            }
            handle_json["segments"] = segments_json;
            handles_json.append(handle_json);
        }
        session_json["stream_handle_infos"] = handles_json;
    }
    return session_json;
}

Json::Value SdCardSyncManager::makeDisconnectSessionsJson(const std::string& device_id) {
    std::lock_guard<std::recursive_mutex> lck(_mtx);
    Json::Value sessions_json = Json::arrayValue;
    if (!hasCamera(device_id)) {
        WarnL << "Camera not found: " << device_id;
        return sessions_json;
    }
    for (const auto& session :  _camera_map[device_id].tracker.sessions()) {
        if (session.isInitializing()) continue;
        Json::Value session_json = Json::objectValue;
        session_json["session_id"] = session.session_id;
        session_json["disconnect_time"] = session.disconnect_time;
        session_json["reconnect_time"] = session.reconnect_time;
        session_json["processing_start_time"] = session.processing_start_time;
        session_json["processing_end_time"] = session.processing_end_time;
        session_json["session_state"] = session.session_state;
        session_json["progress_percent"] = session.progress_percent;

        Json::Value handles_json = Json::arrayValue;
        for (const auto& info : session.stream_handle_infos) {
            Json::Value handle_json = Json::objectValue;
            handle_json["stream_id"] = info.stream_id;
            handle_json["handle_state"] = info.handle_state;

            Json::Value segments_json = Json::arrayValue;
            for (const auto& seg : info.segments) {
                Json::Value seg_json = Json::objectValue;
                seg_json["segment_index"] = seg.segment_index;
                seg_json["start_time"] = seg.start_time;
                seg_json["end_time"] = seg.end_time;
                seg_json["state"] = seg.state;
                seg_json["processed_up_to"] = seg.processed_up_to;
                segments_json.append(seg_json);
            }
            handle_json["segments"] = segments_json;
            handles_json.append(handle_json);
        }
        session_json["stream_handle_infos"] = handles_json;
        sessions_json.append(session_json);
    }

    return sessions_json;
}

} // namespace managerkit