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
#include "Util/SHA1.h"
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

// ---- Failure classification ----------------------------------------------

TEST(OnvifFailureTest, ClassifiesNetworkAndCredentialFailures) {
    EXPECT_EQ(DeviceFailure::Unauthorized, OnvifControl::classifyFailure(401, "Error 401 fault detected", false));
    EXPECT_EQ(DeviceFailure::AuthMethodUnsupported, OnvifControl::classifyFailure(401, "", true));
    EXPECT_EQ(DeviceFailure::Unauthorized, OnvifControl::classifyFailure(403, "", false));
    EXPECT_EQ(DeviceFailure::Unreachable, OnvifControl::classifyFailure(SOAP_TCP_ERROR, "connect failed", false));
    EXPECT_EQ(DeviceFailure::Timeout, OnvifControl::classifyFailure(SOAP_EOF, "End of file or no input", false));
    // WS-Security rejected inside a SOAP fault (camera in "digest&ws-username token" mode).
    EXPECT_EQ(DeviceFailure::Unauthorized,
              OnvifControl::classifyFailure(SOAP_FAULT, "SOAP 1.2 fault SOAP-ENV:Sender [ter:NotAuthorized]", false));
    EXPECT_EQ(DeviceFailure::Other,
              OnvifControl::classifyFailure(SOAP_FAULT, "SOAP 1.2 fault SOAP-ENV:Receiver [ter:ActionNotSupported]", false));
    EXPECT_EQ(DeviceFailure::Other, OnvifControl::classifyFailure(500, "HTTP Error", false));
}

TEST(OnvifFailureTest, TellsClockSkewFromBadCredentials) {
    OnvifControl::FailureContext ctx;
    ctx.soap_error = SOAP_FAULT;
    ctx.fault = "SOAP 1.2 fault SOAP-ENV:Sender [ter:NotAuthorized]";
    ctx.skew_threshold_sec = 30;
    // Clock not measured, or measured close: the credentials.
    EXPECT_EQ(DeviceFailure::Unauthorized, OnvifControl::classifyFailure(ctx));
    ctx.clock_known = true;
    ctx.clock_offset = 29;
    EXPECT_EQ(DeviceFailure::Unauthorized, OnvifControl::classifyFailure(ctx));
    // Far off, either direction: the clock.
    ctx.clock_offset = -30;
    EXPECT_EQ(DeviceFailure::ClockSkew, OnvifControl::classifyFailure(ctx));
    ctx.clock_offset = 600;
    EXPECT_EQ(DeviceFailure::ClockSkew, OnvifControl::classifyFailure(ctx));
    // A bare 401 to WS-Security is a rejected signature too...
    ctx.soap_error = 401;
    ctx.fault = "Error 401 fault detected";
    EXPECT_EQ(DeviceFailure::ClockSkew, OnvifControl::classifyFailure(ctx));
    // ...but not when it answered HTTP credentials: Digest does not depend on the clock.
    ctx.http_credentials_sent = true;
    EXPECT_EQ(DeviceFailure::Unauthorized, OnvifControl::classifyFailure(ctx));
    // The camera saying so is enough.
    OnvifControl::FailureContext expired;
    expired.soap_error = SOAP_FAULT;
    expired.fault = "SOAP 1.2 fault SOAP-ENV:Sender [wsse:MessageExpired]";
    EXPECT_EQ(DeviceFailure::ClockSkew, OnvifControl::classifyFailure(expired));
    // Not a credentials fault: the clock does not matter.
    OnvifControl::FailureContext other;
    other.soap_error = SOAP_FAULT;
    other.fault = "SOAP 1.2 fault SOAP-ENV:Receiver [ter:ActionNotSupported]";
    other.clock_known = true;
    other.clock_offset = 600;
    EXPECT_EQ(DeviceFailure::Other, OnvifControl::classifyFailure(other));
}

// ---- Fake camera ------------------------------------------------------------

// A blocking one-connection-at-a-time HTTP server answering like an ONVIF camera whose web
// service accepts only HTTP authentication. It never checks WS-Security.
class FakeCamera {
public:
    // SILENT: never answers anything but GetSystemDateAndTime.
    // WSSE: checks the WS-UsernameToken like a camera in "WS-UsernameToken" mode, against its own
    // clock (see setClockOffset()); a signature more than 10 s off is rejected like a bad password.
    enum Auth { NONE, DIGEST, BASIC, SILENT, WSSE };

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

