#include "MediaGapFillManager.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <set>
#include <sstream>

#include "Camera/GenericRtspCameraImp.h"
#include "Camera/RecordPolicy.h"
#include "Common/config.h"
#include "Player/PlayerProxy.h"
#include "Record/Recorder.h"
#include "Storage/DbStorage.h"
#include "Storage/MediaGapFillTask.h"
#include "Common/StrUtil.h"
#include "server/WebApi.h"
#include "Util/NoticeCenter.h"
#include "Thread/WorkThreadPool.h"

using namespace mediakit;
using namespace toolkit;

namespace managerkit {

namespace GapFillState {
static const char *const Pending = "Pending";
static const char *const Processing = "Processing";
static const char *const Success = "Success";
static const char *const Failed = "Failed";
}

namespace {

std::map<int, std::string> getConfiguredStreamMap(const std::string &device_id) {
    std::map<int, std::string> stream_quality;
    Broadcast::StreamQualityInvoker invoker =
        [&](const std::map<int, std::string> &streams) { stream_quality = streams; };
    const std::string vhost = DEFAULT_VHOST;
    NOTICE_EMIT(BroadcastGetStreamQualityArgs, Broadcast::kBroadcastGetStreamQuality,
                vhost, device_id, invoker);
    return stream_quality;
}

CameraStatisticImp::Ptr getCameraStats(const std::string &device_id) {
    auto source = DeviceSource::find(DEFAULT_VHOST, device_id);
    if (!source) return nullptr;
    auto listener = source->getListener().lock();
    auto camera = std::dynamic_pointer_cast<GenericRtspCameraImp>(listener);
    return camera ? camera->getCameraStatisticImp() : nullptr;
}

std::vector<VideoSegment> splitEnabledRecordRanges(
    const VideoSegment &segment, const RecordScheduler::RecordScheduleMap &schedule) {
    std::vector<VideoSegment> result;
    time_t current = segment.start_time;
    const time_t end_time = segment.end_time;
    if (current >= end_time) return result;

    bool range_open = false;
    time_t range_start = 0;
    while (current < end_time) {
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &current);
#else
        localtime_r(&current, &local);
#endif
        local.tm_min = 0;
        local.tm_sec = 0;
        const time_t next_hour = std::mktime(&local) + 3600;
        const time_t range_end = std::min(next_hour, end_time);
        const auto week_time = StrTimeUtils::getWeekTime(current);
        const std::string key = StrPrinter << week_time.day_of_week << ',' << week_time.hour;
        const auto it = schedule.find(key);
        const bool enabled = it != schedule.end() &&
            (it->second.mode == RecordMode::RecordLowResAndMotion ||
             it->second.mode == RecordMode::RecordOnlyMotion ||
             it->second.mode == RecordMode::RecordAlways);

        if (enabled && !range_open) {
            range_open = true;
            range_start = current;
        } else if (!enabled && range_open) {
            VideoSegment enabled_segment;
            enabled_segment.start_time = range_start;
            enabled_segment.end_time = current;
            result.emplace_back(std::move(enabled_segment));
            range_open = false;
        }
        current = range_end;
    }
    if (range_open) {
        VideoSegment enabled_segment;
        enabled_segment.start_time = range_start;
        enabled_segment.end_time = end_time;
        result.emplace_back(std::move(enabled_segment));
    }
    return result;
}

MediaGapFillTask makeTask(const MediaGapFillRequest &request, uint64_t processed_up_to,
                          int retry_count, const std::string &status,
                          const std::string &error) {
    MediaGapFillTask task;
    task.connection_id = request.id;
    task.gap_id = request.gap_id;
    task.session_id = request.session_id;
    task.device_id = request.device_id;
    task.stream_id = request.stream_id;
    task.source_type = request.source_type;
    task.origin_url = request.origin_url;
    task.source_begin_time = request.source_begin_time;
    task.segment_index = request.segment_index;
    task.disconnect_time = request.disconnect_time;
    task.reconnect_time = request.reconnect_time;
    task.begin_time = request.begin_time;
    task.end_time = request.end_time;
    task.processed_up_to = processed_up_to;
    task.processing_start_time = status == GapFillState::Processing
        ? static_cast<uint64_t>(time(nullptr)) : 0;
    task.processing_end_time = (status == GapFillState::Success || status == GapFillState::Failed)
        ? static_cast<uint64_t>(time(nullptr)) : 0;
    task.retry_count = retry_count;
    task.retry_limit = request.retry_limit;
    task.last_attempt_time = status == GapFillState::Processing
        ? static_cast<uint64_t>(time(nullptr)) : 0;
    task.state = status;
    task.last_error = error;
    task.updated_time = static_cast<uint64_t>(time(nullptr));
    return task;
}

MediaGapFillRequest requestFromTask(const MediaGapFillTask &task) {
    MediaGapFillRequest request;
    request.id = task.connection_id;
    request.source_type = task.source_type;
    request.gap_id = task.gap_id;
    request.session_id = task.session_id;
    request.device_id = task.device_id;
    request.stream_id = task.stream_id;
    request.origin_url = task.origin_url;
    request.source_begin_time = task.source_begin_time;
    request.begin_time = task.begin_time;
    request.end_time = task.end_time;
    request.disconnect_time = task.disconnect_time;
    request.reconnect_time = task.reconnect_time;
    request.segment_index = task.segment_index;
    request.retry_limit = task.retry_limit;
    return request;
}

std::string sessionKey(const std::string &device_id, const std::string &session_id) {
    return device_id + '\n' + session_id;
}

std::string streamKey(const MediaGapFillRequest &request) {
    return sessionKey(request.device_id, request.session_id) + '\n' + request.stream_id;
}

bool persistTask(const MediaGapFillRequest &request, uint64_t processed_up_to,
                 int retry_count, const std::string &status,
                 const std::string &error = "", uint64_t next_retry_time = 0) {
    MediaGapFillTaskImpl repository;
    auto task = makeTask(request, processed_up_to, retry_count, status, error);
    task.next_retry_time = next_retry_time;
    const auto existing = repository.findById(request.id);
    if (!existing.empty()) {
        if (existing.front().processing_start_time != 0)
            task.processing_start_time = existing.front().processing_start_time;
        if (task.processing_end_time == 0)
            task.processing_end_time = existing.front().processing_end_time;
        if (task.last_attempt_time == 0)
            task.last_attempt_time = existing.front().last_attempt_time;
    }
    return repository.addOrUpdate(task);
}

std::string addCredentials(const MediaGapFillRequest &request) {
    const std::string rtsp = "rtsp://";
    const std::string rtsps = "rtsps://";
    std::string schema;
    std::string remainder;
    if (request.origin_url.compare(0, rtsp.size(), rtsp) == 0) {
        schema = rtsp;
        remainder = request.origin_url.substr(rtsp.size());
    } else if (request.origin_url.compare(0, rtsps.size(), rtsps) == 0) {
        schema = rtsps;
        remainder = request.origin_url.substr(rtsps.size());
    } else {
        return std::string();
    }

    std::ostringstream url;
    url << schema;
    if (!request.username.empty()) {
        url << request.username;
        if (!request.password.empty()) {
            url << ':' << request.password;
        }
        url << '@';
    }
    url << remainder;
    return url.str();
}

std::string appendRtspRange(const MediaGapFillRequest &request) {
    auto url = addCredentials(request);
    if (url.empty() || request.source_begin_time > request.begin_time) {
        return std::string();
    }
    const char separator = url.find('?') == std::string::npos ? '?' : '&';
    std::ostringstream result;
    result << url << separator
           << "starttime=" << (request.begin_time - request.source_begin_time) * 1000
           << "&endtime=" << (request.end_time - request.source_begin_time) * 1000;
    return result.str();
}

std::string relayStream(const std::string &stream_id) {
    return StrMediaUtils::isRelayStream(stream_id) ? stream_id : StrMediaUtils::makeRelayStream(stream_id);
}

} // namespace

