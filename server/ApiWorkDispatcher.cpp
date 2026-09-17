#include "ApiWorkDispatcher.h"

#include <algorithm>
#include <memory>
#include <thread>
#include "Common/config.h"
#include "Common/macros.h"
#include "Thread/ThreadPool.h"
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

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace {

const string kApiWorkerThreads = "api.workerThreads";

onceToken s_api_worker_config([]() {
    mINI::Instance()[kApiWorkerThreads] = 0;
});

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
    static TaskExecutor::Ptr s_executor = []() {
        int threads = mINI::Instance()[kApiWorkerThreads];
        if (threads <= 0) {
            threads = max(2, static_cast<int>(thread::hardware_concurrency() / 2));
        }
        InfoL << "API worker pool started with " << threads << " threads";
        return std::make_shared<ThreadPool>(threads, ThreadPool::PRIORITY_NORMAL, true, false, "api worker");
    }();
    return s_executor;
}

void responseApiException(const exception_ptr &error, const HttpSession::HttpResponseInvoker &invoker,
                          const weak_ptr<SocketHelper> &session) {
    // Keep in sync with the synchronous dispatch in WebApi.cpp (installWebApi / http request listener).
    try {
        rethrow_exception(error);
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

void runApiWork(const EventPoller::Ptr &owner, const weak_ptr<SocketHelper> &session,
                const function<void()> &work, const function<void(const exception_ptr &)> &done) {
    CHECK(owner, "API work requires an owner poller");
    // An empty weak_ptr means "no session to watch"; an expired one means the client is gone.
    const bool check_session = !isEmptyWeak(session);
    getApiWorkExecutor()->async([owner, session, check_session, work, done]() {
        if (check_session && session.expired()) {
            // Client is gone: nobody will read the response, skip the blocking work.
            return;
        }
        exception_ptr error;
        try {
            work();
        } catch (...) {
            error = current_exception();
        }
        // Completion and response always run on the request poller.
        owner->async([done, error]() { done(error); }, false);
    }, false);
}

void dispatchApiWork(const EventPoller::Ptr &owner, const weak_ptr<SocketHelper> &session,
                     const HttpSession::KeyValue &headerOut, const Json::Value &val,
                     const HttpSession::HttpResponseInvoker &invoker,
                     const ApiWorkTask &work, const ApiCompletionTask &completion) {
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
        });
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
                     const ApiWorkTask &work, const ApiCompletionTask &completion) {
    dispatchApiWork(apiRequestPoller(sender), apiRequestSession(sender), headerOut, val, invoker, work, completion);
}
