#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Control/OnvifDeviceControl.h"
#include "Onvif/SoapUtil.h"
#include "Util/MD5.h"
#include "Util/NoticeCenter.h"
#include "Util/base64.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace {

void setAllowBasic(bool allow) {
    mINI::Instance()[OnvifAuth::kAllowHttpBasicAuth] = allow ? 1 : 0;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
}

map<string, string> parseParams(const string &str) {
    map<string, string> ret;
    for (auto &item : split(str, ",")) {
        auto pos = item.find('=');
        if (pos == string::npos) {
            continue;
        }
        auto key = trim(item.substr(0, pos));
        auto val = trim(item.substr(pos + 1), " \"");
        ret[key] = val;
    }
    return ret;
}

// ---- RFC 2617 vector and challenge selection ------------------------------

TEST(OnvifHttpAuthorizationTest, DigestMatchesRfc2617Example) {
    vector<string> challenges = { R"(Digest realm="testrealm@host.com", qop="auth,auth-int", )"
                                  R"(nonce="dcd98b7102dd2f0e8b11d0f600bfb0c093", opaque="5ccc069c403ebaf9f0171e9517f40e41")" };
    auto auth = SoapUtil::makeHttpAuthorization(challenges, "GET", "/dir/index.html", "Mufasa", "Circle Of Life",
                                                "0a4f113b", false);
    ASSERT_EQ(0u, auth.find("Digest "));
    auto params = parseParams(auth.substr(7));
    EXPECT_EQ("6629fae49393a05397450978507c4ef1", params["response"]);
    EXPECT_EQ("auth", params["qop"]);
    EXPECT_EQ("00000001", params["nc"]);
    EXPECT_EQ("0a4f113b", params["cnonce"]);
    EXPECT_EQ("5ccc069c403ebaf9f0171e9517f40e41", params["opaque"]);
    EXPECT_EQ("/dir/index.html", params["uri"]);
}

TEST(OnvifHttpAuthorizationTest, DigestWithoutQopUsesRfc2069Response) {
    vector<string> challenges = { R"(Digest realm="cam", nonce="abc")" };
    auto auth = SoapUtil::makeHttpAuthorization(challenges, "POST", "/onvif/device_service", "admin", "pw", "x", false);
    auto ha1 = MD5("admin:cam:pw").hexdigest();
    auto ha2 = MD5("POST:/onvif/device_service").hexdigest();
    auto params = parseParams(auth.substr(7));
    EXPECT_EQ(MD5(ha1 + ":abc:" + ha2).hexdigest(), params["response"]);
    EXPECT_EQ(0u, params.count("qop"));
    EXPECT_EQ(0u, params.count("nc"));
}

TEST(OnvifHttpAuthorizationTest, PrefersDigestOverBasic) {
    vector<string> challenges = { R"(Basic realm="cam")", R"(Digest realm="cam", nonce="n", qop="auth")" };
    auto auth = SoapUtil::makeHttpAuthorization(challenges, "POST", "/", "u", "p", "c", true);
    EXPECT_EQ(0u, auth.find("Digest "));
}

TEST(OnvifHttpAuthorizationTest, BasicOnlyWhenAllowed) {
    vector<string> challenges = { R"(Basic realm="cam")" };
    EXPECT_EQ("", SoapUtil::makeHttpAuthorization(challenges, "POST", "/", "u", "p", "c", false));
    EXPECT_EQ("Basic " + encodeBase64("u:p"), SoapUtil::makeHttpAuthorization(challenges, "POST", "/", "u", "p", "c", true));
}

TEST(OnvifHttpAuthorizationTest, RefusesChallengesItCannotAnswer) {
    vector<string> challenges = { R"(Digest realm="cam", nonce="n", qop="auth-int")",
                                  R"(Digest realm="cam", nonce="n", algorithm=SHA-512-256)",
                                  R"(Digest realm="cam")" };
    EXPECT_EQ("", SoapUtil::makeHttpAuthorization(challenges, "POST", "/", "u", "p", "c", false));
}

// ---- Fake camera ------------------------------------------------------------

// A blocking one-connection-at-a-time HTTP server answering like an ONVIF camera whose web
// service accepts only HTTP authentication. It never checks WS-Security.
class FakeCamera {
public:
    enum Auth { NONE, DIGEST, BASIC };