INSTANCE_IMP(MediaGapFillManager)

MediaGapFillManager::MediaGapFillManager() {
    _progress_timer = std::make_shared<Timer>(5.0f, [this]() {
        checkpointProgress();
        schedule();
        return true;
    }, EventPollerPool::Instance().getPoller());
}

MediaGapPlaybackProgress MediaGapFillManager::calculatePlaybackProgress(
    uint64_t begin_time, uint64_t end_time, double raw_progress) {
    MediaGapPlaybackProgress result;
    result.processed_up_to = begin_time;
    if (begin_time >= end_time) {
        result.processed_up_to = end_time;
        result.completed = true;
        return result;
    }
    if (!std::isfinite(raw_progress)) raw_progress = 0.0;
    raw_progress = std::max(0.0, std::min(1.0, raw_progress));
    result.completed = raw_progress > 0.99;
    if (result.completed) {
        result.processed_up_to = end_time;
    } else {
        result.processed_up_to = begin_time + static_cast<uint64_t>(
            raw_progress * static_cast<double>(end_time - begin_time));
    }
    return result;
}

uint64_t MediaGapFillManager::calculateRetryDelay(int retry_count) {
    const int normalized = std::max(1, retry_count);
    return static_cast<uint64_t>(std::max(2, std::min(60, (normalized - 1) * 3)));
}

std::vector<MediaGapStreamMapping> MediaGapFillManager::mapRecordingsToStreams(
    const std::unordered_map<std::string, VideoEncoderConfig> &encoder_configs,
    const std::vector<RecordingInformation> &recordings) {
    std::vector<std::pair<std::string, VideoEncoderConfig>> streams(
        encoder_configs.begin(), encoder_configs.end());
    std::sort(streams.begin(), streams.end(),
              [](const std::pair<std::string, VideoEncoderConfig> &lhs,
                 const std::pair<std::string, VideoEncoderConfig> &rhs) {
                  return lhs.first < rhs.first;
              });
    if (streams.empty() || recordings.empty()) return {};

    auto valid = [](const VideoEncoderConfig &config) { return !config.vcodec.empty(); };
    auto score = [](const VideoEncoderConfig &lhs, const VideoEncoderConfig &rhs) {
        double value = lhs.vcodec == rhs.vcodec ? 30.0 : 0.0;
        if (lhs.width == rhs.width && lhs.height == rhs.height) value += 40.0;
        if (lhs.bitrate > 0 && rhs.bitrate > 0)
            value += 20.0 * static_cast<double>(std::min(lhs.bitrate, rhs.bitrate)) /
                     static_cast<double>(std::max(lhs.bitrate, rhs.bitrate));
        if (lhs.fps > 0 && rhs.fps > 0)
            value += 10.0 * static_cast<double>(std::min(lhs.fps, rhs.fps)) /
                     static_cast<double>(std::max(lhs.fps, rhs.fps));
        return value;
    };
    auto mapping = [](const std::string &stream_id, size_t recording_index) {
        MediaGapStreamMapping result;
        result.stream_id = stream_id;
        result.recording_index = recording_index;
        return result;
    };

    if (streams.size() == 1 && recordings.size() == 1)
        return {mapping(streams[0].first, 0)};
    if (streams.size() > 2 || recordings.size() > 2) return {};
    for (const auto &stream : streams) if (!valid(stream.second)) return {};
    for (const auto &recording : recordings) if (!valid(recording.vEncoder)) return {};

    if (streams.size() == 2 && recordings.size() == 1) {
        const double first = score(recordings[0].vEncoder, streams[0].second);
        const double second = score(recordings[0].vEncoder, streams[1].second);
        if (std::max(first, second) < 90.0) return {};
        return {mapping(second > first ? streams[1].first : streams[0].first, 0)};
    }
    if (streams.size() == 1 && recordings.size() == 2) {
        const double first = score(recordings[0].vEncoder, streams[0].second);
        const double second = score(recordings[1].vEncoder, streams[0].second);
        if (std::max(first, second) < 90.0) return {};
        return {mapping(streams[0].first, second > first ? 1 : 0)};
    }
    if (streams.size() == 2 && recordings.size() == 2) {
        const double direct = score(recordings[0].vEncoder, streams[0].second) +
                              score(recordings[1].vEncoder, streams[1].second);
        const double crossed = score(recordings[0].vEncoder, streams[1].second) +
                               score(recordings[1].vEncoder, streams[0].second);
        if (std::max(direct, crossed) < 180.0) return {};
        if (direct >= crossed)
            return {mapping(streams[0].first, 0), mapping(streams[1].first, 1)};
        return {mapping(streams[1].first, 0), mapping(streams[0].first, 1)};
    }
    return {};
}

