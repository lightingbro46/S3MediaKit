#include "VendorHttp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "Http/HttpRequester.h"
#include "Util/MD5.h"
#include "Util/base64.h"
#include "Util/onceToken.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// Value of `name` in a WWW-Authenticate challenge (quoted or not); "" when absent.
static string challengeParam(const string &challenge, const string &name) {
    size_t pos = 0;
    while ((pos = challenge.find(name, pos)) != string::npos) {
        const bool starts = pos == 0 || challenge[pos - 1] == ' ' || challenge[pos - 1] == ',';
        size_t eq = pos + name.size();
        while (eq < challenge.size() && challenge[eq] == ' ') {
            ++eq;
        }
        if (!starts || eq >= challenge.size() || challenge[eq] != '=') {
            pos += name.size();
            continue;
        }
        size_t begin = eq + 1;
        while (begin < challenge.size() && challenge[begin] == ' ') {
            ++begin;
        }
        if (begin < challenge.size() && challenge[begin] == '"') {
            size_t end = challenge.find('"', begin + 1);
            return challenge.substr(begin + 1, end == string::npos ? string::npos : end - begin - 1);
        }
        size_t end = challenge.find(',', begin);
        string value = challenge.substr(begin, end == string::npos ? string::npos : end - begin);
        while (!value.empty() && value.back() == ' ') {
            value.pop_back();
        }
        return value;
    }
    return "";
}

static bool isDigestMd5(const string &challenge) {
    if (strncasecmp(challenge.c_str(), "Digest", 6) != 0) {
        return false;
    }
    const string algorithm = challengeParam(challenge, "algorithm");
    return algorithm.empty() || strcasecmp(algorithm.c_str(), "MD5") == 0;
}

string vendorHttpAuthorization(const string &challenge, const string &method, const string &uri,
                               const string &username, const string &password, const string &cnonce) {
    if (strncasecmp(challenge.c_str(), "Basic", 5) == 0) {
        return "Basic " + encodeBase64(username + ":" + password);
    }
    if (!isDigestMd5(challenge)) {
        return "";
    }
    const string realm = challengeParam(challenge, "realm");
    const string nonce = challengeParam(challenge, "nonce");
    const string opaque = challengeParam(challenge, "opaque");
    // "auth" or "auth,auth-int": only auth is used.
    const bool qop_auth = challengeParam(challenge, "qop").find("auth") != string::npos;
    const string ha1 = MD5(username + ":" + realm + ":" + password).hexdigest();
    const string ha2 = MD5(method + ":" + uri).hexdigest();
    _StrPrinter printer;
    printer << "Digest username=\"" << username << "\", realm=\"" << realm << "\", nonce=\"" << nonce
            << "\", uri=\"" << uri << "\"";
    if (qop_auth) {
        const string nc = "00000001";
        printer << ", qop=auth, nc=" << nc << ", cnonce=\"" << cnonce << "\", response=\""
                << MD5(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2).hexdigest() << "\"";
    } else {
        printer << ", response=\"" << MD5(ha1 + ":" + nonce + ":" + ha2).hexdigest() << "\"";
    }
    if (!opaque.empty()) {
        printer << ", opaque=\"" << opaque << "\"";
    }
    return printer;
}

DeviceFailure vendorHttpFailure(int sock_err, int http_status) {
    // An HTTP answer came back (even when the body then failed): the status says it all.
    if (http_status) {
        if (http_status == 401 || http_status == 403) {
            return DeviceFailure::Unauthorized;
        }
        return http_status / 100 == 2 ? DeviceFailure::None : DeviceFailure::Other;
    }
    switch (sock_err) {
        case Err_success:
            return DeviceFailure::Other;
        // Connected, but no answer: response timeout, or the device dropped the connection.
        case Err_timeout:
        case Err_eof:
        case Err_reset:
        case Err_shutdown:
            return DeviceFailure::Timeout;
        // No connection: refused, unknown host, no route.
        default:
            return DeviceFailure::Unreachable;
    }
}

// Path and query of `url` ("/" when it has none), the URI a Digest response is computed over.
static string requestUri(const string &url) {
    size_t scheme = url.find("://");
    size_t path = url.find('/', scheme == string::npos ? 0 : scheme + 3);
    return path == string::npos ? "/" : url.substr(path);
}

// Picks the challenge this client can answer: Digest MD5 first, then Basic.
static string pickChallenge(const Parser &parser) {
    string basic;
    auto range = parser.getHeader().equal_range("WWW-Authenticate");
    for (auto it = range.first; it != range.second; ++it) {
        if (isDigestMd5(it->second)) {
            return it->second;
        }
        if (strncasecmp(it->second.c_str(), "Basic", 5) == 0) {
            basic = it->second;
        }
    }
    return basic;
}

static void sendGet(const string &url, const string &authorization, float timeout_sec,
                    const HttpRequester::HttpRequesterResult &cb) {
    auto requester = std::make_shared<HttpRequester>();
    requester->setMethod("GET");
    if (!authorization.empty()) {
        requester->addHeader("Authorization", authorization);
    }
    // Same lifetime as the webhooks: the callback holds the requester and drops it when it is done.
    requester->startRequester(url, [requester, cb](const SockException &ex, const Parser &res) mutable {
        onceToken token(nullptr, [&requester]() mutable { requester.reset(); });
        cb(ex, res);
    }, timeout_sec);
}

static VendorHttpAnswer toAnswer(const SockException &ex, const Parser &res) {
    VendorHttpAnswer answer;
    answer.status = atoi(res.status().c_str());
    answer.sockErr = answer.status ? Err_success : ex.getErrCode();
    if (!answer.status && !ex) {
        answer.sockErr = Err_other;
    }
    answer.body = res.content();
    return answer;
}

void vendorHttpGet(const string &url, const string &username, const string &password, float timeout_sec,
                   const OnVendorHttp &cb) {
    sendGet(url, "", timeout_sec, [url, username, password, timeout_sec, cb](const SockException &ex, const Parser &res) {
        VendorHttpAnswer answer = toAnswer(ex, res);
        if (answer.status != 401 || username.empty()) {
            cb(answer);
            return;
        }
        const string authorization = vendorHttpAuthorization(pickChallenge(res), "GET", requestUri(url), username,
                                                             password, MD5(makeRandStr(16)).hexdigest().substr(0, 16));
        if (authorization.empty()) {
            cb(answer);
            return;
        }
        sendGet(url, authorization, timeout_sec, [cb](const SockException &ex, const Parser &res) {
            cb(toAnswer(ex, res));
        });
    });
}

} // namespace managerkit