    struct Request {
        string action;
        string authorization;
        bool authorized = false;
    };

    FakeCamera(Auth auth, string user, string pass, int nonce_uses = 1000)
        : _auth(auth), _user(std::move(user)), _pass(std::move(pass)), _nonce_uses(nonce_uses) {
        _fd = socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(_fd, (sockaddr *)&addr, sizeof(addr));
        listen(_fd, 16);
        socklen_t len = sizeof(addr);
        getsockname(_fd, (sockaddr *)&addr, &len);
        _port = ntohs(addr.sin_port);
        _thread = std::thread([this]() { run(); });
    }

    ~FakeCamera() {
        _stop = true;
        shutdown(_fd, SHUT_RDWR);
        close(_fd);
        _thread.join();
    }

    string host() const { return "127.0.0.1:" + to_string(_port); }

    vector<Request> requests() {
        lock_guard<mutex> lk(_mtx);
        return _requests;
    }

    vector<Request> requestsFor(const string &action) {
        vector<Request> ret;
        for (auto &r : requests()) {
            if (r.action == action) {
                ret.push_back(r);
            }
        }
        return ret;
    }

private:
    void run() {
        while (!_stop) {
            int fd = accept(_fd, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            serve(fd);
            close(fd);
        }
    }

    static string headerValue(const string &head, const string &key) {
        auto lower = head;
        transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto lkey = key;
        transform(lkey.begin(), lkey.end(), lkey.begin(), ::tolower);
        auto pos = lower.find("\r\n" + lkey + ":");
        if (pos == string::npos) {
            return "";
        }
        pos += lkey.size() + 3;
        auto end = head.find("\r\n", pos);
        return trim(head.substr(pos, end - pos));
    }

    void serve(int fd) {
        string data;
        char buf[4096];
        size_t head_end;
        while ((head_end = data.find("\r\n\r\n")) == string::npos) {
            auto n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                return;
            }
            data.append(buf, n);
        }
        auto head = data.substr(0, head_end);
        size_t content_len = atoi(headerValue(head, "Content-Length").data());
        while (data.size() < head_end + 4 + content_len) {
            auto n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            data.append(buf, n);
        }
        auto body = data.substr(head_end + 4);
        auto path = split(head.substr(0, head.find("\r\n")), " ")[1];

        Request req;
        static const char *actions[] = { "GetSystemDateAndTime", "GetDeviceInformation", "GetNetworkInterfaces",
                                         "GetCapabilities", "GetServices" };
        for (auto action : actions) {
            if (body.find(action) != string::npos) {
                req.action = action;
                break;
            }
        }
        req.authorization = headerValue(head, "Authorization");
        // ONVIF requires GetSystemDateAndTime to work unauthenticated.
        req.authorized = _auth == NONE || req.action == "GetSystemDateAndTime" || checkAuth(req.authorization, path);

        string resp;
        if (!req.authorized) {
            resp = "HTTP/1.1 401 Unauthorized\r\n" + challenge() + "Content-Length: 0\r\nConnection: close\r\n\r\n";
        } else if (req.action == "GetDeviceInformation") {
            string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                         R"(<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope" )"
                         R"(xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><SOAP-ENV:Body>)"
                         R"(<tds:GetDeviceInformationResponse><tds:Manufacturer>Fake</tds:Manufacturer>)"
                         R"(<tds:Model>DigestOnly</tds:Model><tds:FirmwareVersion>1</tds:FirmwareVersion>)"
                         R"(<tds:SerialNumber>42</tds:SerialNumber><tds:HardwareId>1</tds:HardwareId>)"
                         R"(</tds:GetDeviceInformationResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>)";
            resp = "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml; charset=utf-8\r\nContent-Length: "
                + to_string(xml.size()) + "\r\nConnection: close\r\n\r\n" + xml;
        } else {
            // Everything else fails, which ends OnvifControl::connect() at GetCapabilities.
            resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        }
        {
            lock_guard<mutex> lk(_mtx);
            _requests.push_back(req);
        }
        send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    }

    string challenge() {
        if (_auth == BASIC) {
            return "WWW-Authenticate: Basic realm=\"cam\"\r\n";
        }
        return "WWW-Authenticate: Digest realm=\"cam\", qop=\"auth\", nonce=\"" + nonce() + "\", opaque=\"op\""
            + (_stale ? ", stale=TRUE" : "") + "\r\n";
    }

    string nonce() const { return "nonce" + to_string(_nonce_gen); }

    bool checkAuth(const string &auth, const string &path) {
        _stale = false;
        if (_auth == BASIC) {
            return auth == "Basic " + encodeBase64(_user + ":" + _pass);
        }
        if (auth.find("Digest ") != 0) {
            return false;
        }
        auto p = parseParams(auth.substr(7));
        if (p["nonce"] != nonce()) {
            _stale = true;
            return false;
        }
        auto ha1 = MD5(_user + ":cam:" + _pass).hexdigest();
        auto ha2 = MD5("POST:" + p["uri"]).hexdigest();
        auto expect = MD5(ha1 + ":" + p["nonce"] + ":" + p["nc"] + ":" + p["cnonce"] + ":auth:" + ha2).hexdigest();
        if (p["uri"] != path || p["response"] != expect || p["opaque"] != "op" || p["qop"] != "auth") {
            return false;
        }
        // Expire the nonce after it has been used _nonce_uses times.
        if (++_nonce_used >= _nonce_uses) {
            ++_nonce_gen;
            _nonce_used = 0;
        }
        return true;
    }