void MediaGapFillManager::onManager(const std::string &device_id,
                                    const std::string &username,
                                    const std::string &password,
                                    const OnvifControl::Ptr &onvif,
                                    const SdCardSyncConfig &config,
                                    bool ready) {
    const std::string source_type = "sdcard:" + device_id;
    if (!onvif || !onvif->getSDCardInfo().isSDSupport() ||
        !config.sdCardSyncEnabled || !config.sdCardSyncAutoSyncEnabled || !ready) {
        checkpointProgress();
        SourceProvider provider;
        std::vector<MediaGapFillRequest> active_requests;
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto source = _providers.find(source_type);
            if (source != _providers.end()) provider = source->second;
            for (auto it = _active.begin(); it != _active.end();) {
                if (it->second.request.device_id != device_id) {
                    ++it;
                    continue;
                }
                persistTask(it->second.request, it->second.processed_up_to,
                            it->second.retry_count, GapFillState::Pending,
                            "SD-card gap fill disabled");
                active_requests.push_back(it->second.request);
                it = _active.erase(it);
            }
            for (auto it = _pending.begin(); it != _pending.end();) {
                if (it->request.device_id != device_id) {
                    ++it;
                    continue;
                }
                persistTask(it->request, it->processed_up_to, it->retry_count,
                            GapFillState::Pending, "SD-card gap fill disabled");
                it = _pending.erase(it);
            }
            _providers.erase(source_type);
            _device_source_types.erase(device_id);
        }
        if (provider.stop) {
            for (const auto &request : active_requests) provider.stop(request);
        }
        schedule();
        return;
    }

    const int min_gap = config.sdCardSyncMinSegmentGapSec;
    const int retry_limit = config.sdCardSyncRetryCount;
    SourceProvider provider = makeRtspPlaybackProvider(
        [onvif, device_id, username, password, min_gap, retry_limit]
        (const LocalResourceStatusGap &gap) {
            MediaGapDiscoveryResult result;
            auto &requests = result.requests;
            if (gap.resource_id != device_id || gap.reconnect_time <= gap.disconnect_time ||
                gap.reconnect_time - gap.disconnect_time < static_cast<uint64_t>(std::max(0, min_gap))) {
                return result;
            }

            const auto segments = onvif->findVideoSegments(
                static_cast<time_t>(gap.disconnect_time),
                static_cast<time_t>(gap.reconnect_time), min_gap);
            const auto recordings = onvif->getRecordingInformation();
            const auto stream_ids = getConfiguredStreamMap(device_id);
            const auto stats = getCameraStats(device_id);
            if (!stats) {
                result.success = false;
                result.error = "camera statistics unavailable";
                return result;
            }
            const auto schedule = RecordScheduler::parseRecordScheduleStr(
                stats->getParams().option.recordSchedules);
            const std::string session_id = "sess-" + std::to_string(gap.disconnect_time);
            auto mappings = MediaGapFillManager::mapRecordingsToStreams(
                stats->getStreamVideoEncoderConfigMap(), recordings);
            std::map<std::string, const RecordingInformation *> recordings_by_stream;
            for (const auto &mapping : mappings) {
                if (mapping.recording_index < recordings.size())
                    recordings_by_stream[mapping.stream_id] = &recordings[mapping.recording_index];
            }
            if (recordings_by_stream.empty()) {
                for (const auto &recording : recordings) {
                    auto configured = stream_ids.find(recording.streamType);
                    if (configured != stream_ids.end() && !configured->second.empty())
                        recordings_by_stream[configured->second] = &recording;
                }
            }

            for (const auto &entry : segments) {
                auto stream_it = stream_ids.find(entry.first);
                if (stream_it == stream_ids.end() || stream_it->second.empty()) continue;
                auto rec_it = recordings_by_stream.find(stream_it->second);
                if (rec_it == recordings_by_stream.end() || !rec_it->second ||
                    rec_it->second->uri.empty()) continue;
                const auto &recording = *rec_it->second;
                if (recording.earliestRecording <= 0 ||
                    static_cast<uint64_t>(recording.earliestRecording) > gap.reconnect_time ||
                    (recording.latestRecording > 0 &&
                     static_cast<uint64_t>(recording.latestRecording) < gap.disconnect_time))
                    continue;

                std::vector<VideoSegment> enabled_segments;
                for (const auto &segment : entry.second) {
                    auto ranges = splitEnabledRecordRanges(segment, schedule);
                    enabled_segments.insert(enabled_segments.end(), ranges.begin(), ranges.end());
                }
                std::sort(enabled_segments.begin(), enabled_segments.end(),
                          [](const VideoSegment &lhs, const VideoSegment &rhs) {
                              return lhs.start_time < rhs.start_time ||
                                     (lhs.start_time == rhs.start_time && lhs.end_time < rhs.end_time);
                          });

                int32_t segment_index = 0;
                for (const auto &segment : enabled_segments) {
                    const uint64_t begin = std::max<uint64_t>(
                        std::max<uint64_t>(gap.disconnect_time,
                                           static_cast<uint64_t>(segment.start_time)),
                        static_cast<uint64_t>(recording.earliestRecording));
                    uint64_t end = std::min<uint64_t>(gap.reconnect_time,
                                                       static_cast<uint64_t>(segment.end_time));
                    if (recording.latestRecording > 0)
                        end = std::min(end, static_cast<uint64_t>(recording.latestRecording));
                    if (begin >= end) continue;
                    MediaGapFillRequest request;
                    request.device_id = device_id;
                    request.stream_id = stream_it->second;
                    request.gap_id = gap.gap_id;
                    request.session_id = session_id;
                    request.origin_url = recording.uri;
                    request.username = username;
                    request.password = password;
                    request.source_begin_time = static_cast<uint64_t>(recording.earliestRecording);
                    request.disconnect_time = gap.disconnect_time;
                    request.reconnect_time = gap.reconnect_time;
                    request.begin_time = begin;
                    request.end_time = end;
                    request.segment_index = segment_index++;
                    request.retry_limit = retry_limit;
                    requests.emplace_back(std::move(request));
                }
            }
            return result;
        });
    provider.prepare = [username, password](MediaGapFillRequest &request) {
        request.username = username;
        request.password = password;
    };
    registerSource(source_type, std::move(provider));
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        _device_source_types[device_id] = source_type;
    }
    WorkThreadPool::Instance().getExecutor()->async([device_id, source_type]() {
        MediaGapFillManager::Instance().fillDevice(device_id, source_type);
    });
}

