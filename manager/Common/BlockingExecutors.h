#ifndef COMMON_BLOCKINGEXECUTORS_H
#define COMMON_BLOCKINGEXECUTORS_H

#include <string>
#include "Thread/TaskExecutor.h"

namespace managerkit {

/**
 * Named thread pools for blocking work (SQLite, filesystem, S3, device SOAP/SDK calls).
 *
 * WorkThreadPool threads are EventPollers that also run timers and ordered queues (SyncManager,
 * camera ESC writes, tier storage); a long blocking call there delays all of them. These pools are
 * plain toolkit::ThreadPool instances, created on first use with a size read once from config.
 */
class BlockingExecutors {
public:
    // Blocking part of HTTP API handlers (server/ApiWorkDispatcher). Threads are named "api worker N".
    // Size: `blocking.apiThreads`, falling back to the legacy `api.workerThreads`;
    // <= 0 means max(2, hardware_concurrency / 2).
    static toolkit::TaskExecutor::Ptr api();

    // Blocking device calls (ONVIF SOAP, vendor SDK). Threads are named "device worker N".
    // Size: `blocking.deviceThreads` (<= 0 means 4).
    static toolkit::TaskExecutor::Ptr device();

    // Device discovery probes (subnet scan: one ONVIF connect per IP, ~20s for an IP that does not
    // answer). Kept apart from `device` so a long scan never delays commands to known cameras.
    // Threads are named "discovery N". Size: `blocking.discoveryThreads` (<= 0 means 8).
    static toolkit::TaskExecutor::Ptr discovery();

    // Waiting for an ffmpeg/ffprobe child process (snapshots, thumbnails). One wait lasts as long as
    // the media takes, so it is kept apart from `api`: a slow thumbnail must not use up the threads
    // that serve API requests. Threads are named "ffmpeg worker N".
    // Size: `blocking.ffmpegThreads` (<= 0 means 4).
    static toolkit::TaskExecutor::Ptr ffmpeg();

    // Pool sizes as resolved from config (exposed for logging and tests).
    static int apiThreadCount();
    static int deviceThreadCount();
    static int discoveryThreadCount();
    static int ffmpegThreadCount();
};

namespace Blocking {
extern const std::string kApiThreads;
extern const std::string kDeviceThreads;
extern const std::string kDiscoveryThreads;
extern const std::string kFfmpegThreads;
extern const std::string kLegacyApiWorkerThreads;
} // namespace Blocking

} // namespace managerkit

#endif // COMMON_BLOCKINGEXECUTORS_H