private:
    Auth _auth;
    string _user;
    string _pass;
    int _nonce_uses;
    int _nonce_gen = 1;
    int _nonce_used = 0;
    bool _stale = false;
    int _fd = -1;
    uint16_t _port = 0;
    atomic<bool> _stop { false };
    thread _thread;
    mutex _mtx;
    vector<Request> _requests;
};

bool anyBasic(const vector<FakeCamera::Request> &reqs) {
    for (auto &r : reqs) {
        if (r.authorization.find("Basic") == 0) {
            return true;
        }
    }
    return false;
}

class OnvifHttpAuthTest : public ::testing::Test {
protected:
    void SetUp() override { setAllowBasic(false); }
    void TearDown() override { setAllowBasic(false); }
};

// ---- gSOAP client (OnvifControl) ------------------------------------------

TEST_F(OnvifHttpAuthTest, CameraWithoutHttpAuthSeesNoAuthorizationHeader) {
    FakeCamera cam(FakeCamera::NONE, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect()); // GetCapabilities fails on purpose
    EXPECT_EQ("Fake", ctr.getDeviceInfo().manufacturer);
    auto reqs = cam.requests();
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    for (auto &r : reqs) {
        EXPECT_EQ("", r.authorization) << r.action;
    }
}

TEST_F(OnvifHttpAuthTest, DigestOnlyCameraConnectsAndLaterCallsAuthenticateUpFront) {
    FakeCamera cam(FakeCamera::DIGEST, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect()); // GetCapabilities fails on purpose, after authenticating
    EXPECT_EQ("Fake", ctr.getDeviceInfo().manufacturer);

    auto info = cam.requestsFor("GetDeviceInformation");
    ASSERT_EQ(2u, info.size());
    EXPECT_FALSE(info[0].authorized);
    EXPECT_EQ("", info[0].authorization);
    EXPECT_TRUE(info[1].authorized);

    // Calls after the first challenge carry Digest without being challenged again.
    auto caps = cam.requestsFor("GetCapabilities");
    ASSERT_EQ(1u, caps.size());
    EXPECT_TRUE(caps[0].authorized);
    auto nics = cam.requestsFor("GetNetworkInterfaces");
    ASSERT_EQ(1u, nics.size());
    EXPECT_TRUE(nics[0].authorized);
    // Same nonce, higher count: no new challenge was needed. http_da may skip counts, which
    // RFC 2617 allows; it only has to grow.
    auto first = parseParams(info[1].authorization.substr(7));
    auto later = parseParams(nics[0].authorization.substr(7));
    EXPECT_EQ(first["nonce"], later["nonce"]);
    EXPECT_GT(strtoul(later["nc"].data(), nullptr, 16), strtoul(first["nc"].data(), nullptr, 16));
    EXPECT_FALSE(anyBasic(cam.requests()));
}