bool MediaGapFillManager::registerSource(const std::string &source_type,
                                         SourceProvider provider) {
    if (source_type.empty() || !provider.discover || !provider.start) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _providers[source_type] = std::move(provider);
    return true;
}

void MediaGapFillManager::unregisterSource(const std::string &source_type) {
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _providers.erase(source_type);
}

size_t MediaGapFillManager::fillDevice(const std::string &device_id,
                                       const std::string &source_type) {
    SourceProvider provider;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        auto it = _providers.find(source_type);
        if (it == _providers.end()) {
            return 0;
        }
        provider = it->second;
    }

    size_t accepted = 0;
    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    LocalResourceStatusGapImp gap_repository;
    MediaGapFillSessionImpl session_repository;
    MediaGapFillTaskImpl task_repository;
    for (const auto &gap : gap_repository.findByResourceIdAndTimeRange(device_id)) {
        if (gap.state != 1 || gap.disconnect_time == 0 ||
            gap.reconnect_time <= gap.disconnect_time) {
            continue;
        }
        MediaGapFillSession session;
        auto saved_sessions = session_repository.find(device_id, gap.gap_id);
        if (saved_sessions.empty()) {
            session.device_id = device_id;
            session.gap_id = gap.gap_id;
            session.session_id = "sess-" + std::to_string(gap.disconnect_time);
            session.source_type = source_type;
            session.disconnect_time = gap.disconnect_time;
            session.reconnect_time = gap.reconnect_time;
            session.updated_time = now;
            if (!session_repository.addOrUpdate(session)) continue;
        } else {
            session = saved_sessions.front();
        }

        // An explicit management pass resumes a session stopped through the API.
        if (session.control_state == MediaGapControlState::Paused) {
            session.control_state = MediaGapControlState::Active;
            session.updated_time = now;
            session_repository.addOrUpdate(session);
        }
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            _paused_sessions.erase({device_id, session.session_id});
            const std::string prefix = sessionKey(device_id, session.session_id) + '\n';
            for (auto it = _paused_streams.begin(); it != _paused_streams.end();) {
                if (it->first.compare(0, prefix.size(), prefix) == 0)
                    it = _paused_streams.erase(it);
                else
                    ++it;
            }
        }

        if (session.discovery_state == MediaGapDiscoveryState::NoMedia ||
            session.discovery_state == MediaGapDiscoveryState::Completed) {
            continue;
        }
        if (session.discovery_state == MediaGapDiscoveryState::Ready) {
            for (const auto &task : task_repository.findByDeviceAndGap(device_id, gap.gap_id)) {
                if (task.state == GapFillState::Success || task.state == GapFillState::Failed) continue;
                auto request = requestFromTask(task);
                if (provider.prepare) provider.prepare(request);
                if (enqueue(std::move(request))) ++accepted;
            }
            continue;
        }
        if (session.discovery_state == MediaGapDiscoveryState::Discovering &&
            now < session.updated_time + 60) {
            continue;
        }
        if (session.discovery_state == MediaGapDiscoveryState::Failed) {
            const int exponent = std::min(4, std::max(0, session.discovery_attempts - 1));
            const uint64_t backoff = static_cast<uint64_t>(30 << exponent);
            if (now < session.updated_time + backoff) continue;
        }

        if (!session_repository.claimDiscovery(session, now)) continue;

        MediaGapDiscoveryResult discovery;
        try {
            discovery = provider.discover(gap);
        } catch (const std::exception &ex) {
            discovery.success = false;
            discovery.error = ex.what();
        } catch (...) {
            discovery.success = false;
            discovery.error = "unknown discovery error";
        }
        if (!discovery.success) {
            session.discovery_state = MediaGapDiscoveryState::Failed;
            session.last_error = discovery.error.empty() ? "source discovery failed" : discovery.error;
            session.updated_time = static_cast<uint64_t>(time(nullptr));
            session_repository.addOrUpdate(session);
            continue;
        }

        std::vector<MediaGapFillRequest> requests;
        for (auto request : discovery.requests) {
            request.source_type = source_type;
            request.device_id = request.device_id.empty() ? device_id : request.device_id;
            request.gap_id = request.gap_id.empty() ? gap.gap_id : request.gap_id;
            request.session_id = request.session_id.empty()
                ? "sess-" + std::to_string(gap.disconnect_time) : request.session_id;
            request.disconnect_time = request.disconnect_time == 0
                ? gap.disconnect_time : request.disconnect_time;
            request.reconnect_time = request.reconnect_time == 0
                ? gap.reconnect_time : request.reconnect_time;
            request.begin_time = std::max(request.begin_time, gap.disconnect_time);
            request.end_time = std::min(request.end_time, gap.reconnect_time);
            if (!validRequest(request)) continue;
            if (request.id.empty()) {
                request.id = request.device_id + '/' + request.stream_id + '/' +
                             request.session_id + '/' + std::to_string(request.begin_time);
            }
            requests.emplace_back(std::move(request));
        }

        // IMMEDIATE: addOrUpdateWithTxn reads before it writes (see DbStorage::execTxn).
        auto transaction = session_repository.getExecutor()->execTxn(true);
        bool committed = true;
        for (const auto &request : requests) {
            auto task = makeTask(request, request.begin_time, 0, GapFillState::Pending, "");
            auto existing = task_repository.findById(request.id);
            if (!existing.empty()) {
                task.processed_up_to = existing.front().processed_up_to;
                task.retry_count = existing.front().retry_count;
                task.state = existing.front().state;
                task.processing_start_time = existing.front().processing_start_time;
                task.processing_end_time = existing.front().processing_end_time;
                task.next_retry_time = existing.front().next_retry_time;
                task.last_attempt_time = existing.front().last_attempt_time;
            }
            committed = task_repository.addOrUpdateWithTxn(task, transaction) && committed;
        }
        session.discovery_state = requests.empty()
            ? MediaGapDiscoveryState::NoMedia : MediaGapDiscoveryState::Ready;
        session.updated_time = static_cast<uint64_t>(time(nullptr));
        committed = session_repository.addOrUpdateWithTxn(session, transaction) && committed;
        if (!committed) {
            transaction->rollback();
            continue;
        }
        transaction->commit();

        for (auto request : requests) {
            if (provider.prepare) provider.prepare(request);
            if (enqueue(std::move(request))) ++accepted;
        }
    }
    return accepted;
}

