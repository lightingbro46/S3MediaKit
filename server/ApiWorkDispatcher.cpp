#include "ApiWorkDispatcher.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "Common/BlockingExecutors.h"
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

namespace {

const string kApiLimitPrefix = "api.limit.";

string limitKey(const string &group, const char *field) {
    return kApiLimitPrefix + group + "." + field;
}

onceToken s_api_limit_config([]() {
    // Snapshots are large and slow to build: few at a time.
    mINI::Instance()[limitKey("sync.snapshot", "maxInflight")] = 2;
    mINI::Instance()[limitKey("sync.snapshot", "maxQueueMs")] = 30000;
    mINI::Instance()[limitKey("sync.changes", "maxInflight")] = 8;
    mINI::Instance()[limitKey("sync.changes", "maxQueueMs")] = 30000;
    // Each ONVIF discovery may block up to ~20s on an address that does not answer.
    mINI::Instance()[limitKey("device.discovery", "maxInflight")] = 4;
    mINI::Instance()[limitKey("device.discovery", "maxQueueMs")] = 30000;
});

int readLimit(const string &key) {
    auto &ini = mINI::Instance();
    auto it = ini.find(key);
    if (it == ini.end() || it->second.empty()) {
        return 0;
    }
    return it->second.as<int>();
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

void sendApiError(int code, const string &msg, int status_code, const HttpSession::HttpResponseInvoker &invoker) {
    GET_CONFIG(string, charSet, Http::kCharSet);
    HttpSession::KeyValue headerOut;
    headerOut["Content-Type"] = string("application/json; charset=") + charSet;
    Json::Value res;
    res["code"] = code;
    res["msg"] = msg;
    invoker(status_code, headerOut, res.toStyledString());
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

int apiWorkInflight(const string &group) {
    return inflightCounter(group)->load();
}

void runApiWork(const EventPoller::Ptr &owner, const weak_ptr<SocketHelper> &session,
                const function<void()> &work, const function<void(const exception_ptr &)> &done,
                const ApiWorkLimit &limit) {
    CHECK(owner, "API work requires an owner poller");
    // An empty weak_ptr means "no session to watch"; an expired one means the client is gone.
    const bool check_session = !isEmptyWeak(session);

    shared_ptr<InflightSlot> slot;
    if (!limit.group.empty() && limit.max_inflight > 0) {
        auto counter = inflightCounter(limit.group);
        if (++(*counter) > limit.max_inflight) {
            --(*counter);
            WarnL << "API group " << limit.group << " is busy (" << limit.max_inflight << " requests in flight), rejecting";
            auto error = make_exception_ptr(ApiBusyException("Server busy (" + limit.group + "), retry later"));
            owner->async([done, error]() { done(error); }, false);
            return;
        }
        slot = std::make_shared<InflightSlot>(counter);
    }
    const auto queued_at = chrono::steady_clock::now();
    const string group = limit.group;
    const int max_queue_ms = limit.group.empty() ? 0 : limit.max_queue_ms;

    getApiWorkExecutor()->async([owner, session, check_session, work, done, slot, queued_at, group, max_queue_ms]() {
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
    }, false);
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