TEST_F(OnvifHttpAuthTest, ExpiredNonceIsRenewedAndTheCallSentOnce) {
    // Every nonce is good for one request only.
    FakeCamera cam(FakeCamera::DIGEST, "admin", "pw", 1);
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ("Fake", ctr.getDeviceInfo().manufacturer);
    for (auto action : { "GetNetworkInterfaces", "GetCapabilities" }) {
        auto reqs = cam.requestsFor(action);
        ASSERT_EQ(2u, reqs.size()) << action;
        EXPECT_FALSE(reqs[0].authorized) << action;
        EXPECT_TRUE(reqs[1].authorized) << action;
    }
}

TEST_F(OnvifHttpAuthTest, WrongPasswordStopsAfterOneRetry) {
    FakeCamera cam(FakeCamera::DIGEST, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "wrong");
    EXPECT_FALSE(ctr.connect());
    auto reqs = cam.requestsFor("GetDeviceInformation");
    ASSERT_EQ(2u, reqs.size());
    EXPECT_FALSE(reqs[1].authorized);
    EXPECT_NE(string::npos, reqs[1].authorization.find("Digest "));
    EXPECT_EQ(0u, cam.requestsFor("GetCapabilities").size());
}

TEST_F(OnvifHttpAuthTest, BasicOnlyCameraIsRefusedByDefault) {
    FakeCamera cam(FakeCamera::BASIC, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    EXPECT_FALSE(anyBasic(cam.requests()));
}

TEST_F(OnvifHttpAuthTest, BasicOnlyCameraWorksWhenAllowed) {
    setAllowBasic(true);
    FakeCamera cam(FakeCamera::BASIC, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ("Fake", ctr.getDeviceInfo().manufacturer);
    auto info = cam.requestsFor("GetDeviceInformation");
    ASSERT_EQ(2u, info.size());
    EXPECT_TRUE(info[1].authorized);
    auto caps = cam.requestsFor("GetCapabilities");
    ASSERT_EQ(1u, caps.size());
    EXPECT_TRUE(caps[0].authorized);
}

// ---- HTTP client (SoapUtil) -------------------------------------------------

struct SoapResult {
    mutex mtx;
    condition_variable cv;
    bool done = false;
    bool ok = false;
    int http_code = 0;
};

void getDeviceInformation(const string &url, const string &user, const string &pass, SoapResult &res) {
    SoapUtil::sendGetDeviceInformation(url, user, pass, [&res](const SoapObject &, const SoapErr &err) {
        lock_guard<mutex> lk(res.mtx);
        res.ok = !err;
        res.http_code = err.httpCode();
        res.done = true;
        res.cv.notify_all();
    });
    unique_lock<mutex> lk(res.mtx);
    ASSERT_TRUE(res.cv.wait_for(lk, chrono::seconds(10), [&res]() { return res.done; }));
}

TEST_F(OnvifHttpAuthTest, SoapUtilAnswersDigestChallenge) {
    FakeCamera cam(FakeCamera::DIGEST, "admin", "pw");
    SoapResult res;
    getDeviceInformation("http://" + cam.host() + "/onvif/device_service", "admin", "pw", res);
    EXPECT_TRUE(res.ok);
    auto reqs = cam.requestsFor("GetDeviceInformation");
    ASSERT_EQ(2u, reqs.size());
    EXPECT_TRUE(reqs[1].authorized);
}

TEST_F(OnvifHttpAuthTest, SoapUtilWithoutCredentialsKeepsThe401) {
    FakeCamera cam(FakeCamera::DIGEST, "admin", "pw");
    SoapResult res;
    getDeviceInformation("http://" + cam.host() + "/onvif/device_service", "", "", res);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(401, res.http_code);
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
}

TEST_F(OnvifHttpAuthTest, SoapUtilRefusesBasicByDefault) {
    FakeCamera cam(FakeCamera::BASIC, "admin", "pw");
    SoapResult res;
    getDeviceInformation("http://" + cam.host() + "/onvif/device_service", "admin", "pw", res);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    EXPECT_FALSE(anyBasic(cam.requests()));
}

} // namespace
