#include <chrono>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Local/MediaGapFillManager.h"
#include "Extension/Resource.h"
#include "Storage/DbStorage.h"
#include "Storage/MediaGapFillTask.h"
#include "Storage/MediaGapFillSession.h"
#include "Storage/LocalResourceStatusGap.h"
#include "Util/mini.h"
#include "server/Manager.h"
#include "server/WebApi.h"

using namespace managerkit;
using namespace toolkit;

// s3managerkit is also linked into MediaServer, where these symbols live.
// The compatibility test only exercises persistence/JSON projection, so no
// playback proxy is expected to be created; lightweight stubs keep the unit
// test independent from the server executable.

void addStreamProxy(const mediakit::MediaTuple &, const mediakit::ProtocolOption &,
                    const std::function<void(const std::string &, const mediakit::PlayerProxy::Ptr &)> &) {}
void delStreamProxy(const mediakit::MediaTuple &) {}
void addReplayStreamProxy(const mediakit::MediaTuple &, const mediakit::ProtocolOption &,
                          const std::function<void(const std::string &, const mediakit::PlayerProxy::Ptr &)> &, int) {}
void delReplayStreamProxy(const std::string &) {}
const mediakit::PlayerProxy::Ptr getReplayStreamProxy(const std::string &) { return nullptr; }