bool MediaGapFillManager::enqueue(MediaGapFillRequest request) {
    if (!validRequest(request)) {
        return false;
    }
    if (request.id.empty()) {
        request.id = request.device_id + '/' + request.stream_id + '/' +
                     request.session_id + '/' + std::to_string(request.begin_time);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        if (_providers.find(request.source_type) == _providers.end() ||
            _active.find(request.id) != _active.end()) {
            return false;
        }
        auto duplicate = std::find_if(_pending.begin(), _pending.end(), [&](const ConnectionState &state) {
            return state.request.id == request.id;
        });
        if (duplicate != _pending.end()) {
            return false;
        }
        ConnectionState state;
        MediaGapFillTaskImpl repository;
        auto saved_tasks = repository.findById(request.id);
        if (!saved_tasks.empty()) {
            const auto &saved = saved_tasks.front();
            if (saved.state == GapFillState::Success ||
                saved.processed_up_to >= request.end_time) {
                return false;
            }
            if (saved.state == GapFillState::Failed) {
                return false;
            }
            state.processed_up_to = std::max(request.begin_time, saved.processed_up_to);
            state.retry_count = saved.retry_count;
            state.next_retry_time = saved.next_retry_time;
        } else {
            state.processed_up_to = request.begin_time;
        }
        state.request = std::move(request);
        if (!persistTask(state.request, state.processed_up_to, state.retry_count,
                         GapFillState::Pending, "", state.next_retry_time)) {
            return false;
        }
        _pending.emplace_back(std::move(state));
    }
    schedule();
    return true;
}

void MediaGapFillManager::checkpointProgress() {
    struct Probe {
        std::string id;
        uint64_t generation = 0;
        MediaGapFillRequest request;
        ProgressCallback progress;
        StopCallback stop;
    };
    std::vector<Probe> probes;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        for (const auto &item : _active) {
            auto provider = _providers.find(item.second.request.source_type);
            if (provider == _providers.end() || !provider->second.progress) continue;
            Probe probe;
            probe.id = item.first;
            probe.generation = item.second.generation;
            probe.request = item.second.request;
            probe.request.begin_time = item.second.attempt_begin;
            probe.progress = provider->second.progress;
            probe.stop = provider->second.stop;
            probes.emplace_back(std::move(probe));
        }
    }

    for (const auto &probe : probes) {
        uint64_t processed = probe.request.begin_time;
        if (!probe.progress(probe.request, processed)) continue;
        bool completed = false;
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto active = _active.find(probe.id);
            if (active == _active.end() || active->second.generation != probe.generation) continue;
            processed = std::min(active->second.request.end_time,
                                 std::max(active->second.processed_up_to, processed));
            if (processed > active->second.processed_up_to) {
                active->second.processed_up_to = processed;
                persistTask(active->second.request, processed, active->second.retry_count,
                            GapFillState::Processing);
            }
            completed = processed >= active->second.request.end_time;
        }
        if (completed) {
            if (probe.stop) probe.stop(probe.request);
            onConnectionClosed(probe.id, probe.generation, true, processed,
                               "source progress completed");
        }
    }
}

bool MediaGapFillManager::stop(const std::string &connection_id) {
    checkpointProgress();
    SourceProvider provider;
    MediaGapFillRequest request;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        auto active = _active.find(connection_id);
        if (active == _active.end()) {
            auto pending = std::find_if(_pending.begin(), _pending.end(), [&](const ConnectionState &state) {
                return state.request.id == connection_id;
            });
            if (pending == _pending.end()) {
                return false;
            }
            persistTask(pending->request, pending->processed_up_to, pending->retry_count,
                        GapFillState::Pending, "stopped by request");
            _pending.erase(pending);
            return true;
        }
        auto source = _providers.find(active->second.request.source_type);
        if (source != _providers.end()) {
            provider = source->second;
        }
        request = active->second.request;
        _paused_streams[streamKey(request)] = true;
        persistTask(active->second.request, active->second.processed_up_to,
                active->second.retry_count, GapFillState::Pending,
                "stopped by request");
        _active.erase(active);
    }
    if (provider.stop) {
        provider.stop(request);
    }
    // The next explicit fillDevice management pass resumes a stopped stream.
    return true;
}

void MediaGapFillManager::setMaxConnections(size_t max_connections) {
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        _max_connections = std::max<size_t>(1, max_connections);
    }
    schedule();
}

