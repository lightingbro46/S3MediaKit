#ifndef S3MEDIAKIT_API_WORK_DISPATCHER_H
#define S3MEDIAKIT_API_WORK_DISPATCHER_H

#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include "json/json.h"
#include "Http/HttpSession.h"
#include "Network/Socket.h"
#include "Poller/EventPoller.h"
#include "Thread/TaskExecutor.h"

/**
 * Moves the blocking part of an HTTP API handler (SQLite, filesystem, S3, ...) off the request
 * EventPoller. Flow:
 *
 *   request poller : auth / validation, copy request values   (in the handler, before dispatch)
 *   API worker     : work(val)                                  (blocking, may throw)
 *   request poller : completion(val), then response / error    (state owned by the poller)
 *
 * Handlers opt in explicitly; routes, payloads and error codes stay unchanged: exceptions are
 * mapped exactly like in the synchronous dispatch of WebApi.cpp.
 *
 * Lambdas must capture request data by value (never allArgs / val / headerOut by reference)
 * and must not capture raw `this`.
 */

// Blocking part of a request. Runs on the API worker pool, fills `val`, may throw.
using ApiWorkTask = std::function<void(Json::Value &val)>;
// Optional follow-up on the request poller (e.g. poller-owned state updates). May throw.
using ApiCompletionTask = std::function<void(Json::Value &val)>;

// Dedicated executor for blocking API work. It is separate from WorkThreadPool: WorkThreadPool
// threads are EventPollers that also run timers and ordered queues (SyncManager, camera ESC
// writes, tier storage), which a long API request must not delay.
// It is managerkit::BlockingExecutors::api() (pool size: `blocking.apiThreads`, legacy `api.workerThreads`).
toolkit::TaskExecutor::Ptr getApiWorkExecutor();

// Admission limit for a group of endpoints (backpressure). A request over the limit is answered
// 503 CODE_SERVER_BUSY without running its work, and the connection is kept:
//   - max_inflight: requests of the group queued or running at the same time (<= 0: unlimited);
//   - max_queue_ms: a request that waited longer in the worker queue is dropped (<= 0: unlimited).
//   - serial: requests of the group run one at a time, in arrival order (a SerialTaskQueue per
//     group on the API pool), e.g. mutations of shared caches/registrations.
// An empty group means no limit (the default, so existing handlers are unchanged).
struct ApiWorkLimit {
    ApiWorkLimit() = default;
    ApiWorkLimit(std::string group_in, int max_inflight_in, int max_queue_ms_in, bool serial_in = false)
        : group(std::move(group_in)), max_inflight(max_inflight_in), max_queue_ms(max_queue_ms_in), serial(serial_in) {}

    std::string group;
    int max_inflight = 0;
    int max_queue_ms = 0;
    bool serial = false;
};

// Serialized group: its requests run one at a time, in order (no in-flight/queue limit).
ApiWorkLimit apiWorkSerial(const std::string &group);

// Reserved group name of the pool-wide limit (config `api.limit.global.maxInflight` /
// `api.limit.global.maxQueueMs`). It is applied to **every** dispatched request, after its own
// group limit, so that several busy groups together cannot fill the API pool and grow an unbounded
// queue. Defaults are wide enough not to change the behaviour of existing endpoints.
extern const char kApiGlobalLimitGroup[];

// Limit of `group` from config `api.limit.<group>.maxInflight` / `api.limit.<group>.maxQueueMs`,
// read on every call. Registered groups: sync.snapshot, sync.changes, device.discovery,
// storage.read, media.thumbnail (plus the reserved `global` above).
ApiWorkLimit apiWorkLimit(const std::string &group);

// Requests of `group` currently queued or running (for tests and diagnostics).
int apiWorkInflight(const std::string &group);

// Error of a request rejected by an ApiWorkLimit: answered 503 CODE_SERVER_BUSY.
class ApiBusyException : public std::runtime_error {
public:
    explicit ApiBusyException(const std::string &msg) : std::runtime_error(msg) {}
};

// Thrown from API work to answer like RETURN_API_RESPONSE(code, msg): HTTP status of `code`,
// `code`/`msg` (default message when empty) added to the response, connection kept. With
// dispatchApiWork, fields already set on `val` by the work are kept in the response.
class ApiResponseException : public std::runtime_error {
public:
    ApiResponseException(int code, const std::string &msg) : std::runtime_error(msg), _code(code) {}
    int code() const { return _code; }

private:
    int _code;
};

// Sends the error response for an exception thrown while serving an API request, using the same
// mapping as the synchronous dispatch (ApiRetException / SqliteException / std::exception).
// As in the synchronous path, the connection is shut down after an ApiRetException (but not after
// an ApiBusyException).
void responseApiException(const std::exception_ptr &error,
                          const mediakit::HttpSession::HttpResponseInvoker &invoker,
                          const std::weak_ptr<toolkit::SocketHelper> &session);

// Request context for handlers that continue asynchronously (e.g. after an HTTP health check):
// the poller that owns the HTTP session, and the session itself (weak).
toolkit::EventPoller::Ptr apiRequestPoller(toolkit::SockInfo &sender);
std::weak_ptr<toolkit::SocketHelper> apiRequestSession(toolkit::SockInfo &sender);

// Generic form: `work` runs on the API worker pool, then `done(error)` runs on `owner`
// exactly once (`error` is null on success). If `session` is given and already expired
// before the work starts, the work is skipped and `done` is not called. A request rejected by
// `limit` does not run `work`; `done` gets an ApiBusyException.
void runApiWork(const toolkit::EventPoller::Ptr &owner,
                const std::weak_ptr<toolkit::SocketHelper> &session,
                const std::function<void()> &work,
                const std::function<void(const std::exception_ptr &error)> &done,
                const ApiWorkLimit &limit = ApiWorkLimit());

// Posts `respond` to the request poller. Only the first call of a request has an effect.
using ApiReply = std::function<void(const std::function<void()> &respond)>;

// For services that answer through a callback, possibly later and from another thread (e.g. after
// querying remote peers): `start(reply)` runs on the API worker; the service calls `reply(respond)`
// once and `respond` (which calls the invoker) runs on `owner`. If `start` throws before replying,
// the error is answered like in dispatchApiWork. Session/limit handling as in runApiWork.
void runApiCallbackWork(const toolkit::EventPoller::Ptr &owner,
                        const std::weak_ptr<toolkit::SocketHelper> &session,
                        const mediakit::HttpSession::HttpResponseInvoker &invoker,
                        const std::function<void(const ApiReply &reply)> &start,
                        const ApiWorkLimit &limit = ApiWorkLimit());

// Typical handler form: runs `work`, then `completion` on the request poller, then replies
// `200 + val` (or the mapped error). `headerOut` / `val` are copied.
void dispatchApiWork(toolkit::SockInfo &sender,
                     const mediakit::HttpSession::KeyValue &headerOut,
                     const Json::Value &val,
                     const mediakit::HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work,
                     const ApiCompletionTask &completion = nullptr,
                     const ApiWorkLimit &limit = ApiWorkLimit());

// Same as above with an explicit owner poller and session (for callers without a SockInfo, and tests).
void dispatchApiWork(const toolkit::EventPoller::Ptr &owner,
                     const std::weak_ptr<toolkit::SocketHelper> &session,
                     const mediakit::HttpSession::KeyValue &headerOut,
                     const Json::Value &val,
                     const mediakit::HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work,
                     const ApiCompletionTask &completion = nullptr,
                     const ApiWorkLimit &limit = ApiWorkLimit());

#endif // S3MEDIAKIT_API_WORK_DISPATCHER_H
