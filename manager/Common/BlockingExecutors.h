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

    // Periodic storage maintenance (scanning the record tree, deleting expired segments, rebuilding
    // timefiles, cleaning temporary folders). One cycle walks every recorded file, so on a large
    // archive it can run for minutes; kept apart from `api` so it never uses up the threads that
    // serve REST requests. Threads are named "storage worker N".
    // Size: `blocking.storageThreads` (<= 0 means 1). One thread is enough and also keeps the
    // cycles of one manager from overlapping.
    static toolkit::TaskExecutor::Ptr storage();

    // Waiting for an ffmpeg/ffprobe child process (snapshots, thumbnails). One wait lasts as long as
    // the media takes, so it is kept apart from `api`: a slow thumbnail must not use up the threads
    // that serve API requests. Threads are named "ffmpeg worker N".
    // Size: `blocking.ffmpegThreads` (<= 0 means 4).
    static toolkit::TaskExecutor::Ptr ffmpeg();

    // Record encryption work: encrypting each finished segment and the re-encryption jobs that
    // rewrite a whole pool after a password or mode change. Both are long and disk bound, so they
    // are kept apart from `api` (must not starve REST) and from `storage` (whose single thread runs
    // the retention/timefile cycles). Threads are named "encryption worker N".
    // Size: `blocking.encryptionThreads` (<= 0 means 2).
    static toolkit::TaskExecutor::Ptr encryption();

    // Pool sizes as resolved from config (exposed for logging and tests).
    static int apiThreadCount();
    static int deviceThreadCount();
    static int encryptionThreadCount();
    static int discoveryThreadCount();
    static int storageThreadCount();
    static int ffmpegThreadCount();
};

namespace Blocking {
extern const std::string kApiThreads;
extern const std::string kDeviceThreads;
extern const std::string kDiscoveryThreads;
extern const std::string kStorageThreads;
extern const std::string kFfmpegThreads;
extern const std::string kLegacyApiWorkerThreads;
} // namespace Blocking

} // namespace managerkit

#endif // COMMON_BLOCKINGEXECUTORS_H