void MediaGapFillManager::setDefaultSourceType(const std::string &source_type) {
    if (source_type.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _default_source_type = source_type;
}

Json::Value MediaGapFillManager::sessionToJson(const std::vector<MediaGapFillTask> &tasks) {
    Json::Value result(Json::objectValue);
    if (tasks.empty()) return result;

    bool processing = false;
    bool pending = false;
    bool failed = false;
    uint64_t processing_start = 0;
    uint64_t processing_end = 0;
    std::map<std::string, std::vector<MediaGapFillTask>> streams;
    for (const auto &task : tasks) {
        processing = processing || task.state == GapFillState::Processing;
        pending = pending || task.state == GapFillState::Pending;
        failed = failed || task.state == GapFillState::Failed;
        if (task.processing_start_time != 0 &&
            (processing_start == 0 || task.processing_start_time < processing_start))
            processing_start = task.processing_start_time;
        processing_end = std::max(processing_end, task.processing_end_time);
        streams[task.stream_id].push_back(task);
    }

    const std::string session_state = processing ? GapFillState::Processing
        : pending ? GapFillState::Pending
        : failed ? GapFillState::Failed : GapFillState::Success;
    if (processing || pending) processing_end = 0;
    result["session_id"] = tasks.front().session_id;
    result["disconnect_time"] = Json::UInt64(tasks.front().disconnect_time);
    result["reconnect_time"] = Json::UInt64(tasks.front().reconnect_time);
    result["processing_start_time"] = Json::UInt64(processing_start);
    result["processing_end_time"] = Json::UInt64(processing_end);
    result["session_state"] = session_state;
    double session_progress = 0.0;
    for (const auto &stream : streams) {
        uint64_t stream_duration = 0;
        uint64_t stream_processed = 0;
        for (const auto &task : stream.second) {
            const uint64_t duration = task.end_time > task.begin_time
                ? task.end_time - task.begin_time : 0;
            stream_duration += duration;
            const uint64_t capped = task.state == GapFillState::Success
                ? task.end_time
                : std::min(task.end_time, std::max(task.begin_time, task.processed_up_to));
            stream_processed += capped - task.begin_time;
        }
        if (stream_duration != 0)
            session_progress += 100.0 * static_cast<double>(stream_processed) /
                                static_cast<double>(stream_duration);
    }
    result["progress_percent"] = streams.empty()
        ? 0.0 : session_progress / static_cast<double>(streams.size());

    Json::Value handles(Json::arrayValue);
    for (auto &stream : streams) {
        auto &segments = stream.second;
        std::sort(segments.begin(), segments.end(), [](const MediaGapFillTask &lhs,
                                                        const MediaGapFillTask &rhs) {
            return lhs.segment_index < rhs.segment_index ||
                   (lhs.segment_index == rhs.segment_index && lhs.begin_time < rhs.begin_time);
        });
        bool stream_processing = false;
        bool stream_pending = false;
        bool stream_failed = false;
        Json::Value segment_values(Json::arrayValue);
        for (const auto &task : segments) {
            stream_processing = stream_processing || task.state == GapFillState::Processing;
            stream_pending = stream_pending || task.state == GapFillState::Pending;
            stream_failed = stream_failed || task.state == GapFillState::Failed;
            Json::Value segment(Json::objectValue);
            segment["segment_index"] = task.segment_index;
            segment["start_time"] = Json::UInt64(task.begin_time);
            segment["end_time"] = Json::UInt64(task.end_time);
            segment["state"] = task.state;
            segment["processed_up_to"] = Json::UInt64(task.processed_up_to);
            segment_values.append(std::move(segment));
        }
        Json::Value handle(Json::objectValue);
        handle["stream_id"] = stream.first;
        handle["handle_state"] = stream_processing ? GapFillState::Processing
            : stream_pending ? GapFillState::Pending
            : stream_failed ? GapFillState::Failed : GapFillState::Success;
        handle["segments"] = std::move(segment_values);
        handles.append(std::move(handle));
    }
    result["stream_handle_infos"] = std::move(handles);
    return result;
}

bool MediaGapFillManager::resetFailedSession(const std::string &device_id,
                                             const std::string &session_id) {
    MediaGapFillTaskImpl repository;
    bool changed = false;
    auto tasks = repository.findByDeviceAndSession(device_id, session_id);
    if (tasks.empty()) {
        for (const auto &task : repository.findByDevice(device_id))
            if (task.gap_id == session_id) tasks.push_back(task);
    }
    if (tasks.empty() || sessionToJson(tasks)["session_state"].asString() != GapFillState::Failed)
        return false;
    for (auto task : tasks) {
        if (task.state != GapFillState::Failed) continue;
        task.state = GapFillState::Pending;
        task.retry_count = 0;
        task.next_retry_time = 0;
        task.last_error.clear();
        changed = repository.addOrUpdate(task) || changed;
    }
    if (changed) {
        MediaGapFillSessionImpl sessions;
        auto saved = sessions.find(device_id, tasks.front().gap_id);
        if (!saved.empty()) {
            auto session = saved.front();
            session.discovery_state = MediaGapDiscoveryState::Ready;
            session.control_state = MediaGapControlState::Active;
            session.last_error.clear();
            session.updated_time = static_cast<uint64_t>(time(nullptr));
            sessions.addOrUpdate(session);
        }
        std::string source;
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto it = _device_source_types.find(device_id);
            source = it == _device_source_types.end() ? _default_source_type : it->second;
        }
        WorkThreadPool::Instance().getExecutor()->async([device_id, source]() {
            MediaGapFillManager::Instance().fillDevice(device_id, source);
        });
    }
    return changed;
}

bool MediaGapFillManager::resetFailedSegment(const std::string &device_id,
                                             const std::string &session_id,
                                             const std::string &stream_id,
                                             int32_t segment_index) {
    MediaGapFillTaskImpl repository;
    MediaGapFillTask task;
    bool found = false;
    auto candidates = repository.findByDeviceAndSession(device_id, session_id);
    if (candidates.empty()) {
        for (const auto &candidate : repository.findByDevice(device_id))
            if (candidate.gap_id == session_id) candidates.push_back(candidate);
    }
    for (const auto &candidate : candidates) {
        if (candidate.stream_id == stream_id &&
            candidate.segment_index == segment_index) {
            task = candidate;
            found = true;
            break;
        }
    }
    if (!found) return false;
    if (task.state != GapFillState::Failed) return false;
    task.state = GapFillState::Pending;
    task.retry_count = 0;
    task.next_retry_time = 0;
    task.last_error.clear();
    if (!repository.addOrUpdate(task)) return false;
    MediaGapFillSessionImpl sessions;
    auto saved = sessions.find(device_id, task.gap_id);
    if (!saved.empty()) {
        auto session = saved.front();
        session.discovery_state = MediaGapDiscoveryState::Ready;
        session.control_state = MediaGapControlState::Active;
        session.updated_time = static_cast<uint64_t>(time(nullptr));
        sessions.addOrUpdate(session);
    }
    std::string source;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        auto it = _device_source_types.find(device_id);
        source = it == _device_source_types.end() ? _default_source_type : it->second;
    }
    WorkThreadPool::Instance().getExecutor()->async([device_id, source]() {
        MediaGapFillManager::Instance().fillDevice(device_id, source);
    });
    return true;
}

bool MediaGapFillManager::stopSession(const std::string &device_id) {
    std::string session_id;
    std::string gap_id;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        uint64_t earliest_disconnect = UINT64_MAX;
        for (const auto &item : _active) {
            const auto &request = item.second.request;
            if (request.device_id == device_id && request.disconnect_time < earliest_disconnect) {
                earliest_disconnect = request.disconnect_time;
                session_id = request.session_id;
                gap_id = request.gap_id;
            }
        }
        if (session_id.empty()) return false;
        _paused_sessions[{device_id, session_id}] = true;
    }

    MediaGapFillSessionImpl sessions;
    auto saved = sessions.find(device_id, gap_id);
    if (!saved.empty()) {
        auto session = saved.front();
        session.control_state = MediaGapControlState::Paused;
        session.updated_time = static_cast<uint64_t>(time(nullptr));
        sessions.addOrUpdate(session);
    }

    checkpointProgress();
    std::vector<std::pair<SourceProvider, MediaGapFillRequest>> stops;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        for (auto it = _active.begin(); it != _active.end();) {
            auto &state = it->second;
            if (state.request.device_id != device_id || state.request.session_id != session_id) {
                ++it;
                continue;
            }
            persistTask(state.request, state.processed_up_to, state.retry_count,
                        GapFillState::Pending, "session stopped by request");
            auto provider = _providers.find(state.request.source_type);
            if (provider != _providers.end()) stops.emplace_back(provider->second, state.request);
            it = _active.erase(it);
        }
        for (auto it = _pending.begin(); it != _pending.end();) {
            if (it->request.device_id == device_id && it->request.session_id == session_id) {
                persistTask(it->request, it->processed_up_to, it->retry_count,
                            GapFillState::Pending, "session stopped by request");
                it = _pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto &entry : stops)
        if (entry.first.stop) entry.first.stop(entry.second);
    schedule();
    return !stops.empty();
}

