#include "ApiWorkDispatcher.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "Common/BlockingExecutors.h"
#include "Common/SerialTaskQueue.h"
#include "Common/config.h"
#include "Common/macros.h"
#include "Util/logger.h"
#include "Util/mini.h"
#include "Util/onceToken.h"
#include "Util/util.h"

#ifdef ENABLE_MYSQL
#include "Util/SqlPool.h"
#endif // ENABLE_MYSQL

#ifdef ENABLE_SQLITE
#include "Util/SqlitePool.h"
#endif // ENABLE_SQLITE

#include "WebApi.h"
#include "WebApiErrCode.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

// Constant-initialized: the config registration below runs during static init and reads it.
const char kApiGlobalLimitGroup[] = "global";

namespace {

const string kApiLimitPrefix = "api.limit.";

string limitKey(const string &group, const char *field) {
    return kApiLimitPrefix + group + "." + field;
}

onceToken s_api_limit_config([]() {
    // Total cap of the API pool, applied to every dispatched request on top of its group limit:
    // without it, several busy groups together can fill every worker and grow an unbounded queue of
    // requests the clients have already given up on. Wide by default, so no existing endpoint
    // changes behaviour; `global` is a reserved group name.
    mINI::Instance()[limitKey(kApiGlobalLimitGroup, "maxInflight")] = 256;
    mINI::Instance()[limitKey(kApiGlobalLimitGroup, "maxQueueMs")] = 30000;
    // Snapshots are large and slow to build: few at a time.
    mINI::Instance()[limitKey("sync.snapshot", "maxInflight")] = 2;
    mINI::Instance()[limitKey("sync.snapshot", "maxQueueMs")] = 30000;
    mINI::Instance()[limitKey("sync.changes", "maxInflight")] = 8;
    mINI::Instance()[limitKey("sync.changes", "maxQueueMs")] = 30000;
    // Each ONVIF discovery may block up to ~20s on an address that does not answer.
    mINI::Instance()[limitKey("device.discovery", "maxInflight")] = 4;
    mINI::Instance()[limitKey("device.discovery", "maxQueueMs")] = 30000;
    // Storage reads (tier repositories, mount scan, dashboard): plain SQLite queries, but a whole
    // endpoint family, so the group gets a budget of its own well above normal use.
    mINI::Instance()[limitKey("storage.read", "maxInflight")] = 32;
    mINI::Instance()[limitKey("storage.read", "maxQueueMs")] = 15000;
    // Recorded thumbnails: opening the timefile plus rendering the overlay (which writes an SVG
    // mask to disk) before ffmpeg is even started.
    mINI::Instance()[limitKey("media.thumbnail", "maxInflight")] = 8;
    mINI::Instance()[limitKey("media.thumbnail", "maxQueueMs")] = 15000;
});

int readLimit(const string &key) {
    auto &ini = mINI::Instance();
    auto it = ini.find(key);
    if (it == ini.end() || it->second.empty()) {
        return 0;
    }
    return it->second.as<int>();
}

managerkit::SerialTaskQueue::Ptr serialQueue(const string &group) {
    static mutex s_mtx;
    static unordered_map<string, managerkit::SerialTaskQueue::Ptr> s_queues;
    lock_guard<mutex> lck(s_mtx);
    auto &queue = s_queues[group];
    if (!queue) {
        queue = std::make_shared<managerkit::SerialTaskQueue>(getApiWorkExecutor(), "api " + group);
    }
    return queue;
}

shared_ptr<atomic<int>> inflightCounter(const string &group) {
    static mutex s_mtx;
    static unordered_map<string, shared_ptr<atomic<int>>> s_counters;
    lock_guard<mutex> lck(s_mtx);
    auto &counter = s_counters[group];
    if (!counter) {
        counter = std::make_shared<atomic<int>>(0);
    }
    return counter;
}

// Holds one in-flight slot of a group; released when the last copy of the task is destroyed,
// whether the work ran, threw or was skipped.
class InflightSlot {
public:
    explicit InflightSlot(shared_ptr<atomic<int>> counter) : _counter(std::move(counter)) {}
    ~InflightSlot() {
        if (_counter) {
            --(*_counter);
        }
    }

private:
    shared_ptr<atomic<int>> _counter;
};

// Takes an in-flight slot of `group`; false when the group already has `max_inflight` requests.
// No group or no limit means no slot to take, which always succeeds.
bool acquireInflight(const string &group, int max_inflight, shared_ptr<InflightSlot> &slot) {
    if (group.empty() || max_inflight <= 0) {
        return true;
    }
    auto counter = inflightCounter(group);
    if (++(*counter) > max_inflight) {
        --(*counter);
        return false;
    }
    slot = std::make_shared<InflightSlot>(counter);
    return true;
}

// The stricter of two queue deadlines (<= 0 means unlimited).
int tighterQueueMs(int a, int b) {
    if (a <= 0) {
        return b > 0 ? b : 0;
    }
    if (b <= 0) {
        return a;
    }
    return a < b ? a : b;
}

void sendApiError(int code, const string &msg, int status_code, const HttpSession::HttpResponseInvoker &invoker) {
    GET_CONFIG(string, charSet, Http::kCharSet);
    HttpSession::KeyValue headerOut;
    headerOut["Content-Type"] = string("application/json; charset=") + charSet;
    Json::Value res;
    res["code"] = code;
    res["msg"] = msg;
    invoker(status_code, headerOut, res.toStyledString());
}

// Same answer as RETURN_API_RESPONSE(ex.code(), ex.what()) on `val`.
void sendApiResponse(const ApiResponseException &ex, Json::Value val, const HttpSession::KeyValue &headerOut,
                     const HttpSession::HttpResponseInvoker &invoker) {
    const auto code = static_cast<managerkit::ApiErrCode>(ex.code());
    string msg = ex.what();
    if (msg.empty()) {
        msg = managerkit::getDefaultMessage(code);
    }
    val["code"] = static_cast<int>(code);
    val["msg"] = msg;
    invoker(managerkit::getStatusCode(code), headerOut, val.toStyledString());
}

bool isEmptyWeak(const weak_ptr<SocketHelper> &ptr) {
    weak_ptr<SocketHelper> empty;
    return !ptr.owner_before(empty) && !empty.owner_before(ptr);
}

} // namespace