    // Camera clock = server clock + offset. SetSystemDateAndTime changes it, as on a real camera.
    void setClockOffset(long offset) { _clock_offset = offset; }
    long clockOffset() const { return _clock_offset.load(); }
    // GetSystemDateAndTime needs the same credentials as everything else (not ONVIF compliant).
    void setTimeRequiresAuth(bool on) { _time_requires_auth = on; }
    // Whether responses carry an HTTP Date header (camera clock).
    void setSendDate(bool on) { _send_date = on; }

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
        static const char *actions[] = { "GetSystemDateAndTime", "SetSystemDateAndTime", "GetDeviceInformation",
                                         "GetNetworkInterfaces", "GetCapabilities", "GetServices" };
        for (auto action : actions) {
            if (body.find(action) != string::npos) {
                req.action = action;
                break;
            }
        }
        req.authorization = headerValue(head, "Authorization");
        // ONVIF requires GetSystemDateAndTime to work unauthenticated.
        const bool open_time = req.action == "GetSystemDateAndTime" && !_time_requires_auth;
        if (_auth == WSSE) {
            req.authorized = open_time || checkWsse(body);
        } else {
            req.authorized = _auth == NONE || _auth == SILENT || open_time || checkAuth(req.authorization, path);
        }

        if (_auth == SILENT && req.action != "GetSystemDateAndTime") {
            // Hold the request until the client gives up and closes the connection.
            while (!_stop && recv(fd, buf, sizeof(buf), 0) > 0) {
            }
            return;
        }
        string resp;
        if (!req.authorized && _auth == WSSE) {
            auto xml = envelope(R"(<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value>)"
                                R"(<SOAP-ENV:Subcode><SOAP-ENV:Value>ter:NotAuthorized</SOAP-ENV:Value></SOAP-ENV:Subcode>)"
                                R"(</SOAP-ENV:Code><SOAP-ENV:Reason><SOAP-ENV:Text xml:lang="en">Sender not Authorized)"
                                R"(</SOAP-ENV:Text></SOAP-ENV:Reason></SOAP-ENV:Fault>)");
            resp = "HTTP/1.1 400 Bad Request\r\n" + dateHeader() + "Content-Type: application/soap+xml\r\n"
                "Content-Length: " + to_string(xml.size()) + "\r\nConnection: close\r\n\r\n" + xml;
        } else if (!req.authorized) {
            resp = "HTTP/1.1 401 Unauthorized\r\n" + dateHeader() + challenge() + "Content-Length: 0\r\nConnection: close\r\n\r\n";
        } else if (req.action == "GetSystemDateAndTime") {
            time_t now = time(nullptr) + _clock_offset;
            struct tm t;
            gmtime_r(&now, &t);
            char buf_xml[512];
            snprintf(buf_xml, sizeof(buf_xml),
                     "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
                     "<tt:DateTimeType>Manual</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
                     "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone><tt:UTCDateTime>"
                     "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
                     "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
                     "</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
                     t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
            resp = ok(envelope(buf_xml));
        } else if (req.action == "SetSystemDateAndTime") {
            struct tm t;
            memset(&t, 0, sizeof(t));
            t.tm_year = atoi(tagValue(body, "Year").data()) - 1900;
            t.tm_mon = atoi(tagValue(body, "Month").data()) - 1;
            t.tm_mday = atoi(tagValue(body, "Day").data());
            t.tm_hour = atoi(tagValue(body, "Hour").data());
            t.tm_min = atoi(tagValue(body, "Minute").data());
            t.tm_sec = atoi(tagValue(body, "Second").data());
            _clock_offset = static_cast<long>(timegm(&t) - time(nullptr));
            resp = ok(envelope("<tds:SetSystemDateAndTimeResponse/>"));
        } else if (req.action == "GetDeviceInformation") {
            string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                         R"(<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope" )"
                         R"(xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><SOAP-ENV:Body>)"
                         R"(<tds:GetDeviceInformationResponse><tds:Manufacturer>Fake</tds:Manufacturer>)"
                         R"(<tds:Model>DigestOnly</tds:Model><tds:FirmwareVersion>1</tds:FirmwareVersion>)"
                         R"(<tds:SerialNumber>42</tds:SerialNumber><tds:HardwareId>1</tds:HardwareId>)"
                         R"(</tds:GetDeviceInformationResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>)";
            resp = ok(xml);
        } else {
            // Everything else fails, which ends OnvifControl::connect() at GetCapabilities.
            resp = "HTTP/1.1 500 Internal Server Error\r\n" + dateHeader() + "Content-Length: 0\r\nConnection: close\r\n\r\n";
        }
        {
            lock_guard<mutex> lk(_mtx);
            _requests.push_back(req);
        }
        send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    }

    static string envelope(const string &body) {
        return R"(<?xml version="1.0" encoding="UTF-8"?>)"
               R"(<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope" )"
               R"(xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema" )"
               R"(xmlns:ter="http://www.onvif.org/ver10/error"><SOAP-ENV:Body>)" + body + "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
    }

    string ok(const string &xml) {
        return "HTTP/1.1 200 OK\r\n" + dateHeader() + "Content-Type: application/soap+xml; charset=utf-8\r\n"
            "Content-Length: " + to_string(xml.size()) + "\r\nConnection: close\r\n\r\n" + xml;
    }

    string dateHeader() {
        if (!_send_date) {
            return "";
        }
        time_t now = time(nullptr) + _clock_offset;
        struct tm t;
        gmtime_r(&now, &t);
        char buf[64];
        strftime(buf, sizeof(buf), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &t);
        return buf;
    }

    // Text of the first element named `name` (any namespace prefix) at or after `from`.
    static string tagValue(const string &xml, const string &name, size_t from = 0) {
        size_t pos = from;
        while ((pos = xml.find(name, pos)) != string::npos) {
            const char before = pos ? xml[pos - 1] : ' ';
            const char after = pos + name.size() < xml.size() ? xml[pos + name.size()] : ' ';
            if ((before == '<' || before == ':') && (after == '>' || after == ' ')) {
                auto open_end = xml.find('>', pos);
                auto close = xml.find("</", open_end);
                if (open_end == string::npos || close == string::npos) {
                    return "";
                }
                return xml.substr(open_end + 1, close - open_end - 1);
            }
            pos += name.size();
        }
        return "";
    }

    // PasswordDigest = Base64(SHA1(nonce + Created + password)), Created within 10 s of the camera
    // clock. The UsernameToken's own Created, not the wsu:Timestamp one.
    bool checkWsse(const string &body) {
        auto token = body.find("UsernameToken");
        if (token == string::npos) {
            return false;
        }
        auto created = tagValue(body, "Created", token);
        auto nonce = decodeBase64(tagValue(body, "Nonce", token));
        if (tagValue(body, "Username", token) != _user || created.empty()) {
            return false;
        }
        if (tagValue(body, "Password", token) != encodeBase64(SHA1::encode_bin(nonce + created + _pass))) {
            return false;
        }
        struct tm t;
        memset(&t, 0, sizeof(t));
        if (!strptime(created.data(), "%Y-%m-%dT%H:%M:%S", &t)) {
            return false;
        }
        const long skew = static_cast<long>(timegm(&t) - (time(nullptr) + _clock_offset));
        return skew <= 10 && skew >= -10;
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
    atomic<long> _clock_offset { 0 };
    atomic<bool> _time_requires_auth { false };
    atomic<bool> _send_date { true };
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
    // The fake camera reports TZ UTC0; with the server also on UTC, setCameraTimeManual() sets the
    // camera to the server clock and the skew equals the raw offset.
    static void SetUpTestSuite() {
        setenv("TZ", "UTC", 1);
        tzset();
    }
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
    // HTTP 500: neither network nor credentials.
    EXPECT_EQ(DeviceFailure::Other, ctr.lastFailure());

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
    EXPECT_EQ(DeviceFailure::Unauthorized, ctr.lastFailure());
}