bool MediaGapFillManager::stopSegment(const std::string &device_id,
                                      const std::string &stream_id,
                                      int32_t segment_index) {
    std::string connection_id;
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        for (const auto &item : _active) {
            const auto &request = item.second.request;
            if (request.device_id == device_id && request.stream_id == stream_id &&
                request.segment_index == segment_index) {
                connection_id = item.first;
                break;
            }
        }
    }
    return !connection_id.empty() && stop(connection_id);
}

void MediaGapFillManager::onDeviceStateChanged(const std::string &device_id, bool connected,
                                               const std::string &source_type) {
    if (!connected) {
        // The status-gap recorder owns the disconnect interval. Keep unfinished
        // playback tasks so reconnect can resume their remaining ranges.
        return;
    }
    std::string source = source_type;
    if (source.empty()) {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        auto it = _device_source_types.find(device_id);
        source = it == _device_source_types.end() ? _default_source_type : it->second;
    }
    WorkThreadPool::Instance().getExecutor()->async([device_id, source]() {
        MediaGapFillManager::Instance().fillDevice(device_id, source);
    });
}

Json::Value MediaGapFillManager::makeRemainingSegmentsJson(const std::string &device_id) {
    Json::Value result(Json::arrayValue);
    MediaGapFillTaskImpl repository;
    std::map<std::string, std::vector<MediaGapFillTask>> sessions;
    std::vector<std::pair<int64_t, Json::Value>> summaries;
    for (const auto &task : repository.findByDevice(device_id))
        sessions[task.session_id].push_back(task);
    for (const auto &entry : sessions) {
        auto session = sessionToJson(entry.second);
        const auto state = session["session_state"].asString();
        if (state != GapFillState::Pending && state != GapFillState::Failed) continue;
        Json::Value summary(Json::objectValue);
        summary["session_id"] = session["session_id"];
        summary["disconnect_time"] = session["disconnect_time"];
        summary["reconnect_time"] = session["reconnect_time"];
        summaries.emplace_back(session["disconnect_time"].asInt64(), std::move(summary));
    }
    std::sort(summaries.begin(), summaries.end(),
              [](const std::pair<int64_t, Json::Value> &lhs,
                 const std::pair<int64_t, Json::Value> &rhs) { return lhs.first < rhs.first; });
    for (auto &summary : summaries) result.append(std::move(summary.second));
    return result;
}

Json::Value MediaGapFillManager::makeDisconnectSessionJson(const std::string &device_id,
                                                           const std::string &session_id) {
    MediaGapFillTaskImpl repository;
    auto tasks = repository.findByDeviceAndSession(device_id, session_id);
    if (tasks.empty()) {
        for (const auto &task : repository.findByDevice(device_id)) {
            if (task.gap_id == session_id) tasks.push_back(task);
        }
    }
    return sessionToJson(tasks);
}

Json::Value MediaGapFillManager::makeDisconnectSessionsJson(const std::string &device_id) {
    Json::Value result(Json::arrayValue);
    std::map<std::string, std::vector<MediaGapFillTask>> sessions;
    MediaGapFillTaskImpl repository;
    for (const auto &task : repository.findByDevice(device_id))
        sessions[task.session_id].push_back(task);
    std::vector<Json::Value> ordered;
    for (auto &entry : sessions) ordered.push_back(sessionToJson(entry.second));
    std::sort(ordered.begin(), ordered.end(), [](const Json::Value &lhs, const Json::Value &rhs) {
        return lhs["disconnect_time"].asUInt64() < rhs["disconnect_time"].asUInt64();
    });
    for (auto &session : ordered) result.append(std::move(session));
    return result;
}

MediaGapFillManager::SourceProvider MediaGapFillManager::makeRtspPlaybackProvider(
    DiscoverCallback discover) {
    SourceProvider provider;
    provider.discover = std::move(discover);
    provider.start = [](const MediaGapFillRequest &request, const ClosedCallback &closed) {
        const auto play_url = appendRtspRange(request);
        if (play_url.empty()) {
            return false;
        }

        ProtocolOption protocol;
        protocol.enable_mp4 = true;
        const std::string params = "start_time_ms=" + std::to_string(request.begin_time * 1000) +
                                   "&continuous=1";
        MediaTuple tuple(DEFAULT_VHOST, request.device_id, relayStream(request.stream_id), params);
        addReplayStreamProxy(tuple, protocol,
            [request, play_url, tuple, closed](const std::string &err, const PlayerProxy::Ptr &player) {
                if (!err.empty() || !player) {
                    closed(false, request.begin_time, err.empty() ? "create RTSP player failed" : err);
                    return;
                }
                std::weak_ptr<PlayerProxy> weak_player = player;
                player->setOnClose([request, weak_player, tuple, closed](const SockException &ex) {
                    auto strong_player = weak_player.lock();
                    if (!strong_player) {
                        closed(false, request.begin_time, ex.what());
                        return;
                    }
                    const auto progress = MediaGapFillManager::calculatePlaybackProgress(
                        request.begin_time, request.end_time,
                        static_cast<double>(strong_player->MediaPlayer::getProgress()));
                    delReplayStreamProxy(tuple.shortUrl());
                    closed(progress.completed, progress.processed_up_to, ex.what());
                });
                player->play(play_url);
            }, 0);
        return true;
    };
    provider.stop = [](const MediaGapFillRequest &request) {
        MediaTuple tuple(DEFAULT_VHOST, request.device_id, relayStream(request.stream_id), "");
        delReplayStreamProxy(tuple.shortUrl());
    };
    provider.progress = [](const MediaGapFillRequest &request, uint64_t &processed_up_to) {
        MediaTuple tuple(DEFAULT_VHOST, request.device_id, relayStream(request.stream_id), "");
        auto player = getReplayStreamProxy(tuple.shortUrl());
        if (!player || request.begin_time >= request.end_time) return false;
        const auto progress = MediaGapFillManager::calculatePlaybackProgress(
            request.begin_time, request.end_time,
            static_cast<double>(player->MediaPlayer::getProgress()));
        processed_up_to = progress.processed_up_to;
        return true;
    };
    return provider;
}