TaskExecutor::Ptr getApiWorkExecutor() {
    return managerkit::BlockingExecutors::api();
}

void responseApiException(const exception_ptr &error, const HttpSession::HttpResponseInvoker &invoker,
                          const weak_ptr<SocketHelper> &session) {
    // Keep in sync with the synchronous dispatch in WebApi.cpp (installWebApi / http request listener).
    try {
        rethrow_exception(error);
    } catch (ApiResponseException &ex) {
        GET_CONFIG(string, charSet, Http::kCharSet);
        HttpSession::KeyValue headerOut;
        headerOut["Content-Type"] = string("application/json; charset=") + charSet;
        sendApiResponse(ex, Json::Value(), headerOut, invoker);
    } catch (ApiBusyException &ex) {
        // Backpressure: the client may retry, keep the connection.
        sendApiError(static_cast<int>(managerkit::ApiErrCode::CODE_SERVER_BUSY), ex.what(), 503, invoker);
    } catch (ApiRetException &ex) {
        sendApiError(ex.code(), ex.what(), ex.status_code(), invoker);
        auto helper = session.lock();
        if (helper) {
            string reason = ex.what();
            weak_ptr<SocketHelper> weak_helper = helper;
            helper->getPoller()->async([weak_helper, reason]() {
                auto helper = weak_helper.lock();
                if (helper) {
                    helper->shutdown(SockException(Err_shutdown, reason));
                }
            }, false);
        }
    }
#ifdef ENABLE_MYSQL
    catch (SqlException &ex) {
        sendApiError(API::SqlFailed, StrPrinter << "Failed to operate the database:" << ex.what() << ":" << ex.getSql(), 500, invoker);
    }
#endif // ENABLE_MYSQL
#ifdef ENABLE_SQLITE
    catch (SqliteException &ex) {
        sendApiError(API::SqlFailed, StrPrinter << "Failed to operate the database:" << ex.what() << ":" << ex.getSql(), 500, invoker);
    }
#endif // ENABLE_SQLITE
    catch (std::exception &ex) {
        sendApiError(API::Exception, ex.what(), 500, invoker);
    } catch (...) {
        sendApiError(API::Exception, "unknown exception", 500, invoker);
    }
}

ApiWorkLimit apiWorkLimit(const string &group) {
    return ApiWorkLimit(group, readLimit(limitKey(group, "maxInflight")), readLimit(limitKey(group, "maxQueueMs")));
}

ApiWorkLimit apiWorkSerial(const string &group) {
    return ApiWorkLimit(group, 0, 0, true);
}

int apiWorkInflight(const string &group) {
    return inflightCounter(group)->load();
}