namespace {

bool expect(bool condition, const std::string &message) {
    if (!condition) std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

MediaGapFillTask makeTask(const std::string &id, const std::string &session,
                          const std::string &stream, int index, uint64_t begin,
                          uint64_t end, uint64_t processed, const std::string &state) {
    MediaGapFillTask task;
    task.connection_id = id;
    task.gap_id = "gap-100";
    task.session_id = session;
    task.device_id = "camera-a";
    task.stream_id = stream;
    task.source_type = "sdcard:camera-a";
    task.origin_url = "rtsp://camera/recording";
    task.source_begin_time = 90;
    task.segment_index = index;
    task.disconnect_time = 100;
    task.reconnect_time = 140;
    task.begin_time = begin;
    task.end_time = end;
    task.processed_up_to = processed;
    task.processing_start_time = 150;
    task.processing_end_time = state == "Success" || state == "Failed" ? 160 : 0;
    task.retry_count = 0;
    task.retry_limit = 3;
    task.next_retry_time = 0;
    task.last_attempt_time = 0;
    task.state = state;
    task.updated_time = 160;
    return task;
}

} // namespace

int main() {
    char temp_dir[] = "/tmp/s3-gap-fill-test-XXXXXX";
    if (!mkdtemp(temp_dir)) return 1;
    mINI::Instance()[Database::kDbSavePath] = std::string(temp_dir);

    MediaGapFillTaskImpl repository;
    auto transaction = repository.getExecutor()->beginWriteTxn();
    transaction->execScript(
        "CREATE TABLE media_gap_fill_task ("
        "connection_id TEXT NOT NULL PRIMARY KEY, gap_id TEXT NOT NULL, session_id TEXT NOT NULL, "
        "device_id TEXT NOT NULL, stream_id TEXT NOT NULL, source_type TEXT NOT NULL, "
        "origin_url TEXT NOT NULL, source_begin_time INTEGER NOT NULL, "
        "segment_index INTEGER NOT NULL, disconnect_time INTEGER NOT NULL, reconnect_time INTEGER NOT NULL, "
        "begin_time INTEGER NOT NULL, end_time INTEGER NOT NULL, processed_up_to INTEGER NOT NULL, "
        "processing_start_time INTEGER NOT NULL, processing_end_time INTEGER NOT NULL, "
        "retry_count INTEGER NOT NULL, retry_limit INTEGER NOT NULL, "
        "next_retry_time INTEGER NOT NULL, last_attempt_time INTEGER NOT NULL, state TEXT NOT NULL, "
        "last_error TEXT NOT NULL, updated_time INTEGER NOT NULL);");
    transaction->execScript(
        "CREATE TABLE media_gap_fill_session ("
        "device_id TEXT NOT NULL, gap_id TEXT NOT NULL, session_id TEXT NOT NULL, "
        "source_type TEXT NOT NULL, disconnect_time INTEGER NOT NULL, reconnect_time INTEGER NOT NULL, "
        "discovery_state TEXT NOT NULL, control_state TEXT NOT NULL, discovery_attempts INTEGER NOT NULL, "
        "last_error TEXT NOT NULL, updated_time INTEGER NOT NULL, PRIMARY KEY(device_id, gap_id));"
        "CREATE TABLE local_resource_status_gap ("
        "resource_id TEXT NOT NULL, gap_id TEXT NOT NULL, disconnect_time INTEGER NOT NULL, "
        "reconnect_time INTEGER NOT NULL, disconnect_reason TEXT NOT NULL, state INTEGER NOT NULL, "
        "updated_time INTEGER NOT NULL, PRIMARY KEY(resource_id, gap_id));");
    transaction->commit();

    bool ok = true;
    auto p0 = MediaGapFillManager::calculatePlaybackProgress(100, 200, 0.0);
    auto p50 = MediaGapFillManager::calculatePlaybackProgress(100, 200, 0.5);
    auto p99 = MediaGapFillManager::calculatePlaybackProgress(100, 200, 0.99);
    auto pOver99 = MediaGapFillManager::calculatePlaybackProgress(100, 200, 0.9901);
    auto pHigh = MediaGapFillManager::calculatePlaybackProgress(100, 200, 2.0);
    auto pNan = MediaGapFillManager::calculatePlaybackProgress(
        100, 200, std::numeric_limits<double>::quiet_NaN());
    ok &= expect(p0.processed_up_to == 100 && !p0.completed,
                 "zero playback progress starts at segment begin");
    ok &= expect(p50.processed_up_to == 150 && !p50.completed,
                 "calculate duration-relative playback progress");
    ok &= expect(p99.processed_up_to == 199 && !p99.completed,
                 "exactly 99 percent is not complete");
    ok &= expect(pOver99.processed_up_to == 200 && pOver99.completed &&
                 pHigh.processed_up_to == 200 && pHigh.completed,
                 "progress over 99 percent completes and clamps to end");
    ok &= expect(pNan.processed_up_to == 100 && !pNan.completed,
                 "invalid progress is safely clamped");
    ok &= expect(MediaGapFillManager::calculateRetryDelay(1) == 2 &&
                 MediaGapFillManager::calculateRetryDelay(2) == 3 &&
                 MediaGapFillManager::calculateRetryDelay(30) == 60,
                 "retry delay preserves bounded legacy backoff");
    ok &= expect(repository.addOrUpdate(makeTask("a/0", "sess-100", "stream-a", 0,
                                                 100, 120, 120, "Success")), "save success segment");
    ok &= expect(repository.addOrUpdate(makeTask("a/1", "sess-100", "stream-a", 1,
                                                 120, 140, 130, "Failed")), "save failed segment");
    ok &= expect(repository.addOrUpdate(makeTask("b/0", "sess-100", "stream-b", 0,
                                                 100, 120, 100, "Pending")), "save pending segment");

    auto &manager = MediaGapFillManager::Instance();
    auto session = manager.makeDisconnectSessionJson("camera-a", "sess-100");
    ok &= expect(session["session_id"].asString() == "sess-100", "preserve historical session id");
    ok &= expect(session["disconnect_time"].asUInt64() == 100 &&
                 session["reconnect_time"].asUInt64() == 140, "preserve outage timestamps");
    ok &= expect(session["session_state"].asString() == "Pending", "aggregate state with old precedence");
    ok &= expect(session["stream_handle_infos"].isArray() &&
                 session["stream_handle_infos"].size() == 2, "emit old stream hierarchy");
    ok &= expect(session["stream_handle_infos"][0]["segments"][1]["segment_index"].asInt() == 1,
                 "emit stable segment index");
    ok &= expect(std::abs(session["progress_percent"].asDouble() - 37.5) < 0.001,
                 "preserve equal-weight progress across streams");

    auto all = manager.makeDisconnectSessionsJson("camera-a");
    ok &= expect(all.isArray() && all.size() == 1, "list sessions with old response shape");
    auto remaining = manager.makeRemainingSegmentsJson("camera-a");
    ok &= expect(remaining.size() == 1 && remaining[0].getMemberNames().size() == 3,
                 "remaining response contains only historical summary fields");
    ok &= expect(manager.makeDisconnectSessionJson("camera-a", "missing").empty(),
                 "unknown session keeps old empty-object behavior");

    ok &= expect(!manager.resetFailedSession("camera-a", "sess-100"),
                 "do not reset individual failures while aggregate session is pending");

    VideoEncoderConfig primary{};
    primary.vcodec = "H264";
    primary.width = 1920;
    primary.height = 1080;
    primary.bitrate = 4096;
    primary.fps = 25;
    VideoEncoderConfig secondary{};
    secondary.vcodec = "H264";
    secondary.width = 640;
    secondary.height = 360;
    secondary.bitrate = 512;
    secondary.fps = 15;
    RecordingInformation low;
    low.uri = "rtsp://camera/low";
    low.vEncoder = secondary;
    RecordingInformation high;
    high.uri = "rtsp://camera/high";
    high.vEncoder = primary;
    std::unordered_map<std::string, VideoEncoderConfig> configured;
    configured["stream-main"] = primary;
    configured["stream-sub"] = secondary;
    auto mappings = MediaGapFillManager::mapRecordingsToStreams(configured, {low, high});
    ok &= expect(mappings.size() == 2 && mappings[0].stream_id == "stream-sub" &&
                 mappings[0].recording_index == 0 && mappings[1].stream_id == "stream-main" &&
                 mappings[1].recording_index == 1, "map 2x2 recording profiles by encoder score");

    LocalResourceStatusGap gap;
    gap.resource_id = "camera-discovery";
    gap.gap_id = "gap-discovery";
    gap.disconnect_time = 200;
    gap.reconnect_time = 260;
    gap.updated_time = 260;
    LocalResourceStatusGapImp gap_repository;
    ok &= expect(gap_repository.addOrUpdate(gap), "save closed resource gap");
    int discovery_count = 0;
    MediaGapFillManager::SourceProvider discovery_provider;
    discovery_provider.discover = [&](const LocalResourceStatusGap &) {
        ++discovery_count;
        return MediaGapDiscoveryResult{};
    };
    discovery_provider.start = [](const MediaGapFillRequest &,
                                  const MediaGapFillManager::ClosedCallback &) { return true; };
    ok &= expect(manager.registerSource("discovery-test", discovery_provider),
                 "register discovery provider");
    manager.fillDevice("camera-discovery", "discovery-test");
    manager.fillDevice("camera-discovery", "discovery-test");
    ok &= expect(discovery_count == 1, "terminal no-media gap is not discovered repeatedly");

    MediaGapFillSession claim_session;
    claim_session.device_id = "camera-claim";
    claim_session.gap_id = "gap-claim";
    claim_session.session_id = "sess-claim";
    claim_session.source_type = "discovery-test";
    claim_session.disconnect_time = 500;
    claim_session.reconnect_time = 520;
    claim_session.updated_time = 500;
    MediaGapFillSessionImpl session_repository;
    ok &= expect(session_repository.addOrUpdate(claim_session), "save discovery claim session");
    auto competing_claim = claim_session;
    ok &= expect(session_repository.claimDiscovery(claim_session, 600),
                 "first discovery worker claims gap");
    ok &= expect(!session_repository.claimDiscovery(competing_claim, 600),
                 "stale discovery worker cannot claim the same gap");

    std::vector<MediaGapFillRequest> started;
    std::vector<MediaGapFillRequest> stopped;
    std::map<std::string, MediaGapFillManager::ClosedCallback> close_callbacks;
    MediaGapFillManager::SourceProvider scheduler_provider;
    scheduler_provider.discover = [](const LocalResourceStatusGap &) {
        return MediaGapDiscoveryResult{};
    };
    scheduler_provider.start = [&](const MediaGapFillRequest &request,
                                   const MediaGapFillManager::ClosedCallback &closed) {
        started.push_back(request);
        close_callbacks[request.id] = closed;
        return true;
    };
    scheduler_provider.stop = [&](const MediaGapFillRequest &request) {
        stopped.push_back(request);
    };
    scheduler_provider.progress = [](const MediaGapFillRequest &request, uint64_t &processed) {
        processed = std::min(request.end_time, request.begin_time + 5);
        return true;
    };
    ok &= expect(manager.registerSource("scheduler-test", scheduler_provider),
                 "register scheduler provider");

    auto makeRequest = [](const std::string &id, const std::string &session,
                          const std::string &stream, uint64_t disconnect) {
        MediaGapFillRequest request;
        request.id = id;
        request.source_type = "scheduler-test";
        request.gap_id = "gap-" + session;
        request.session_id = session;
        request.device_id = "camera-scheduler";
        request.stream_id = stream;
        request.origin_url = "mock://recording";
        request.source_begin_time = disconnect;
        request.disconnect_time = disconnect;
        request.reconnect_time = disconnect + 20;
        request.begin_time = disconnect;
        request.end_time = disconnect + 20;
        request.segment_index = 0;
        return request;
    };
    ok &= expect(manager.enqueue(makeRequest("sched-a", "sess-300", "main", 300)),
                 "enqueue first stream");
    ok &= expect(manager.enqueue(makeRequest("sched-b", "sess-300", "sub", 300)),
                 "enqueue parallel stream in same session");
    ok &= expect(manager.enqueue(makeRequest("sched-c", "sess-400", "third", 400)),
                 "enqueue next session");
    ok &= expect(started.size() == 2, "only one session per device runs while its streams run in parallel");
    manager.checkpointProgress();
    auto checkpointed = repository.findById("sched-a");
    ok &= expect(!checkpointed.empty() && checkpointed.front().processed_up_to == 305,
                 "persist live monotonic checkpoint");
    ok &= expect(manager.stopSession("camera-scheduler"), "atomically stop active session");
    ok &= expect(stopped.size() == 2 && started.size() == 2,
                 "stop all session streams without starting next session");
    close_callbacks["sched-a"](true, 320, "late callback");
    auto after_late_close = repository.findById("sched-a");
    ok &= expect(!after_late_close.empty() && after_late_close.front().state == "Pending" &&
                 after_late_close.front().processed_up_to == 305,
                 "ignore stale callback after stop");

    int retry_starts = 0;
    MediaGapFillManager::SourceProvider retry_provider;
    retry_provider.discover = [](const LocalResourceStatusGap &) {
        return MediaGapDiscoveryResult{};
    };
    retry_provider.start = [&](const MediaGapFillRequest &request,
                               const MediaGapFillManager::ClosedCallback &closed) {
        ++retry_starts;
        closed(false, request.begin_time + 2, "simulated source failure");
        return true;
    };
    ok &= expect(manager.registerSource("retry-test", retry_provider),
                 "register retry provider");
    auto retry_request = makeRequest("retry-task", "sess-500", "main", 500);
    retry_request.device_id = "camera-retry";
    retry_request.source_type = "retry-test";
    retry_request.retry_limit = 2;
    ok &= expect(manager.enqueue(retry_request), "enqueue retry task");
    // The close callback is persisted asynchronously, off the player's poller.
    auto retry_task = repository.findById("retry-task");
    for (int i = 0; i < 300 && (retry_task.empty() || retry_task.front().retry_count != 1); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        retry_task = repository.findById("retry-task");
    }
    ok &= expect(retry_starts == 1 && !retry_task.empty() &&
                 retry_task.front().state == "Pending" &&
                 retry_task.front().retry_count == 1 &&
                 retry_task.front().next_retry_time > static_cast<uint64_t>(time(nullptr)),
                 "failed playback is persisted with backoff instead of immediate retry");
    ok &= expect(manager.stop("retry-task"), "remove pending retry task from runtime queue");
    retry_task = repository.findById("retry-task");
    const uint64_t persisted_retry_time = static_cast<uint64_t>(time(nullptr)) + 60;
    retry_task.front().next_retry_time = persisted_retry_time;
    ok &= expect(repository.addOrUpdate(retry_task.front()), "seed persisted retry schedule");
    ok &= expect(manager.enqueue(retry_request), "recover task waiting for persisted retry deadline");
    retry_task = repository.findById("retry-task");
    ok &= expect(retry_starts == 1 && !retry_task.empty() &&
                 retry_task.front().next_retry_time == persisted_retry_time,
                 "restart preserves retry deadline without starting task early");
    ok &= expect(manager.stop("retry-task"), "remove recovered retry from runtime queue");

    LocalResourceStatusGap recovery_gap;
    recovery_gap.resource_id = "camera-recovery";
    recovery_gap.gap_id = "gap-recovery";
    recovery_gap.disconnect_time = 600;
    recovery_gap.reconnect_time = 620;
    recovery_gap.updated_time = 620;
    ok &= expect(gap_repository.addOrUpdate(recovery_gap), "save recovery gap");
    MediaGapFillSession recovery_session;
    recovery_session.device_id = "camera-recovery";
    recovery_session.gap_id = "gap-recovery";
    recovery_session.session_id = "sess-600";
    recovery_session.source_type = "recovery-test";
    recovery_session.disconnect_time = 600;
    recovery_session.reconnect_time = 620;
    recovery_session.discovery_state = MediaGapDiscoveryState::Ready;
    recovery_session.updated_time = 620;
    ok &= expect(session_repository.addOrUpdate(recovery_session), "save ready recovery session");
    auto recovery_task = makeTask("recovery-task", "sess-600", "main", 0,
                                  600, 620, 610, "Processing");
    recovery_task.device_id = "camera-recovery";
    recovery_task.gap_id = "gap-recovery";
    recovery_task.source_type = "recovery-test";
    recovery_task.origin_url = "mock://recovery";
    recovery_task.source_begin_time = 590;
    ok &= expect(repository.addOrUpdate(recovery_task), "seed processing task from previous process");
    std::vector<MediaGapFillRequest> recovered_starts;
    MediaGapFillManager::SourceProvider recovery_provider;
    recovery_provider.discover = [](const LocalResourceStatusGap &) {
        return MediaGapDiscoveryResult{};
    };
    recovery_provider.prepare = [](MediaGapFillRequest &request) {
        request.username = "runtime-user";
        request.password = "runtime-password";
    };
    recovery_provider.start = [&](const MediaGapFillRequest &request,
                                  const MediaGapFillManager::ClosedCallback &) {
        recovered_starts.push_back(request);
        return true;
    };
    recovery_provider.stop = [](const MediaGapFillRequest &) {};
    ok &= expect(manager.registerSource("recovery-test", recovery_provider),
                 "register recovery provider");
    manager.fillDevice("camera-recovery", "recovery-test");
    ok &= expect(recovered_starts.size() == 1 && recovered_starts.front().begin_time == 610 &&
                 recovered_starts.front().username == "runtime-user",
                 "restart resumes Processing task from persisted checkpoint with runtime credentials");
    ok &= expect(manager.stopSession("camera-recovery"), "stop recovered session");

    mINI::Instance()["database.resource_status_gap_retention_days"] = 1;
    const uint64_t cleanup_now = static_cast<uint64_t>(time(nullptr));
    LocalResourceStatusGap expired_gap;
    expired_gap.resource_id = "camera-cleanup";
    expired_gap.gap_id = "gap-cleanup";
    expired_gap.disconnect_time = cleanup_now - 3 * 24 * 3600;
    expired_gap.reconnect_time = cleanup_now - 2 * 24 * 3600;
    expired_gap.updated_time = expired_gap.reconnect_time;
    ok &= expect(gap_repository.addOrUpdate(expired_gap), "save expired gap");
    MediaGapFillSession expired_session;
    expired_session.device_id = expired_gap.resource_id;
    expired_session.gap_id = expired_gap.gap_id;
    expired_session.session_id = "sess-cleanup";
    expired_session.source_type = "cleanup-test";
    expired_session.disconnect_time = expired_gap.disconnect_time;
    expired_session.reconnect_time = expired_gap.reconnect_time;
    expired_session.discovery_state = MediaGapDiscoveryState::Completed;
    expired_session.updated_time = expired_gap.updated_time;
    ok &= expect(session_repository.addOrUpdate(expired_session), "save expired child session");
    auto expired_task = makeTask("cleanup-task", "sess-cleanup", "main", 0,
                                 expired_gap.disconnect_time, expired_gap.reconnect_time,
                                 expired_gap.reconnect_time, "Success");
    expired_task.device_id = expired_gap.resource_id;
    expired_task.gap_id = expired_gap.gap_id;
    expired_task.source_type = "cleanup-test";
    ok &= expect(repository.addOrUpdate(expired_task), "save expired child task");
    ok &= expect(ResourceStatusManager::Instance().onDisconnected(
                     "cleanup-trigger", "test", cleanup_now),
                 "trigger retention cleanup");
    ok &= expect(gap_repository.findByResourceIdAndTimeRange("camera-cleanup").empty() &&
                 session_repository.find("camera-cleanup", "gap-cleanup").empty() &&
                 repository.findById("cleanup-task").empty(),
                 "retention cleanup removes gap, session and task atomically");

    if (!ok) return 1;
    std::cout << "Media gap fill compatibility tests passed" << std::endl;
    return 0;
}