void MediaGapFillManager::armRetryTimer(uint64_t wakeup_time) {
    if (wakeup_time == 0) return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (_retry_wakeup_time != 0 && _retry_wakeup_time <= wakeup_time) return;
    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    const float delay = static_cast<float>(std::max<uint64_t>(1, wakeup_time > now
        ? wakeup_time - now : 1));
    _retry_wakeup_time = wakeup_time;
    _retry_timer = std::make_shared<Timer>(delay, [this]() {
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            _retry_wakeup_time = 0;
        }
        schedule();
        return false;
    }, EventPollerPool::Instance().getPoller());
}

void MediaGapFillManager::schedule() {
    while (true) {
        ConnectionState state;
        SourceProvider provider;
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            if (_active.size() >= _max_connections || _pending.empty()) {
                return;
            }
            const uint64_t now = static_cast<uint64_t>(time(nullptr));
            auto runnable = std::find_if(_pending.begin(), _pending.end(), [&](const ConnectionState &candidate) {
                if (candidate.next_retry_time > now) return false;
                for (const auto &paused : _paused_sessions)
                    if (paused.first.first == candidate.request.device_id) return false;
                if (_paused_streams.count(streamKey(candidate.request))) return false;

                bool device_active = false;
                for (const auto &active : _active) {
                    if (active.second.request.device_id != candidate.request.device_id) continue;
                    device_active = true;
                    if (active.second.request.session_id != candidate.request.session_id ||
                        active.second.request.stream_id == candidate.request.stream_id)
                        return false;
                }
                if (device_active) return true;

                for (const auto &pending : _pending) {
                    if (pending.request.device_id != candidate.request.device_id) continue;
                    if (pending.request.disconnect_time < candidate.request.disconnect_time ||
                        (pending.request.disconnect_time == candidate.request.disconnect_time &&
                         pending.request.session_id < candidate.request.session_id))
                        return false;
                }
                return true;
            });
            if (runnable == _pending.end()) {
                uint64_t earliest_retry = UINT64_MAX;
                for (const auto &pending : _pending) {
                    if (pending.next_retry_time > now)
                        earliest_retry = std::min(earliest_retry, pending.next_retry_time);
                }
                if (earliest_retry != UINT64_MAX) armRetryTimer(earliest_retry);
                return;
            }
            state = std::move(*runnable);
            _pending.erase(runnable);
            auto source = _providers.find(state.request.source_type);
            if (source == _providers.end()) {
                continue;
            }
            provider = source->second;
            state.attempt_begin = std::max(state.request.begin_time, state.processed_up_to);
            state.generation = ++_next_generation;
            state.next_retry_time = 0;
            _active[state.request.id] = state;
            persistTask(state.request, state.processed_up_to, state.retry_count,
                        GapFillState::Processing);
        }

        const auto connection_id = state.request.id;
        const auto generation = state.generation;
        auto closed = [this, connection_id, generation](bool completed, uint64_t processed_up_to,
                                            const std::string &reason) {
            onConnectionClosed(connection_id, generation, completed, processed_up_to, reason);
        };
        auto start_request = state.request;
        start_request.begin_time = state.attempt_begin;
        if (!provider.start(start_request, closed)) {
            onConnectionClosed(connection_id, generation, false, state.processed_up_to,
                               "source rejected connection");
        }
    }
}

void MediaGapFillManager::onConnectionClosed(const std::string &connection_id,
                                             uint64_t generation,
                                             bool completed,
                                             uint64_t processed_up_to,
                                             const std::string &reason) {
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        auto it = _active.find(connection_id);
        if (it == _active.end() || it->second.generation != generation) {
            return;
        }
        auto state = std::move(it->second);
        _active.erase(it);
        const auto session_request = state.request;
        state.processed_up_to = std::max(state.processed_up_to, processed_up_to);
        if (completed || state.processed_up_to >= state.request.end_time) {
            state.processed_up_to = state.request.end_time;
            persistTask(state.request, state.processed_up_to, state.retry_count,
                        GapFillState::Success);
        } else if (state.processed_up_to < state.request.end_time &&
            state.retry_count < std::max(0, state.request.retry_limit)) {
            ++state.retry_count;
            state.next_retry_time = static_cast<uint64_t>(time(nullptr)) +
                                    calculateRetryDelay(state.retry_count);
            persistTask(state.request, state.processed_up_to, state.retry_count,
                        GapFillState::Pending, reason, state.next_retry_time);
            _pending.emplace_back(std::move(state));
        } else {
            persistTask(state.request, state.processed_up_to, state.retry_count,
                        GapFillState::Failed, reason);
            WarnL << "Gap-fill connection failed: id=" << connection_id
                  << " reason=" << reason;
        }
        updateSessionCompletion(session_request);
    }
    schedule();
}

void MediaGapFillManager::updateSessionCompletion(const MediaGapFillRequest &request) {
    MediaGapFillTaskImpl tasks;
    const auto rows = tasks.findByDeviceAndGap(request.device_id, request.gap_id);
    if (rows.empty()) return;
    const bool completed = std::all_of(rows.begin(), rows.end(), [](const MediaGapFillTask &task) {
        return task.state == GapFillState::Success || task.processed_up_to >= task.end_time;
    });
    MediaGapFillSessionImpl sessions;
    auto saved = sessions.find(request.device_id, request.gap_id);
    if (saved.empty()) return;
    auto session = saved.front();
    session.discovery_state = completed
        ? MediaGapDiscoveryState::Completed : MediaGapDiscoveryState::Ready;
    session.updated_time = static_cast<uint64_t>(time(nullptr));
    sessions.addOrUpdate(session);
}

bool MediaGapFillManager::validRequest(const MediaGapFillRequest &request) {
    return !request.source_type.empty() && !request.device_id.empty() &&
           !request.stream_id.empty() && !request.origin_url.empty() &&
           request.begin_time < request.end_time;
}

} // namespace managerkit