void runApiWork(const EventPoller::Ptr &owner, const weak_ptr<SocketHelper> &session,
                const function<void()> &work, const function<void(const exception_ptr &)> &done,
                const ApiWorkLimit &limit) {
    CHECK(owner, "API work requires an owner poller");
    // An empty weak_ptr means "no session to watch"; an expired one means the client is gone.
    const bool check_session = !isEmptyWeak(session);

    auto rejectBusy = [&owner, &done](const string &group, int max_inflight) {
        WarnL << "API group " << group << " is busy (" << max_inflight << " requests in flight), rejecting";
        auto error = make_exception_ptr(ApiBusyException("Server busy (" + group + "), retry later"));
        owner->async([done, error]() { done(error); }, false);
    };

    // The group limit is checked first, so a heavy group is rejected on its own budget instead of
    // eating into the pool-wide one.
    shared_ptr<InflightSlot> group_slot;
    if (!acquireInflight(limit.group, limit.max_inflight, group_slot)) {
        rejectBusy(limit.group, limit.max_inflight);
        return;
    }
    // Pool-wide limit: applies to every request, whether or not it belongs to a group.
    const auto global_limit = apiWorkLimit(kApiGlobalLimitGroup);
    shared_ptr<InflightSlot> global_slot;
    if (!acquireInflight(global_limit.group, global_limit.max_inflight, global_slot)) {
        // group_slot is released here, before the response.
        rejectBusy(global_limit.group, global_limit.max_inflight);
        return;
    }
    const auto queued_at = chrono::steady_clock::now();
    string group = limit.group;
    int max_queue_ms = limit.group.empty() ? 0 : limit.max_queue_ms;
    const int queue_ms = tighterQueueMs(max_queue_ms, global_limit.max_queue_ms);
    if (queue_ms != max_queue_ms) {
        group = global_limit.group;
    }
    max_queue_ms = queue_ms;

    auto task = [owner, session, check_session, work, done, group_slot, global_slot, queued_at, group, max_queue_ms]() {
        if (check_session && session.expired()) {
            // Client is gone: nobody will read the response, skip the blocking work.
            return;
        }
        exception_ptr error;
        auto waited_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - queued_at).count();
        if (max_queue_ms > 0 && waited_ms > max_queue_ms) {
            // The client has probably given up already; answer busy instead of doing stale work.
            WarnL << "API group " << group << " request waited " << waited_ms << "ms in queue, dropping";
            error = make_exception_ptr(ApiBusyException("Server busy (" + group + "), retry later"));
        } else {
            try {
                work();
            } catch (...) {
                error = current_exception();
            }
        }
        // Completion and response always run on the request poller.
        owner->async([done, error]() { done(error); }, false);
    };
    if (limit.serial && !limit.group.empty()) {
        serialQueue(limit.group)->post(task);
    } else {
        getApiWorkExecutor()->async(task, false);
    }
}

void dispatchApiWork(const EventPoller::Ptr &owner, const weak_ptr<SocketHelper> &session,
                     const HttpSession::KeyValue &headerOut, const Json::Value &val,
                     const HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work, const ApiCompletionTask &completion,
                     const ApiWorkLimit &limit) {
    auto result = std::make_shared<Json::Value>(val);
    runApiWork(owner, session,
        [result, work]() { work(*result); },
        [result, completion, headerOut, invoker, session](const exception_ptr &error) {
            exception_ptr failure = error;
            if (!failure && completion) {
                try {
                    completion(*result);
                } catch (...) {
                    failure = current_exception();
                }
            }
            if (failure) {
                try {
                    rethrow_exception(failure);
                } catch (ApiResponseException &ex) {
                    // Keep what the work already put in `val` (e.g. data.flag = false).
                    sendApiResponse(ex, *result, headerOut, invoker);
                    return;
                } catch (...) {
                }
                responseApiException(failure, invoker, session);
                return;
            }
            invoker(200, headerOut, result->toStyledString());
        }, limit);
}

void runApiCallbackWork(const EventPoller::Ptr &owner, const weak_ptr<SocketHelper> &session,
                        const HttpSession::HttpResponseInvoker &invoker,
                        const function<void(const ApiReply &reply)> &start, const ApiWorkLimit &limit) {
    auto replied = std::make_shared<atomic<bool>>(false);
    ApiReply reply = [owner, replied](const function<void()> &respond) {
        if (replied->exchange(true)) {
            WarnL << "API reply ignored: the request was already answered";
            return;
        }
        owner->async(respond, false);
    };
    runApiWork(owner, session,
        [start, reply]() { start(reply); },
        [replied, invoker, session](const exception_ptr &error) {
            // Without an error the service replies through `reply` (maybe later).
            if (error && !replied->exchange(true)) {
                responseApiException(error, invoker, session);
            }
        }, limit);
}

EventPoller::Ptr apiRequestPoller(SockInfo &sender) {
    // The HTTP API dispatch passes the session itself as SockInfo (see WebApi.cpp).
    return static_cast<SocketHelper &>(sender).getPoller();
}

weak_ptr<SocketHelper> apiRequestSession(SockInfo &sender) {
    return static_cast<SocketHelper &>(sender).shared_from_this();
}

void dispatchApiWork(SockInfo &sender, const HttpSession::KeyValue &headerOut, const Json::Value &val,
                     const HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work, const ApiCompletionTask &completion,
                     const ApiWorkLimit &limit) {
    dispatchApiWork(apiRequestPoller(sender), apiRequestSession(sender), headerOut, val, invoker, work, completion, limit);
}