TEST_F(OnvifHttpAuthTest, BasicOnlyCameraIsRefusedByDefault) {
    FakeCamera cam(FakeCamera::BASIC, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(DeviceFailure::AuthMethodUnsupported, ctr.lastFailure());
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

TEST_F(OnvifHttpAuthTest, ClosedPortIsUnreachable) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (sockaddr *)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr *)&addr, &len);
    close(fd); // nothing listens there any more: connect is refused
    OnvifControl ctr("127.0.0.1:" + to_string(ntohs(addr.sin_port)), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(DeviceFailure::Unreachable, ctr.lastFailure());
}

TEST_F(OnvifHttpAuthTest, SilentCameraTimesOut) {
    // Connected, request never answered: OnvifControl gives up after its 10 s receive timeout.
    FakeCamera cam(FakeCamera::SILENT, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(DeviceFailure::Timeout, ctr.lastFailure());
}

// ---- Camera clock -------------------------------------------------------------

TEST_F(OnvifHttpAuthTest, CameraClockThatMovedIsMeasuredAndTheRequestSignedAgain) {
    FakeCamera cam(FakeCamera::WSSE, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect()); // fills the clock offset cache (camera in sync); GetCapabilities fails on purpose
    ASSERT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    EXPECT_FALSE(ctr.takeClockSyncWanted());

    // The camera clock jumps while the cached offset still says it is in sync.
    cam.setClockOffset(600);
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ("Fake", ctr.getDeviceInfo().manufacturer);
    auto info = cam.requestsFor("GetDeviceInformation");
    ASSERT_EQ(3u, info.size());
    EXPECT_FALSE(info[1].authorized); // signed with the stale offset
    EXPECT_TRUE(info[2].authorized);  // measured, signed again
    // The 500 of GetCapabilities, not the clock.
    EXPECT_EQ(DeviceFailure::Other, ctr.lastFailure());
    auto reading = ctr.clockReading();
    ASSERT_TRUE(reading.valid());
    EXPECT_NEAR(600, reading.offset_sec, 2);
    EXPECT_FALSE(reading.from_http_date);
    // Only flagged for the maintenance cycle: nothing set the camera clock.
    EXPECT_TRUE(cam.requestsFor("SetSystemDateAndTime").empty());
    EXPECT_TRUE(ctr.takeClockSyncWanted());
    EXPECT_FALSE(ctr.takeClockSyncWanted());
}

TEST_F(OnvifHttpAuthTest, WrongPasswordWithTheClockInSyncIsNotSentAgain) {
    FakeCamera cam(FakeCamera::WSSE, "admin", "pw");
    OnvifControl ctr(cam.host(), "admin", "wrong");
    EXPECT_FALSE(ctr.connect());
    // Measuring the clock changes nothing, so the same signature is not sent a second time.
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    EXPECT_EQ(DeviceFailure::Unauthorized, ctr.lastFailure());
    EXPECT_FALSE(ctr.takeClockSyncWanted());
}

TEST_F(OnvifHttpAuthTest, RejectedWithTheClockFarOffIsClockSkew) {
    FakeCamera cam(FakeCamera::WSSE, "admin", "pw");
    cam.setClockOffset(600);
    // Signed with the camera clock and still rejected: cannot tell, the clock is reported.
    OnvifControl ctr(cam.host(), "admin", "wrong");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    EXPECT_EQ(DeviceFailure::ClockSkew, ctr.lastFailure());
    auto reading = ctr.clockReading();
    ASSERT_TRUE(reading.valid());
    EXPECT_NEAR(600, reading.offset_sec, 2);
}

TEST_F(OnvifHttpAuthTest, ClockReadFromTheHttpDateWhenTheCameraRefusesToTellTheTime) {
    FakeCamera cam(FakeCamera::WSSE, "admin", "pw");
    cam.setClockOffset(600);
    cam.setTimeRequiresAuth(true);
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    auto info = cam.requestsFor("GetDeviceInformation");
    ASSERT_FALSE(info.empty());
    EXPECT_TRUE(info[0].authorized); // signed with the clock of the refusal's Date header
    EXPECT_EQ("Fake", ctr.getDeviceInfo().manufacturer);
}

TEST_F(OnvifHttpAuthTest, NoWayToReadTheClockStaysUnauthorized) {
    FakeCamera cam(FakeCamera::WSSE, "admin", "pw");
    cam.setClockOffset(600);
    cam.setTimeRequiresAuth(true);
    cam.setSendDate(false);
    OnvifControl ctr(cam.host(), "admin", "pw");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(1u, cam.requestsFor("GetDeviceInformation").size());
    EXPECT_EQ(DeviceFailure::Unauthorized, ctr.lastFailure());
}

TEST_F(OnvifHttpAuthTest, CameraClockIsSetWithoutAConnection) {
    FakeCamera cam(FakeCamera::WSSE, "admin", "pw");
    cam.setClockOffset(600);
    OnvifControl ctr(cam.host(), "admin", "pw"); // never connected
    ctr.setCameraTimeManual();
    auto set = cam.requestsFor("SetSystemDateAndTime");
    ASSERT_EQ(1u, set.size());
    EXPECT_TRUE(set[0].authorized); // signed with the camera clock it had
    EXPECT_NEAR(0, cam.clockOffset(), 2);
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
