#ifndef S3MEDIAKIT_API_WORK_DISPATCHER_H
#define S3MEDIAKIT_API_WORK_DISPATCHER_H

#include <exception>
#include <functional>
#include <memory>
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
// Pool size: config `api.workerThreads` (<= 0: max(2, hardware_concurrency / 2)), read once.
toolkit::TaskExecutor::Ptr getApiWorkExecutor();

// Sends the error response for an exception thrown while serving an API request, using the same
// mapping as the synchronous dispatch (ApiRetException / SqliteException / std::exception).
// As in the synchronous path, the connection is shut down after an ApiRetException.
void responseApiException(const std::exception_ptr &error,
                          const mediakit::HttpSession::HttpResponseInvoker &invoker,
                          const std::weak_ptr<toolkit::SocketHelper> &session);

// Request context for handlers that continue asynchronously (e.g. after an HTTP health check):
// the poller that owns the HTTP session, and the session itself (weak).
toolkit::EventPoller::Ptr apiRequestPoller(toolkit::SockInfo &sender);
std::weak_ptr<toolkit::SocketHelper> apiRequestSession(toolkit::SockInfo &sender);

// Generic form: `work` runs on the API worker pool, then `done(error)` runs on `owner`
// exactly once (`error` is null on success). If `session` is given and already expired
// before the work starts, the work is skipped and `done` is not called.
void runApiWork(const toolkit::EventPoller::Ptr &owner,
                const std::weak_ptr<toolkit::SocketHelper> &session,
                const std::function<void()> &work,
                const std::function<void(const std::exception_ptr &error)> &done);

// Typical handler form: runs `work`, then `completion` on the request poller, then replies
// `200 + val` (or the mapped error). `headerOut` / `val` are copied.
void dispatchApiWork(toolkit::SockInfo &sender,
                     const mediakit::HttpSession::KeyValue &headerOut,
                     const Json::Value &val,
                     const mediakit::HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work,
                     const ApiCompletionTask &completion = nullptr);

// Same as above with an explicit owner poller and session (for callers without a SockInfo, and tests).
void dispatchApiWork(const toolkit::EventPoller::Ptr &owner,
                     const std::weak_ptr<toolkit::SocketHelper> &session,
                     const mediakit::HttpSession::KeyValue &headerOut,
                     const Json::Value &val,
                     const mediakit::HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work,
                     const ApiCompletionTask &completion = nullptr);

#endif // S3MEDIAKIT_API_WORK_DISPATCHER_H
