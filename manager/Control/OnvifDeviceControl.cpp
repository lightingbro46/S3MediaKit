#include "wsdd.nsmap" //Namespaces
#include "plugin/wsseapi.h" //WS-Sercurity
#include "plugin/httpda.h" //HTTP Digest
#include "Util/logger.h"
#include "Util/util.h"
#include "Util/onceToken.h"
#include "Common/config.h"
#include "Extension/Plugin.h"
#include "Onvif/SoapUtil.h"
#include "OnvifDeviceControl.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace OnvifCtrl {
#define ONVIF_CTRL_FIELD "onvif."
const string kClockOffsetTtlSec = ONVIF_CTRL_FIELD"clockOffsetTtlSec";
const string kProfileResyncIntervalSec = ONVIF_CTRL_FIELD"profileResyncIntervalSec";
const string kClockSyncIntervalSec = ONVIF_CTRL_FIELD"clockSyncIntervalSec";
const string kClockSkewThresholdSec = ONVIF_CTRL_FIELD"clockSkewThresholdSec";
const string kClockSyncEnable = ONVIF_CTRL_FIELD"clockSyncEnable";

static onceToken token([]() {
    // How long a camera's clock offset stays usable before it is measured again.
    // Camera clocks drift by seconds per day, so five minutes is far shorter than
    // the drift needs to matter, while removing a GetSystemDateAndTime round trip
    // from every single ONVIF operation. 0 or less restores the old behaviour.
    mINI::Instance()[kClockOffsetTtlSec] = 300;
    // The maintenance cycle re-reads a connected camera's media profiles this often
    // when nothing is waiting to be pushed to it. Only needed to notice a config
    // changed on the camera's own web page; a config this server is trying to set
    // is retried on every cycle regardless. <= 0 re-reads on every cycle.
    mINI::Instance()[kProfileResyncIntervalSec] = 900;
    // How often the maintenance cycle pushes the server clock back to a connected camera
    // (SetSystemDateAndTime, and only when the two differ by 10 s or more). A camera clock drifts
    // slowly, so once an hour corrects it long before the drift matters, while the round trip this
    // costs no longer runs on every cycle. <= 0 checks the clock on every cycle.
    mINI::Instance()[kClockSyncIntervalSec] = 3600;
    // A camera rejecting our WS-Security signature looks the same whether the password is wrong or
    // its clock is off. Off by this much or more, the failure is reported as a clock problem.
    mINI::Instance()[kClockSkewThresholdSec] = 30;
    // Whether the maintenance cycle may set a camera clock at all (only ever from that cycle,
    // never from an API command).
    mINI::Instance()[kClockSyncEnable] = 1;
});
} // namespace OnvifCtrl

namespace managerkit {

// ---- HTTP authentication ---------------------------------------------------
// A camera whose ONVIF web service is set to "Digest" (rather than "Digest&WS-UsernameToken")
// ignores the WS-Security header and answers every request with 401 and a WWW-Authenticate
// challenge. gSOAP's http_da plugin answers Digest challenges; the small plugin below records
// which challenges a response carried, because neither gSOAP nor http_da keep that.

static const char kHttpAuthChallengeId[] = "S3-ONVIF-HTTP-AUTH/1.0";

struct HttpAuthChallenge {
    int (*fparsehdr)(struct soap *, const char *, const char *);
    bool digest;    // a Digest challenge http_da can answer
    bool basic;     // a Basic challenge
    char realm[256]; // realm of the Digest challenge
    time_t date;    // HTTP Date header of the last response (camera clock, UTC), 0 = none
};

// RFC 7231 IMF-fixdate, e.g. "Sun, 04 Oct 2026 08:12:03 GMT". 0 when it does not parse.
static time_t parseHttpDate(const char *val) {
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    const char *end = strptime(val, "%a, %d %b %Y %H:%M:%S", &tm_val);
    return end ? timegm(&tm_val) : 0;
}

static HttpAuthChallenge *lookupHttpAuthChallenge(struct soap *soap) {
    return static_cast<HttpAuthChallenge *>(soap_lookup_plugin(soap, kHttpAuthChallengeId));
}

static int httpAuthParseHeader(struct soap *soap, const char *key, const char *val) {
    auto data = lookupHttpAuthChallenge(soap);
    if (!data) {
        return SOAP_PLUGIN_ERROR;
    }
    if (key && val && !soap_tag_cmp(key, "WWW-Authenticate")) {
        if (!soap_tag_cmp(val, "Digest *")) {
            const char *alg = soap_http_header_attribute(soap, val + 7, "algorithm");
            // http_da cannot compute SHA-512-256; keep the first challenge it can answer.
            if (!data->digest && (!alg || soap_tag_cmp(alg, "SHA-512-256*"))) {
                const char *realm = soap_http_header_attribute(soap, val + 7, "realm");
                if (realm) {
                    data->digest = true;
                    snprintf(data->realm, sizeof(data->realm), "%s", realm);
                    // http_da only takes a challenge whose realm differs from soap->authrealm,
                    // which a Basic header with the same realm earlier in this response has set.
                    soap->authrealm = nullptr;
                }
            }
        } else if (!soap_tag_cmp(val, "Basic*")) {
            data->basic = true;
        }
    } else if (key && val && !soap_tag_cmp(key, "Date")) {
        data->date = parseHttpDate(val);
    }
    return data->fparsehdr(soap, key, val);
}

static void httpAuthPluginDelete(struct soap *, struct soap_plugin *p) {
    free(p->data);
}

static int httpAuthPlugin(struct soap *soap, struct soap_plugin *p, void *) {
    auto data = static_cast<HttpAuthChallenge *>(calloc(1, sizeof(HttpAuthChallenge)));
    if (!data) {
        return SOAP_EOM;
    }
    data->fparsehdr = soap->fparsehdr;
    soap->fparsehdr = httpAuthParseHeader;
    p->id = kHttpAuthChallengeId;
    p->data = data;
    p->fcopy = nullptr;
    p->fdelete = httpAuthPluginDelete;
    return SOAP_OK;
}

static bool qopHasToken(const char *qop, const char *token) {
    if (!qop) {
        return false;
    }
    for (auto &item : split(qop, ",")) {
        if (strcasecmp(trim(item, " \t\"").data(), token) == 0) {
            return true;
        }
    }
    return false;
}

// HTTP credentials of one soap context, learnt from its 401 challenges. gSOAP clears
// soap->userid/passwd/authrealm after every response, so apply() must run before every request.
struct OnvifHttpAuth {
    enum Mode { NONE, DIGEST, BASIC };

    // Registers the plugins; must be called once, right after soap_new(). Registration order
    // matters: the challenge recorder has to see a header before http_da does.
    static void attach(struct soap *soap) {
        soap_register_plugin(soap, http_da);
        soap_register_plugin(soap, httpAuthPlugin);
    }

    OnvifHttpAuth(struct soap *soap, std::string ip) : _soap(soap), _ip(std::move(ip)) {
        memset(&_info, 0, sizeof(_info));
    }

    // Must run while _soap is still alive: it releases what http_da_save() allocated.
    ~OnvifHttpAuth() { releaseInfo(); }

    void apply() {
        _basic_refused = false;
        if (auto data = lookupHttpAuthChallenge(_soap)) {
            data->digest = data->basic = false;
            data->realm[0] = '\0';
        }
        _sent = _mode != NONE;
        if (_mode == DIGEST) {
            http_da_restore(_soap, &_info);
        } else if (_mode == BASIC) {
            _soap->userid = _user.c_str();
            _soap->passwd = _pass.c_str();
        }
    }

    // After a 401: take the challenge of that response. Returns true when the request should be
    // sent once more with the new credentials.
    bool learn(const std::string &user, const std::string &pass) {
        auto data = lookupHttpAuthChallenge(_soap);
        if (!data || user.empty()) {
            return false;
        }
        if (data->digest) {
            // Save into a fresh store before releasing the old one: releasing clears the
            // plugin's nonce, which http_da_save() copies.
            struct http_da_info fresh;
            memset(&fresh, 0, sizeof(fresh));
            http_da_save(_soap, &fresh, data->realm, user.c_str(), pass.c_str());
            if (!fresh.nonce) {
                http_da_release(_soap, &fresh);
                return false;
            }
            releaseInfo();
            _info = fresh;
            // auth-int hashes the body before installSoapHook() rewrites its timestamps, so the
            // digest would no longer match. Plain auth signs only method and URI.
            if (qopHasToken(_info.qop, "auth")) {
                free(_info.qop);
                _info.qop = soap_strdup(nullptr, "auth");
            }
            if (_mode != DIGEST) {
                InfoL << "Onvif device " << _ip << " requires HTTP Digest authentication, realm " << data->realm;
            }
            _mode = DIGEST;
            return true;
        }
        if (data->basic) {
            GET_CONFIG(bool, allow_basic, OnvifAuth::kAllowHttpBasicAuth);
            if (!allow_basic) {
                _basic_refused = true;
                WarnL << "Onvif device " << _ip << " only offers HTTP Basic authentication, which sends the password "
                      << "in clear text; refused (set " << OnvifAuth::kAllowHttpBasicAuth << "=1 to allow it)";
                return false;
            }
            if (_mode != BASIC) {
                WarnL << "Onvif device " << _ip << " uses HTTP Basic authentication: the password is sent in clear text";
            }
            releaseInfo();
            _user = user;
            _pass = pass;
            _mode = BASIC;
            return true;
        }
        return false;
    }

    // Whether the last request carried HTTP credentials, i.e. a 401 to it means bad credentials.
    bool sentCredentials() const { return _sent; }

    // Whether the last 401 only offered HTTP Basic and onvif.allowHttpBasicAuth refused it.
    bool basicRefused() const { return _basic_refused; }

    // auth-int is only kept when the camera offers nothing else.
    bool signsBody() const { return _mode == DIGEST && qopHasToken(_info.qop, "auth-int"); }

private:
    void releaseInfo() {
        if (_mode == DIGEST) {
            http_da_release(_soap, &_info);
        }
        memset(&_info, 0, sizeof(_info));
        if (_mode != BASIC) {
            return;
        }
        _soap->userid = _soap->passwd = nullptr;
        _user.clear();
        _pass.clear();
    }

private:
    struct soap *_soap;
    std::string _ip;
    Mode _mode = NONE;
    bool _sent = false;
    bool _basic_refused = false;
    struct http_da_info _info;
    std::string _user;
    std::string _pass;
};

static uint64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The usual per-request timeout, also the cap while a deadline is set.
static const int kSoapTimeoutSec = 10;

void OnvifControl::setDeadline(uint64_t steady_ms) {
    _deadline_ms = steady_ms;
    _deadline_exceeded = false;
}

bool OnvifControl::applyDeadline(struct soap *soap) {
    if (_deadline_ms == 0) {
        return true;
    }
    const uint64_t now = steadyNowMs();
    if (now >= _deadline_ms) {
        _deadline_exceeded = true;
        soap_set_sender_error(soap, "Deadline exceeded", "The discovery deadline has passed", SOAP_TCP_ERROR);
        return false;
    }
    const uint64_t left_ms = std::min<uint64_t>(_deadline_ms - now, kSoapTimeoutSec * 1000);
    // Negative = microseconds. transfer_timeout bounds the whole request (a camera trickling its
    // answer byte by byte resets recv_timeout forever), but gSOAP 2.8.133 scales its negative
    // form wrongly, so it gets whole seconds, rounded up.
    soap->connect_timeout = soap->recv_timeout = soap->send_timeout = -static_cast<int>(left_ms * 1000);
    soap->transfer_timeout = static_cast<int>((left_ms + 999) / 1000);
    return true;
}

void OnvifControl::checkDeadline() {
    // A request cut by the shortened timeouts fails a little before the deadline itself.
    static const uint64_t kSlackMs = 200;
    if (_deadline_ms != 0 && steadyNowMs() + kSlackMs >= _deadline_ms) {
        _deadline_exceeded = true;
    }
}

int OnvifControl::callWithAuth(const std::function<int()> &call) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    // reportError() sets it again if this call fails.
    _last_failure = static_cast<int>(DeviceFailure::None);
    _measured_this_call = false;
    // Every attempt is checked against the deadline: the retries below would otherwise each wait
    // the full timeout again.
    auto attempt = [&]() {
        if (!applyDeadline(_m_soap)) {
            return static_cast<int>(SOAP_TCP_ERROR);
        }
        const int r = call();
        if (r != SOAP_OK) {
            checkDeadline();
        }
        return r;
    };
    if (_http_auth) {
        // Also covers a second call after one setCredentials(): the first response cleared the
        // HTTP credentials gSOAP keeps.
        _http_auth->apply();
    }
    int ret = attempt();
    if (_deadline_exceeded) {
        return ret;
    }
    if (ret == 401 && _http_auth && _http_auth->learn(_strUsername, _strPassword)) {
        if (!setCredentials()) {
            return ret;
        }
        _http_auth->apply();
        ret = attempt();
    } else if (ret != SOAP_OK && rejectedSignature(ret) && resignWithFreshClock()) {
        _http_auth->apply();
        ret = attempt();
    }
    return ret;
}

bool OnvifControl::rejectedSignature(int soap_error) {
    if (!_http_auth || _http_auth->sentCredentials() || _http_auth->basicRefused()) {
        return false;
    }
    // A bare 401 (no challenge we could answer) or a SOAP fault naming the credentials.
    if (soap_error == 401) {
        return true;
    }
    std::ostringstream oss;
    soap_stream_fault(_m_soap, oss);
    return isAuthOrTimestampFault(oss.str());
}

bool OnvifControl::resignWithFreshClock() {
    // Same threshold as setCredentials() uses to sign with the camera clock at all.
    static const time_t kResignDeltaSec = 5;
    const time_t signed_offset = _signed_offset;
    if (!measureCameraClock()) {
        return false;
    }
    const time_t offset = _clock_offset.load(std::memory_order_relaxed);
    const time_t delta = offset > signed_offset ? offset - signed_offset : signed_offset - offset;
    if (delta < kResignDeltaSec) {
        // Signing again would send the same signature: most likely the password is wrong, and
        // another attempt would only count against the camera's lockout.
        return false;
    }
    InfoL << "Onvif device " << _strDeviceIp << " rejected a request signed " << signed_offset
          << " s off the local clock; its clock is " << offset << " s off, signing again";
    return setCredentials();
}

DeviceClockReading OnvifControl::clockReading() const {
    DeviceClockReading reading;
    reading.server_time = _reading_server_time.load(std::memory_order_acquire);
    reading.offset_sec = _reading_offset.load(std::memory_order_relaxed);
    reading.from_http_date = _reading_from_date.load(std::memory_order_relaxed);
    return reading;
}

OnvifControl::OnvifControl(std::string strDeviceIp, std::string strUsername, std::string strPassword)
    : DeviceControl(), _strDeviceIp(strDeviceIp), _strUsername(strUsername), _strPassword(strPassword) {}

OnvifControl::~OnvifControl() {
    disconnect();
}

// gSOAP generates every repeated element as a vector of raw pointers, and the project does not
// enable SOAP_XML_STRICT, so a device that omits an element yields an empty vector (or a null
// entry) instead of a parse error. Indexing such a vector is undefined behaviour.
template <typename T>
static T *firstOrNull(const std::vector<T *> &v) {
    return v.empty() ? nullptr : v.front();
}

// Copy a 2D range (pan/tilt). XRange/YRange are "required" per schema but generated as raw
// pointers, so they can still be null.
static void copyPanTiltRange(const tt__Space2DDescription *space, float &min_x, float &max_x,
                             float &min_y, float &max_y) {
    if (!space) {
        return;
    }
    if (space->XRange) {
        min_x = space->XRange->Min;
        max_x = space->XRange->Max;
    }
    if (space->YRange) {
        min_y = space->YRange->Min;
        max_y = space->YRange->Max;
    }
}

// Copy a 1D range (zoom). tt__Space1DDescription has no YRange: min and max both come from XRange.
static void copyZoomRange(const tt__Space1DDescription *space, float &min_v, float &max_v) {
    if (!space || !space->XRange) {
        return;
    }
    min_v = space->XRange->Min;
    max_v = space->XRange->Max;
}

static std::string ResRangeToString(const std::vector<std::pair<int, int>>& v)
{
    std::ostringstream oss;

    bool first = true;
    for (const auto& p : v)
    {
        if (!first) oss << " ";
        oss << p.first << "x" << p.second;
        first = false;
    }

    return oss.str();
}

static int parse_timezone_offset_seconds(const std::string& tz) {
    if (tz.empty()) return 0;

    int sign = 1;
    size_t pos = std::string::npos;

    // ========================
    // 1. Find the '+' or '-' sign (if any)
    // ========================
    size_t plus_pos  = tz.find('+');
    size_t minus_pos = tz.find('-');

    if (plus_pos != std::string::npos) {
        sign = -1;
        pos = plus_pos + 1;
    } else if (minus_pos != std::string::npos) {
        sign = +1;
        pos = minus_pos + 1;
    } else {
        // ========================
        // 2. If there is no '+' or '-' → find the first digi
        // ========================
        for (size_t i = 0; i < tz.size(); ++i) {
            if (std::isdigit(tz[i])) {
                pos = i;
                break;
            }
        }
        if (pos == std::string::npos) return 0;

        sign = -1;
    }

    // The remaining part should contain only digits and ':' character
    std::string num = tz.substr(pos);

    int hour = 0, min = 0, sec = 0;

    // ========================
    // 3. If ':' is present
    // ========================
    if (num.find(':') != std::string::npos) {
        size_t p1 = num.find(':');
        size_t p2 = num.find(':', p1 + 1);

        hour = std::atoi(num.substr(0, p1).c_str());

        if (p2 != std::string::npos) {
            // HH:MM:SS
            min = std::atoi(num.substr(p1 + 1, p2 - p1 - 1).c_str());
            sec = std::atoi(num.substr(p2 + 1).c_str());
        } else {
            // HH:MM
            min = std::atoi(num.substr(p1 + 1).c_str());
        }
    }
    // ========================
    // 4. If ':' is not present → formats: H, HH, HMM, HHMM, HMMSS, HHMMSS
    // ========================
    else {
        int len = num.length();

        if (len <= 2) {
            // HH
            hour = std::atoi(num.c_str());
        }
        else if (len == 3) {
            // HMM (730 → 7:30)
            hour = std::atoi(num.substr(0, 1).c_str());
            min  = std::atoi(num.substr(1, 2).c_str());
        }
        else if (len == 4) {
            // HHMM (0730 → 07:30)
            hour = std::atoi(num.substr(0, 2).c_str());
            min  = std::atoi(num.substr(2, 2).c_str());
        }
        else if (len == 5) {
            // HMMSS (53000 → 5:30:00)
            hour = std::atoi(num.substr(0, 1).c_str());
            min  = std::atoi(num.substr(1, 2).c_str());
            sec  = std::atoi(num.substr(3, 2).c_str());
        }
        else if (len == 6) {
            // HHMMSS (073000 → 07:30:00)
            hour = std::atoi(num.substr(0, 2).c_str());
            min  = std::atoi(num.substr(2, 2).c_str());
            sec  = std::atoi(num.substr(4, 2).c_str());
        }
        else {
            return 0;
        }
    }

    return sign * (hour * 3600 + min * 60 + sec);
}

static int get_system_offset_seconds() {
    time_t now = time(nullptr);

    struct tm gmt{};
    struct tm local{};

    gmtime_r(&now, &gmt);
    localtime_r(&now, &local);

    return (int)difftime(mktime(&local), mktime(&gmt));
}

static time_t build_camera_utc(const std::string& camera_tz, const time_t& time_point) {
    int offset_camera = parse_timezone_offset_seconds(camera_tz);
    int offset_system = get_system_offset_seconds();

    return time_point + (offset_system - offset_camera);
}

static bool parseIsDataPresentFromXml(const char* raw, const std::string& key, std::string& value)
{
    if (!raw) return false;

    std::string xml(raw);
    std::string nameKey = "Name=\"" + key + "\"";

    size_t pos = xml.find(nameKey);
    if (pos == std::string::npos)
        return false;

    // search Value="..."
    size_t valuePos = xml.find("Value=\"", pos);
    if (valuePos == std::string::npos)
        return false;

    valuePos += 7; // skip 'Value="'

    size_t endPos = xml.find("\"", valuePos);
    if (endPos == std::string::npos)
        return false;

    value = xml.substr(valuePos, endPos - valuePos);
    return true;
}

bool OnvifControl::connect() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_m_soap == nullptr) {
        _m_soap = soap_new();
        _m_soap->connect_timeout = _m_soap->recv_timeout = _m_soap->send_timeout = 10; // timeout connection for 10 seconds
        soap_register_plugin(_m_soap, soap_wsse);
        OnvifHttpAuth::attach(_m_soap);
        _http_auth.reset(new OnvifHttpAuth(_m_soap, _strDeviceIp));

        _proxyDevice = new DeviceBindingProxy(_m_soap);
    }

    _lastConnectError = ConnectError::OTHER;
    if (!getDeviceInformation()) {
        _lastConnectError = classifyConnectError(_m_soap->error, _soapErrMsg);
        if (lastFailure() == DeviceFailure::ClockSkew) {
            // Rejected only because the camera clock is far off, which the caller fixes: not a
            // verdict on the credentials, so no longer retry pause and no other protocol tried.
            _lastConnectError = ConnectError::OTHER;
        }
        disconnect();
        return false;
    }

    getNetworkInterfaces(); // non-critical

    if (!getDeviceCapabilities()) {
        disconnect();
        return false;
    }

    if (!getMediaProfiles()) {
        disconnect();
        return false;
    }

    getImagingCapabilities(); // non-critical

    getRelayOutputCapabilities(); // non-critical

    getRecordingSummary();

    getStorageConfiguration();

    _lastConnectError = ConnectError::NONE;
    return true;
}

ConnectError OnvifControl::classifyConnectError(int soap_error, const std::string &fault) {
    switch (soap_error) {
        case SOAP_OK:
            return ConnectError::NONE;
        case SOAP_EOF:
        case SOAP_TCP_ERROR:
        case SOAP_SSL_ERROR:
            return ConnectError::UNREACHABLE;
        case 401:
        case 403:
            return ConnectError::AUTH_FAILED;
        case 404:
        case 405:
        case SOAP_NO_TAG:
        case SOAP_TAG_MISMATCH:
        case SOAP_SYNTAX_ERROR:
            return ConnectError::NOT_THIS_PROTOCOL;
        default:
            break;
    }
    // A SOAP fault means the device does speak ONVIF.
    return isAuthOrTimestampFault(fault) ? ConnectError::AUTH_FAILED : ConnectError::OTHER;
}

bool OnvifControl::probeConnection() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_m_soap || !_proxyDevice) {
        return false;
    }
    // getDeviceInformation() overwrites _deviceInfo, so keep the identity we
    // connected to and compare afterwards.
    const OnvifDeviceInfo before = _deviceInfo;
    if (!getDeviceInformation()) {
        return false;
    }
    if (before != _deviceInfo) {
        // Another camera answers at this address now: its capabilities, profiles and
        // PTZ spaces are all different, so the cached ones must not be reused.
        WarnL << "Onvif device " << _strDeviceIp << " reports a different identity than the one we connected to";
        return false;
    }
    return true;
}

bool OnvifControl::refreshMediaProfiles() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_m_soap || !_proxyMedia || !_proxyMedia2) {
        return false;
    }
    return getMediaProfiles();
}

void OnvifControl::disconnect() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    // Delete proxies BEFORE freeing the shared soap context.
    // Proxy destructors may call soap_done() on the context they were created
    // with; freeing the context first causes a use-after-free crash.
    delete _proxyDevice;  _proxyDevice = nullptr;
    delete _proxyMedia;   _proxyMedia = nullptr;
    delete _proxyMedia2;  _proxyMedia2 = nullptr;
    delete _proxyImaging; _proxyImaging = nullptr;
    delete _proxyPTZ;     _proxyPTZ = nullptr;
    delete _proxySearch;  _proxySearch = nullptr;
    delete _proxyReplay;  _proxyReplay = nullptr;
    delete _proxyRecording;  _proxyRecording = nullptr;
    // Releases what http_da allocated, so it has to go before the context it belongs to.
    _http_auth.reset();
    if (_m_soap != nullptr) {
        soap_destroy(_m_soap);
        soap_end(_m_soap);
        soap_free(_m_soap);
        _m_soap = nullptr;
    }
}

bool OnvifControl::getDeviceInformation() {
    // get device info and print
    _strDeviceUrl = "http://" + _strDeviceIp + "/onvif/device_service";
    _proxyDevice->soap_endpoint = _strDeviceUrl.c_str();
    TraceL << "Onvif device url: " << _proxyDevice->soap_endpoint;
    _tds__GetDeviceInformation *GetDeviceInformation = soap_new__tds__GetDeviceInformation(_m_soap);
    _tds__GetDeviceInformationResponse GetDeviceInformationResponse;
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyDevice->GetDeviceInformation(GetDeviceInformation, GetDeviceInformationResponse); })) {
        reportError();
        return false;
    }

    _deviceInfo.manufacturer = GetDeviceInformationResponse.Manufacturer;
    _deviceInfo.model = GetDeviceInformationResponse.Model;
    _deviceInfo.firmwareVersion = GetDeviceInformationResponse.FirmwareVersion;
    _deviceInfo.serialNumber = GetDeviceInformationResponse.SerialNumber;
    _deviceInfo.hardwareId = GetDeviceInformationResponse.HardwareId;
    TraceL << "Manufacturer:     " << _deviceInfo.manufacturer;
    TraceL << "Model:            " << _deviceInfo.model;
    TraceL << "FirmwareVersion:  " << _deviceInfo.firmwareVersion;
    TraceL << "SerialNumber:     " << _deviceInfo.serialNumber;
    TraceL << "HardwareId:       " << _deviceInfo.hardwareId;
    return true;
}

tt__Profile *OnvifControl::pickPTZProfile(const _trt__GetProfilesResponse &resp) {
    // Historically only Profiles[0] was considered. Keep preferring it, but fall back to the
    // first profile that actually carries a PTZ configuration instead of giving up on PTZ for
    // devices that do not list it first.
    for (size_t i = 0; i < resp.Profiles.size(); ++i) {
        tt__Profile *profile = resp.Profiles[i];
        if (profile && profile->PTZConfiguration) {
            return profile;
        }
    }
    return nullptr;
}

void OnvifControl::fillPTZSpaces(const _tptz__GetConfigurationOptionsResponse &resp, OnvifPTZProfile &out) {
    tt__PTZSpaces *spaces = resp.PTZConfigurationOptions ? resp.PTZConfigurationOptions->Spaces : nullptr;
    if (!spaces) {
        TraceL << "Device reported no PTZ spaces";
        return;
    }

    // Absolute. The move capability follows pan/tilt only: zoom is read separately because a
    // pan/tilt-only head is legal ONVIF and used to crash here.
    if (tt__Space2DDescription *pan_tilt = firstOrNull(spaces->AbsolutePanTiltPositionSpace)) {
        out.isAbsMoveEnable = true;
        copyPanTiltRange(pan_tilt, out.absMinPan, out.absMaxPan, out.absMinTilt, out.absMaxTilt);
    }
    copyZoomRange(firstOrNull(spaces->AbsoluteZoomPositionSpace), out.absMinZoom, out.absMaxZoom);
    if (out.isAbsMoveEnable) {
        TraceL << "AbsoluteMove supported: Pan[" << out.absMinPan << "~" << out.absMaxPan
               << "] Tilt[" << out.absMinTilt << "~" << out.absMaxTilt
               << "] Zoom[" << out.absMinZoom << "~" << out.absMaxZoom << "]";
    }

    // Continuous.
    if (tt__Space2DDescription *pan_tilt = firstOrNull(spaces->ContinuousPanTiltVelocitySpace)) {
        out.isConsMoveEnable = true;
        copyPanTiltRange(pan_tilt, out.consMinPan, out.consMaxPan, out.consMinTilt, out.consMaxTilt);
    }
    copyZoomRange(firstOrNull(spaces->ContinuousZoomVelocitySpace), out.consMinZoom, out.consMaxZoom);
    if (out.isConsMoveEnable) {
        TraceL << "ContinuousMove supported: Pan[" << out.consMinPan << "~" << out.consMaxPan
               << "] Tilt[" << out.consMinTilt << "~" << out.consMaxTilt
               << "] Zoom[" << out.consMinZoom << "~" << out.consMaxZoom << "]";
    }

    // Relative.
    if (tt__Space2DDescription *pan_tilt = firstOrNull(spaces->RelativePanTiltTranslationSpace)) {
        out.isRelMoveEnable = true;
        copyPanTiltRange(pan_tilt, out.relMinPan, out.relMaxPan, out.relMinTilt, out.relMaxTilt);
    }
    copyZoomRange(firstOrNull(spaces->RelativeZoomTranslationSpace), out.relMinZoom, out.relMaxZoom);
    if (out.isRelMoveEnable) {
        TraceL << "RelativeMove supported: Pan[" << out.relMinPan << "~" << out.relMaxPan
               << "] Tilt[" << out.relMinTilt << "~" << out.relMaxTilt
               << "] Zoom[" << out.relMinZoom << "~" << out.relMaxZoom << "]";
    }
}

bool OnvifControl::getDeviceCapabilities() {
    // get device capabilities and print media
    _tds__GetCapabilities *GetCapabilities = soap_new__tds__GetCapabilities(_m_soap);
    _tds__GetCapabilitiesResponse GetCapabilitiesResponse;
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyDevice->GetCapabilities(GetCapabilities, GetCapabilitiesResponse); })) {
        reportError();
    }

    if (!GetCapabilitiesResponse.Capabilities || !GetCapabilitiesResponse.Capabilities->Media) {
        reportError();
        return false;
    }

    if (GetCapabilitiesResponse.Capabilities->Media != nullptr) {
        _strMediaUrl = GetCapabilitiesResponse.Capabilities->Media->XAddr;
        int indexFooter = _strMediaUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            _strMediaUrl.erase(0, indexFooter);
            _strMediaUrl.insert(0, "http://" + _strDeviceIp);
        }
        TraceL << "Media XAddr:  " << _strMediaUrl;
        _proxyMedia = new MediaBindingProxy(_m_soap);
        _proxyMedia->soap_endpoint = _strMediaUrl.c_str();
        _proxyMedia2 = new Media2BindingProxy(_m_soap);
        _proxyMedia2->soap_endpoint = _strMediaUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->Imaging != nullptr) {
        _strImagingUrl = GetCapabilitiesResponse.Capabilities->Imaging->XAddr;
        int indexFooter = _strImagingUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            _strImagingUrl.erase(0, indexFooter);
            _strImagingUrl.insert(0, "http://" + _strDeviceIp);
        }
        TraceL << "Imaging XAddr:  " << _strImagingUrl << endl;
        _proxyImaging = new ImagingBindingProxy(_m_soap);
        _proxyImaging->soap_endpoint = _strImagingUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->Device != nullptr &&
        GetCapabilitiesResponse.Capabilities->Device->IO != nullptr &&
        GetCapabilitiesResponse.Capabilities->Device->IO->RelayOutputs != nullptr &&
        *GetCapabilitiesResponse.Capabilities->Device->IO->RelayOutputs > 0) {
        TraceL << "RelayOutputs: " << *GetCapabilitiesResponse.Capabilities->Device->IO->RelayOutputs;
    }

    if (GetCapabilitiesResponse.Capabilities->PTZ != nullptr) {
        _strPTZUrl = GetCapabilitiesResponse.Capabilities->PTZ->XAddr;
        int indexFooter = _strPTZUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            _strPTZUrl.erase(0, indexFooter);
            _strPTZUrl.insert(0, "http://" + _strDeviceIp);
        }
        TraceL << "PTZ XAddr:  " << _strPTZUrl << endl;

        _proxyPTZ = new PTZBindingProxy(_m_soap);
        _proxyPTZ->soap_endpoint = _strPTZUrl.c_str();

        _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
        _trt__GetProfilesResponse GetProfilesResponse;
        if (!setCredentials()) {
            return false;
        }

        if (callWithAuth([&] { return _proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse); })) {
            reportError();
            return false;
        }
        tt__Profile *ptz_profile = pickPTZProfile(GetProfilesResponse);
        if (ptz_profile) {
            TraceL << "MediaProfile token for PTZ:" << ptz_profile->token;
            _ptzProfile.strMediaProfileToken = ptz_profile->token;

            _tptz__GetConfigurationOptions *GetConfigurationOptions = soap_new__tptz__GetConfigurationOptions(_m_soap);
            _tptz__GetConfigurationOptionsResponse GetConfigurationOptionsResponse;
            GetConfigurationOptions->ConfigurationToken = ptz_profile->PTZConfiguration->token;
            if (!setCredentials()) {
                return false;
            }

            if (callWithAuth([&] { return _proxyPTZ->GetConfigurationOptions(GetConfigurationOptions, GetConfigurationOptionsResponse); })) {
                reportError();
                return false;
            }

            fillPTZSpaces(GetConfigurationOptionsResponse, _ptzProfile);

            if (_ptzProfile.isAbsMoveEnable) {
                getPTZPresets();
            } else {
                _ptzProfile.isPresetEnable = false;
                _ptzProfile.isHomePresetEnable = false;
                _ptzProfile.homePresetToken = "";
                _ptzProfile.presetMap.clear();
                TraceL << "Preset is not supported since AbsoluteMove is not supported";
            }
        }
    }
    if (GetCapabilitiesResponse.Capabilities->Extension != nullptr && GetCapabilitiesResponse.Capabilities->Extension->Search != nullptr) {
        thread_local string strUrl;
        strUrl = GetCapabilitiesResponse.Capabilities->Extension->Search->XAddr;
        int indexFooter = strUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            strUrl.erase(0, indexFooter);
            strUrl.insert(0, "http://" + _strDeviceIp);
        }

        _proxySearch = new SearchBindingProxy(_m_soap);
        _proxySearch->soap_endpoint = strUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->Extension != nullptr && GetCapabilitiesResponse.Capabilities->Extension->Replay != nullptr) {
        thread_local string strUrl;
        strUrl = GetCapabilitiesResponse.Capabilities->Extension->Replay->XAddr;
        int indexFooter = strUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            strUrl.erase(0, indexFooter);
            strUrl.insert(0, "http://" + _strDeviceIp);
        }

        _proxyReplay = new ReplayBindingProxy(_m_soap);
        _proxyReplay->soap_endpoint = strUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->Extension != nullptr && GetCapabilitiesResponse.Capabilities->Extension->Recording != nullptr) {
        thread_local string strUrl;
        strUrl = GetCapabilitiesResponse.Capabilities->Extension->Recording->XAddr;
        int indexFooter = strUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            strUrl.erase(0, indexFooter);
            strUrl.insert(0, "http://" + _strDeviceIp);
        }

        _proxyRecording = new RecordingBindingProxy(_m_soap);
        _proxyRecording->soap_endpoint = strUrl.c_str();
    }
    return true;
}

bool OnvifControl::getAudioCapabilities() {
    _audioOutputProfile.supportedDecodingEncodings.clear();
    // TODO: Check whether AudioOutputConfiguration is exposed. If not, add it
    if (_proxyMedia == nullptr || _audioOutputProfile.profileToken.empty() || _audioOutputProfile.decoderToken.empty()) {
        return false;
    }

    _trt__GetAudioDecoderConfigurationOptions *GetAudioDecoderConfigurationOptions = soap_new__trt__GetAudioDecoderConfigurationOptions(_m_soap);
    GetAudioDecoderConfigurationOptions->ConfigurationToken = &_audioOutputProfile.decoderToken;
    GetAudioDecoderConfigurationOptions->ProfileToken = &_audioOutputProfile.profileToken;
    _trt__GetAudioDecoderConfigurationOptionsResponse GetAudioDecoderConfigurationOptionsResponse;
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyMedia->GetAudioDecoderConfigurationOptions(GetAudioDecoderConfigurationOptions, GetAudioDecoderConfigurationOptionsResponse); })) {
        reportError();
        return false;
    }

    if (GetAudioDecoderConfigurationOptionsResponse.Options == nullptr) {
        return false;
    }

    if (GetAudioDecoderConfigurationOptionsResponse.Options->AACDecOptions != nullptr) {
        // No standard ONVIF ACC test case exists for backchannel -> Skip for now
        // _audioOutputProfile.supportedDecodingEncodings.push_back("ACC");
    }
    if (GetAudioDecoderConfigurationOptionsResponse.Options->G711DecOptions != nullptr) {
        _audioOutputProfile.supportedDecodingEncodings.push_back("G711");
    }
    if (GetAudioDecoderConfigurationOptionsResponse.Options->G726DecOptions != nullptr) {        
        _audioOutputProfile.supportedDecodingEncodings.push_back("G726");
    }

    return true;
}

bool OnvifControl::getMediaProfiles() {
    if (_proxyMedia == nullptr || _proxyMedia2 == nullptr) {
        WarnL << "Unknown proxyMedia";
        return false;
    }

    _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
    _trt__GetProfilesResponse GetProfilesResponse;
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse); })) {
        reportError();
        return false;
    }

    // note: reset media profile list every time when get media profiles since profile token may change after device reboot
    _mediaProfile.clear();
    _imageProfile.videoSourceToken.clear();
    _audioInputProfile.clear();
    _audioOutputProfile.clear();

    for (const auto &profile : GetProfilesResponse.Profiles) {
        if (!profile || profile->token.empty()) {
            continue;
        }
        TraceL << "========================================";
        TraceL << "Read a profile with MediaProfile token: " << profile->token;

        OnvifMediaProfile _profile;
        _profile.token = profile->token;
        if (profile->VideoSourceConfiguration && profile->VideoEncoderConfiguration) {
            // profile has video source
            _profile.hasVideo = true;

            // Capture the first video source token for Imaging service operations
            if (_imageProfile.videoSourceToken.empty()) {
                _imageProfile.videoSourceToken = profile->VideoSourceConfiguration->SourceToken;
                TraceL << "Video source token: " << _imageProfile.videoSourceToken;
            }

            ns1__GetConfiguration *GetVideoConfig  = soap_new_ns1__GetConfiguration(_m_soap);
            GetVideoConfig->ProfileToken = &profile->token;
            GetVideoConfig->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
            _ns1__GetVideoEncoderConfigurationsResponse GetVideoConfigResponse;
            if (!setCredentials()) {
                return false;
            }
            if (!callWithAuth([&] { return _proxyMedia2->GetVideoEncoderConfigurations(GetVideoConfig, GetVideoConfigResponse); })) {
                TraceL << "Get video encoder configuration for token " << profile->token << ", configurations size = " << GetVideoConfigResponse.Configurations.size();
                if (GetVideoConfigResponse.Configurations.empty()) {
                    WarnL << "No video encoder configuration returned for profile: " << profile->token;
                    continue;
                }

                auto tokenConfig = GetVideoConfigResponse.Configurations.front();
                if (!tokenConfig) {
                    continue;
                }
                _profile.vcodec = tokenConfig->Encoding;
                TraceL << "Video Codec: " << _profile.vcodec;
                _profile.bitrate = tokenConfig->RateControl ? tokenConfig->RateControl->BitrateLimit : 0;
                TraceL << "Bitrate Limit: " << _profile.bitrate;
                _profile.fps = tokenConfig->RateControl ? tokenConfig->RateControl->FrameRateLimit : 0;
                TraceL << "Bitrate Limit: " << _profile.fps;
                _profile.width = tokenConfig->Resolution ? tokenConfig->Resolution->Width : 0;
                TraceL << "Width: " << _profile.width;
                _profile.height = tokenConfig->Resolution ? tokenConfig->Resolution->Height : 0;
                TraceL << "Height: " << _profile.height;
                _profile.quality = tokenConfig->Quality;
                TraceL << "Quality: " << _profile.quality;

                // get video configuration option in profile
                ns1__GetConfiguration *GetVideoConfigOptions  = soap_new_ns1__GetConfiguration(_m_soap);
                _ns1__GetVideoEncoderConfigurationOptionsResponse GetVideoConfigOptionResponse;
                GetVideoConfigOptions->ProfileToken = &profile->token;
                GetVideoConfigOptions->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
                if (!setCredentials()) {
                    reportError();
                    return false;
                }
                if (callWithAuth([&] { return _proxyMedia2->GetVideoEncoderConfigurationOptions(GetVideoConfigOptions, GetVideoConfigOptionResponse); })) {
                    reportError();
                    return false;
                }
                for (const auto &Option : GetVideoConfigOptionResponse.Options) {
                    if (!Option) {
                        continue;
                    }
                    VideoEncoderConfigOption cfg_option;
                    for (const auto *resolution : Option->ResolutionsAvailable) {
                        if (!resolution) {
                            continue;
                        }
                        cfg_option.ResolutionsAvailable.emplace_back(resolution->Width, resolution->Height);
                    }

                    if (!Option->BitrateRange || !Option->QualityRange) {
                        WarnL << "Incomplete video encoder options returned for profile: " << profile->token
                              << ", encoding: " << Option->Encoding;
                        continue;
                    }

                    cfg_option.BitRateRange = bitrateRangeToString(Option->BitrateRange->Min, Option->BitrateRange->Max);
                    cfg_option.QualityRange = std::make_pair(Option->QualityRange->Min, Option->QualityRange->Max);
                    if (Option->FrameRatesSupported) {
                        cfg_option.FrameRatesSupported = *Option->FrameRatesSupported;
                    }
                    cfg_option.FPSEditable = _profile.fps != 0 && !cfg_option.FrameRatesSupported.empty();
                    cfg_option.bitrateEditable = _profile.bitrate != 0 && Option->BitrateRange->Min != Option->BitrateRange->Max;
                    cfg_option.resolutionEditable = cfg_option.ResolutionsAvailable.size() > 1;
                    _profile.vEncoderOptionMap[Option->Encoding] = cfg_option;
                }

                _profile.videoEncEditable = _profile.vEncoderOptionMap.size() > 1;
                bool editable = false;
                for (const auto &it : _profile.vEncoderOptionMap) {
                    editable = it.second.bitrateEditable || it.second.FPSEditable || it.second.resolutionEditable;
                }
                _profile.videoConfigEditable = editable || _profile.videoEncEditable;
            } else {
                reportError();
            }
        }

        // Audio input and audio output (backchannel) are detected independently: a speaker may
        // expose an output without a microphone, and a camera a microphone without an output.
        if (profile->AudioSourceConfiguration && profile->AudioEncoderConfiguration) {
            if (_audioInputProfile.token.empty()) {
                // Audio inputs (sources)
                _audioInputProfile.token = profile->AudioSourceConfiguration->token;
                TraceL << "Audio input token: " << _audioInputProfile.token;
                _audioInputProfile.sourceToken = profile->AudioSourceConfiguration->SourceToken;
                TraceL << "Audio input source token: " << _audioInputProfile.sourceToken;
                _audioInputProfile.name = profile->AudioSourceConfiguration->Name;
                TraceL << "Audio input name: " << _audioInputProfile.name;

                _trt__GetAudioSources *GetAudioSources = soap_new__trt__GetAudioSources(_m_soap);
                _trt__GetAudioSourcesResponse GetAudioSourcesResponse;
                if (setCredentials()) {
                    if (callWithAuth([&] { return _proxyMedia->GetAudioSources(GetAudioSources, GetAudioSourcesResponse); }) == SOAP_OK) {
                        _audioInputProfile.enable = !GetAudioSourcesResponse.AudioSources.empty();
                        for (const auto &audioSource : GetAudioSourcesResponse.AudioSources) {
                            if (audioSource->token == _audioInputProfile.sourceToken) {
                                _audioInputProfile.channelNo = audioSource->Channels ? audioSource->Channels : 0;
                                break;
                            }
                        }
                        TraceL << "Channel No: " << _audioInputProfile.channelNo;
                    } else {
                        reportError();
                    }
                } else {
                    reportError();
                }


                switch (profile->AudioEncoderConfiguration->Encoding) {
                    case tt__AudioEncoding__G711: _audioInputProfile.acodec = "G711"; break;
                    case tt__AudioEncoding__G726: _audioInputProfile.acodec = "G726"; break;
                    case tt__AudioEncoding__AAC: _audioInputProfile.acodec = "AAC"; break;
                    default: _audioInputProfile.acodec = "UNKNOWN"; break;
                }

                if (_audioInputProfile.acodec != "UNKNOWN") {
                    TraceL << "Audio Codec: " << _audioInputProfile.acodec;
                    _audioInputProfile.sampleBit = profile->AudioEncoderConfiguration->Bitrate ? to_string(profile->AudioEncoderConfiguration->Bitrate) : "";
                    TraceL << "Sample Bit Limit: " << _audioInputProfile.sampleBit;
                    _audioInputProfile.sampleRate = profile->AudioEncoderConfiguration->SampleRate ? to_string(profile->AudioEncoderConfiguration->SampleRate) : "";
                    TraceL << "Sample Rate Limit: " << _audioInputProfile.sampleRate;
                    _audioInputProfile.encoderToken = profile->AudioEncoderConfiguration->token;
                    TraceL << "Encoder token: " << _audioInputProfile.encoderToken;
                    _audioInputProfile.enable = true;
                    // profile has audio source
                    _profile.hasAudio = true;
                } else {
                    _audioInputProfile.clear();
                }
            }
        }

        if (profile->Extension && profile->Extension->AudioOutputConfiguration &&
            profile->Extension->AudioDecoderConfiguration) {
            if (_audioOutputProfile.token.empty()) {
                // Audio outputs
                _audioOutputProfile.profileToken = profile->token;
                _audioOutputProfile.token = profile->Extension->AudioOutputConfiguration->token;
                TraceL << "Audio output token: " << _audioOutputProfile.token;
                _audioOutputProfile.name = profile->Extension->AudioOutputConfiguration->Name;
                TraceL << "Audio output name: " << _audioOutputProfile.name;
                _audioOutputProfile.outputToken = profile->Extension->AudioOutputConfiguration->OutputToken;
                TraceL << "Audio output outputToken: " << _audioOutputProfile.outputToken;
                _audioOutputProfile.outputLevel = profile->Extension->AudioOutputConfiguration->OutputLevel;
                TraceL << "Output Level: " << _audioOutputProfile.outputLevel;
                _audioOutputProfile.sendPrimacy = profile->Extension->AudioOutputConfiguration->SendPrimacy ? *profile->Extension->AudioOutputConfiguration->SendPrimacy : "";
                TraceL << "sendPrimacy: " << _audioOutputProfile.sendPrimacy;
                _audioOutputProfile.decoderToken = profile->Extension->AudioDecoderConfiguration->token;
                TraceL << "Decoder token: " << _audioOutputProfile.decoderToken;
                _audioOutputProfile.enable = !_audioOutputProfile.token.empty() && !_audioOutputProfile.outputToken.empty() && !_audioOutputProfile.decoderToken.empty();
            }
        }

        if (_profile.hasVideo) {
            // profile has video stream, get stream uri
            _trt__GetStreamUri *GetStreamUri = soap_new__trt__GetStreamUri(_m_soap);
            _trt__GetStreamUriResponse GetStreamUriResponse;
            GetStreamUri->ProfileToken = profile->token;
            GetStreamUri->StreamSetup = soap_new_tt__StreamSetup(_m_soap, -1);
            GetStreamUri->StreamSetup->Stream = tt__StreamType__RTP_Unicast;
            GetStreamUri->StreamSetup->Transport = soap_new_tt__Transport(_m_soap, -1);
            GetStreamUri->StreamSetup->Transport->Protocol = tt__TransportProtocol__RTSP;
            if (!setCredentials()) {
                return false;
            }

            if (callWithAuth([&] { return _proxyMedia->GetStreamUri(GetStreamUri, GetStreamUriResponse); })) {
                reportError();
                return false;
            }

            if (!GetStreamUriResponse.MediaUri) {
                WarnL << "No media URI returned for profile: " << profile->token;
                continue;
            }

            TraceL << "Uri: " << GetStreamUriResponse.MediaUri->Uri;
            _profile.url = GetStreamUriResponse.MediaUri->Uri;
            if (_profile.url.empty()) {
                continue;
            }
        }

        // add media profile to manager
        _mediaProfile.push_back(_profile);
    }

    // Now that the decoder token is known, ask the device which formats it can decode. Done on
    // every refresh because the audio output profile (and its formats) was just reset above.
    getAudioCapabilities(); // non-critical

    return true;
}

tt__VideoEncoder2Configuration* OnvifControl::getConfigVideoEncoder2ByToken(const std::string& profileToken)
{
    if (_proxyMedia == nullptr || _proxyMedia2 == nullptr) {
        WarnL << "Unknown proxyMedia";
        return nullptr;
    }

    _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
    _trt__GetProfilesResponse GetProfilesResponse;
    if (!setCredentials()) {
        return nullptr;
    }

    if (callWithAuth([&] { return _proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse); })) {
        reportError();
        return nullptr;
    }

    for (auto profile : GetProfilesResponse.Profiles)
    {
        if (profile->token == profileToken)
        {
            ns1__GetConfiguration *GetVideoConfig  = soap_new_ns1__GetConfiguration(_m_soap);
            GetVideoConfig->ProfileToken = &profile->token;
            GetVideoConfig->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
            _ns1__GetVideoEncoderConfigurationsResponse GetVideoConfigResponse;
            if (!setCredentials()) {
                return nullptr;
            }
            if (callWithAuth([&] { return _proxyMedia2->GetVideoEncoderConfigurations(GetVideoConfig, GetVideoConfigResponse); })) {
                reportError();
                return nullptr;
            }
            return GetVideoConfigResponse.Configurations[0];
        }
    }

    return nullptr;
}

bool OnvifControl::setVideoEncoderConfigByToken(const std::string& token, const VideoEncoderConfig& vConfigNew)
{
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    auto config = getConfigVideoEncoder2ByToken(token);
    if (!config) {
        return false;
    }

    tt__VideoEncoder2Configuration* configToSet = soap_new_tt__VideoEncoder2Configuration(_m_soap);
    *configToSet = *config;

    configToSet->Encoding = vConfigNew.vcodec;

    if (!configToSet->Resolution)
        configToSet->Resolution = soap_new_tt__VideoResolution2(_m_soap);
    configToSet->Resolution->Width = vConfigNew.width;
    configToSet->Resolution->Height = vConfigNew.height;

    if (!configToSet->RateControl)
        configToSet->RateControl = soap_new_tt__VideoRateControl2(_m_soap);

    configToSet->RateControl->FrameRateLimit = vConfigNew.fps;
    configToSet->RateControl->BitrateLimit = vConfigNew.bitrate;

    _ns1__SetVideoEncoderConfiguration* setReq = soap_new__ns1__SetVideoEncoderConfiguration(_m_soap);
    ns1__SetConfigurationResponse setResp;

    setReq->Configuration = configToSet;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyMedia2->SetVideoEncoderConfiguration(setReq, setResp); }))
    {
        reportError();
        return false;
    }

    _ns1__GetStreamUri *GetStreamUri = soap_new__ns1__GetStreamUri(_m_soap);
    _ns1__GetStreamUriResponse GetStreamUriResponse;
    GetStreamUri->ProfileToken = token;
    GetStreamUri->Protocol = "RTSP";
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyMedia2->GetStreamUri(GetStreamUri, GetStreamUriResponse); })) {
        reportError();
        return false;
    }

    InfoL << "The new config has been sent to the camera, current URI: " << GetStreamUriResponse.Uri;

    return true;
}

bool OnvifControl::getVideoEncoderConfigByToken(const std::string& token, VideoEncoderConfig& vConfig) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    auto config = getConfigVideoEncoder2ByToken(token);
    if (!config) {
        return false;
    }

    vConfig.vcodec = config->Encoding;
    vConfig.bitrate = config->RateControl ? config->RateControl->BitrateLimit : 0;
    vConfig.fps = config->RateControl ? config->RateControl->FrameRateLimit : 0;
    vConfig.width = config->Resolution ? config->Resolution->Width : 0;
    vConfig.height = config->Resolution ? config->Resolution->Height : 0;
    vConfig.quality = config->Quality;

    return true;
}

bool OnvifControl::getNetworkInterfaces() {
    if (_proxyDevice == nullptr) {
        WarnL << "Unknown proxyDevice";
        return false;
    }
    // get network interface and print mac address
    _tds__GetNetworkInterfaces *GetNetworkInterfaces = soap_new__tds__GetNetworkInterfaces(_m_soap);
    _tds__GetNetworkInterfacesResponse GetNetworkInterfacesResponse;
    if (!setCredentials()) {
        return false;
    }
    if (callWithAuth([&] { return _proxyDevice->GetNetworkInterfaces(GetNetworkInterfaces, GetNetworkInterfacesResponse); })) {
        reportError();
        return false;
    }

    for (const auto network : GetNetworkInterfacesResponse.NetworkInterfaces) {
        if (network->Enabled && !network->Info->HwAddress.empty()) {
            _deviceInfo.macAddress = network->Info->HwAddress;
            break;
        }
    }
    return true;
}

// WS-Security faults that mean the camera rejected our timestamp or digest. The
// cached clock offset is the first suspect, so drop it and let the next operation
// measure the camera clock again instead of replaying the same bad signature.
// Deliberately narrow: a network or "action not supported" fault says nothing
// about the clock, and throwing the offset away there would restore the extra
// round trip this cache exists to remove.
bool OnvifControl::isAuthOrTimestampFault(const std::string &fault) {
    std::string lower(fault);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    static const char *kMarkers[] = {
        "failedauthentication",
        "invalidsecuritytoken",
        "securitytokenunavailable",
        "messageexpired",
        "notauthorized",
        "not authorized",
        "unauthorized",
    };
    for (size_t i = 0; i < sizeof(kMarkers) / sizeof(kMarkers[0]); ++i) {
        if (lower.find(kMarkers[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

DeviceFailure OnvifControl::classifyFailure(const FailureContext &ctx) {
    switch (ctx.soap_error) {
        case 401:
            if (ctx.basic_refused) {
                return DeviceFailure::AuthMethodUnsupported;
            }
            if (ctx.http_credentials_sent) {
                return DeviceFailure::Unauthorized;
            }
            // A bare 401 to WS-Security: handled as a rejected signature below.
            break;
        // Some cameras answer a locked or disabled account with 403.
        case 403:
            return DeviceFailure::Unauthorized;
        // Connect failed: refused, no route, or the connect timeout.
        case SOAP_TCP_ERROR:
        case SOAP_UDP_ERROR:
        case SOAP_SSL_ERROR:
            return DeviceFailure::Unreachable;
        // Connected, but no complete answer: receive timeout or the camera dropped the connection.
        case SOAP_EOF:
            return DeviceFailure::Timeout;
        default:
            // A SOAP fault (any HTTP status) saying the WS-Security credentials were rejected.
            if (!isAuthOrTimestampFault(ctx.fault)) {
                return DeviceFailure::Other;
            }
            break;
    }
    // The WS-Security signature was rejected: wrong credentials and a camera clock too far off look
    // the same, unless the camera names the timestamp or its clock was measured far off.
    std::string lower(ctx.fault);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lower.find("messageexpired") != std::string::npos) {
        return DeviceFailure::ClockSkew;
    }
    const time_t skew = ctx.clock_offset < 0 ? -ctx.clock_offset : ctx.clock_offset;
    if (ctx.clock_known && skew >= ctx.skew_threshold_sec) {
        return DeviceFailure::ClockSkew;
    }
    return DeviceFailure::Unauthorized;
}

DeviceFailure OnvifControl::classifyFailure(int soap_error, const std::string &fault, bool basic_refused) {
    FailureContext ctx;
    ctx.soap_error = soap_error;
    ctx.fault = fault;
    ctx.basic_refused = basic_refused;
    return classifyFailure(ctx);
}

void OnvifControl::reportError() {
    std::ostringstream oss;
    soap_stream_fault(_m_soap, oss);
    _soapErrMsg = oss.str();
    // A 401 to a request that already carried HTTP credentials means wrong user or password,
    // not a clock problem: keep the offset.
    const bool http_auth_rejected = _m_soap->error == 401 && _http_auth && _http_auth->sentCredentials();
    FailureContext ctx;
    ctx.soap_error = _m_soap->error;
    ctx.fault = _soapErrMsg;
    ctx.basic_refused = _http_auth && _http_auth->basicRefused();
    ctx.http_credentials_sent = _http_auth && _http_auth->sentCredentials();
    if (_measured_this_call) {
        const auto reading = clockReading();
        ctx.clock_known = reading.valid();
        ctx.clock_offset = reading.offset_sec;
    }
    GET_CONFIG(int, skew_threshold_sec, OnvifCtrl::kClockSkewThresholdSec);
    ctx.skew_threshold_sec = skew_threshold_sec;
    _last_failure = static_cast<int>(classifyFailure(ctx));
    if (!http_auth_rejected && OnvifControl::isAuthOrTimestampFault(_soapErrMsg)) {
        invalidateClockOffset();
    }
    WarnL << "Oops, something went wrong: " << oss.str();
}

// An own soap context talking to the camera's device service, for requests that must not disturb
// _m_soap (the error of the request being handled) or that run while it does not exist.
struct LocalDeviceSoap {
    explicit LocalDeviceSoap(const std::string &url) : endpoint(url) {
        soap = soap_new();
        soap->connect_timeout = soap->recv_timeout = soap->send_timeout = 10;
        soap_register_plugin(soap, soap_wsse);
        OnvifHttpAuth::attach(soap);
        proxy = new DeviceBindingProxy(soap);
        proxy->soap_endpoint = endpoint.c_str();
    }
    // Proxy before soap_free: its destructor still uses the context.
    ~LocalDeviceSoap() {
        delete proxy;
        soap_destroy(soap);
        soap_end(soap);
        soap_free(soap);
    }
    std::string endpoint;
    struct soap *soap;
    DeviceBindingProxy *proxy;
};

static bool isNetworkError(int soap_error) {
    return soap_error == SOAP_TCP_ERROR || soap_error == SOAP_UDP_ERROR || soap_error == SOAP_SSL_ERROR
        || soap_error == SOAP_EOF;
}

bool OnvifControl::getCameraTime() {
    if (!_proxyDevice || !_m_soap) {
        return false;
    }
    bool request_failed = false;
    if (readCameraClock(_m_soap, _proxyDevice, request_failed)) {
        return true;
    }
    if (request_failed) {
        reportError();
    } else {
        WarnL << "Invalid camera time response";
    }
    return false;
}

bool OnvifControl::readCameraClock(struct soap *soap, DeviceBindingProxy *proxy, bool &request_failed) {
    request_failed = false;
    auto challenge = lookupHttpAuthChallenge(soap);
    if (challenge) {
        challenge->date = 0;
    }
    if (!applyDeadline(soap)) {
        request_failed = true;
        return false;
    }
    _tds__GetSystemDateAndTime* req = soap_new__tds__GetSystemDateAndTime(soap, -1);
    _tds__GetSystemDateAndTimeResponse resp;

    if (proxy->GetSystemDateAndTime(req, resp)) {
        request_failed = true;
        checkDeadline();
        // ONVIF lets anyone read the clock, but some cameras want credentials even for this, and
        // signing needs the very clock we cannot read. Their refusal still carries a Date header.
        if (challenge && challenge->date && !isNetworkError(soap->error)) {
            setCameraClock(challenge->date, true, true);
            return true;
        }
        return false;
    }

    if (!resp.SystemDateAndTime) {
        return false;
    }

    if (resp.SystemDateAndTime->TimeZone) {
        _camTimeInfo.TZ = resp.SystemDateAndTime->TimeZone->TZ;
    }

    auto buildTime = [](tt__DateTime* dt, bool isUtc) -> time_t
    {
        if (!dt || !dt->Date || !dt->Time)
            return 0;

        struct tm t{};
        t.tm_year = dt->Date->Year - 1900;
        t.tm_mon  = dt->Date->Month - 1;
        t.tm_mday = dt->Date->Day;
        t.tm_hour = dt->Time->Hour;
        t.tm_min  = dt->Time->Minute;
        t.tm_sec  = dt->Time->Second;

        if (isUtc) {
            return timegm(&t);
        } else {
            return mktime(&t);
        }
    };

    if (resp.SystemDateAndTime->UTCDateTime) {
        time_t t = buildTime(resp.SystemDateAndTime->UTCDateTime, true);
        if (t > 0) {
            setCameraClock(t, true, false);
            return true;
        }
    }

    if (resp.SystemDateAndTime->LocalDateTime) {
        time_t t = buildTime(resp.SystemDateAndTime->LocalDateTime, false);
        if (t > 0) {
            setCameraClock(t, false, false);
            return true;
        }
    }

    return false;
}

void OnvifControl::setCameraClock(time_t camera_time, bool is_utc, bool from_http_date) {
    _camTimeInfo.cam_time = camera_time;
    if (is_utc) {
        int offset_cam = parse_timezone_offset_seconds(_camTimeInfo.TZ);
        int offset_sys = get_system_offset_seconds();
        _camTimeInfo.cam_sys_time = _camTimeInfo.cam_time + offset_cam - offset_sys;
    } else {
        _camTimeInfo.cam_sys_time = camera_time;
    }
    cacheClockOffset();

    // The reading reports the clock the way the maintenance cycle keeps it in sync (cam_sys_time,
    // see setCameraTimeManual()), not the raw UTC offset used for signing: a camera this server set
    // to its own wall clock in another time zone is not skewed.
    const time_t now = time(nullptr);
    _reading_offset.store(_camTimeInfo.cam_sys_time - now, std::memory_order_relaxed);
    _reading_from_date.store(from_http_date, std::memory_order_relaxed);
    _reading_server_time.store(now != 0 ? now : 1, std::memory_order_release);
}

bool OnvifControl::measureCameraClock() {
    LocalDeviceSoap local(deviceServiceUrl());
    bool request_failed = false;
    _measured_this_call = readCameraClock(local.soap, local.proxy, request_failed);
    if (!_measured_this_call) {
        return false;
    }
    const time_t skew = _reading_offset.load(std::memory_order_relaxed);
    // Same threshold as setCameraTimeManual(). Only flagged: the maintenance cycle sets the clock.
    if (skew >= 10 || skew <= -10) {
        _clock_sync_wanted = true;
    }
    return true;
}

void OnvifControl::cacheClockOffset() {
    const time_t now = time(nullptr);
    _clock_offset.store(_camTimeInfo.cam_time - now, std::memory_order_relaxed);
    // 0 is the "no entry" sentinel, so never store it as a timestamp.
    _clock_offset_at.store(now != 0 ? now : 1, std::memory_order_release);
}

void OnvifControl::invalidateClockOffset() {
    _clock_offset.store(0, std::memory_order_relaxed);
    _clock_offset_at.store(0, std::memory_order_release);
}

bool OnvifControl::clockOffsetFresh(time_t cached_at, time_t now, int ttl_sec) {
    if (ttl_sec <= 0 || cached_at == 0) {
        return false;
    }
    // A `now` before the stamp means the local clock jumped back; the age of the
    // entry is then unknown, so treat it as expired rather than trust it.
    if (now < cached_at) {
        return false;
    }
    return now - cached_at < ttl_sec;
}

bool OnvifControl::clockOffset(time_t &offset) {
    GET_CONFIG(int, ttl_sec, OnvifCtrl::kClockOffsetTtlSec);

    const time_t cached_at = _clock_offset_at.load(std::memory_order_acquire);
    if (clockOffsetFresh(cached_at, time(nullptr), ttl_sec)) {
        offset = _clock_offset.load(std::memory_order_relaxed);
        return true;
    }

    // Cache miss: one round trip here, then none until the TTL runs out.
    // getCameraTime() refreshes the cache itself on success.
    if (getCameraTime() && _clock_offset_at.load(std::memory_order_acquire) != 0) {
        offset = _clock_offset.load(std::memory_order_relaxed);
        return true;
    }

    // The camera's clock is unknown right now. Sign with the local clock rather
    // than keep using an offset we can no longer confirm.
    invalidateClockOffset();
    offset = 0;
    return false;
}

struct SoapHookContext {
    int (*original_fsend)(struct soap*, const char*, size_t);
    time_t offset;
    // The hook cannot be unhooked once soap->fsend is replaced, so it carries its
    // own off switch instead. Without it, a hook installed while the camera clock
    // was far off keeps rewriting timestamps after the drift is gone, and the
    // rewritten wsu:Created no longer matches the digest gSOAP computed.
    bool enabled;
};

void OnvifControl::disableSoapHook(struct soap* soap) {
    if (!soap || !soap->user)
        return;
    auto ctx = static_cast<SoapHookContext*>(soap->user);
    ctx->enabled = false;
    ctx->offset = 0;
}

void OnvifControl::installSoapHook(struct soap* soap, time_t offset) {
    if (!soap || !soap->fsend)
        return;

    if (soap->user) {
        auto ctx = static_cast<SoapHookContext*>(soap->user);
        ctx->offset = offset;
        ctx->enabled = true;
        return;
    }

    auto ctx = (SoapHookContext*)soap_malloc(soap, sizeof(SoapHookContext));
    ctx->original_fsend = soap->fsend;
    ctx->offset = offset;
    ctx->enabled = true;

    soap->user = ctx;

    soap->fsend = [](struct soap* soap, const char* buf, size_t len) -> int {
        auto ctx = static_cast<SoapHookContext*>(soap->user);
        if (!ctx || !ctx->original_fsend)
            return SOAP_ERR;

        // create UTC string (format: Y-m-dTH:M:SZ)
        auto makeUtcTime = [](time_t t) -> std::string {
            struct tm gmt;
            gmtime_r(&t, &gmt);

            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
            return std::string(buf);
        };

        auto replaceTagValue = [](std::string& xml, const std::string& tag, const std::string& value) {
            std::string open  = "<" + tag + ">";
            std::string close = "</" + tag + ">";

            size_t pos = 0;
            while ((pos = xml.find(open, pos)) != std::string::npos) {
                size_t end = xml.find(close, pos);
                if (end == std::string::npos) break;

                xml.replace(pos, end - pos + close.length(), open + value + close);

                pos += value.length();
            }
        };

        std::string xml(buf, len);

        // Only the wsu:Timestamp, which soap_wsse_add_Timestamp() stamps with the local clock. The
        // UsernameToken's wsu:Created is already the camera time and is covered by the password
        // digest: rewriting it a second later broke the signature.
        const size_t ts_begin = xml.find("<wsu:Timestamp");
        const size_t ts_end = ts_begin == std::string::npos ? std::string::npos : xml.find("</wsu:Timestamp>", ts_begin);
        if (ctx->enabled && ts_end != std::string::npos) {

            time_t now = time(nullptr) + ctx->offset;

            std::string created = makeUtcTime(now);
            std::string expires = makeUtcTime(now + 10);

            std::string timestamp = xml.substr(ts_begin, ts_end - ts_begin);
            replaceTagValue(timestamp, "wsu:Created", created);
            replaceTagValue(timestamp, "wsu:Expires", expires);
            xml.replace(ts_begin, ts_end - ts_begin, timestamp);
        }

        return ctx->original_fsend(soap, xml.c_str(), xml.size());
    };
}

void OnvifControl::setCameraTimeManual() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    // Called from the maintenance cycle only, connected or not: a camera whose clock is too far
    // off to accept us never connects, and setting its clock is how it recovers. Uses its own soap
    // context, so neither _m_soap nor the reason the last connect failed is touched.
    LocalDeviceSoap local(deviceServiceUrl());
    struct soap *local_soap = local.soap;
    DeviceBindingProxy *proxy = local.proxy;
    // Declared after `local` so it is released before local_soap is freed. Not shared with
    // _m_soap: a fresh http_da starts its nonce count again, which the camera takes as a replay.
    OnvifHttpAuth http_auth(local_soap, _strDeviceIp);

    bool request_failed = false;
    if (!readCameraClock(local_soap, proxy, request_failed) || _camTimeInfo.TZ.empty()) {
        return;
    }

    if (abs(_camTimeInfo.cam_sys_time - time(nullptr)) >= 10) {
        time_t now = time(nullptr);
        time_t camera_utc = build_camera_utc(_camTimeInfo.TZ, now);

        struct tm utc_tm;
        gmtime_r(&camera_utc, &utc_tm);

        _tds__SetSystemDateAndTime req;
        _tds__SetSystemDateAndTimeResponse resp;

        req.DateTimeType = tt__SetDateTimeType__Manual;
        req.DaylightSavings = false;

        req.UTCDateTime = soap_new_tt__DateTime(local_soap);
        req.UTCDateTime->Date = soap_new_tt__Date(local_soap);
        req.UTCDateTime->Time = soap_new_tt__Time(local_soap);

        req.UTCDateTime->Date->Year  = utc_tm.tm_year + 1900;
        req.UTCDateTime->Date->Month = utc_tm.tm_mon + 1;
        req.UTCDateTime->Date->Day   = utc_tm.tm_mday;

        req.UTCDateTime->Time->Hour   = utc_tm.tm_hour;
        req.UTCDateTime->Time->Minute = utc_tm.tm_min;
        req.UTCDateTime->Time->Second = utc_tm.tm_sec;

        time_t offset_time = _camTimeInfo.cam_time - time(nullptr);
        installSoapHook(local_soap, offset_time);

        if (soap_wsse_add_Timestamp(local_soap, "Time", 10) == SOAP_OK && soap_wsse_add_UsernameTokenDigest_at(local_soap, "Auth", _strUsername.c_str(), _strPassword.c_str(), _camTimeInfo.cam_time) == SOAP_OK) {
            int ret = proxy->SetSystemDateAndTime(&req, resp);
            if (ret == 401 && http_auth.learn(_strUsername, _strPassword)) {
                // The WS-Security header is still in place; only the HTTP credentials are new.
                http_auth.apply();
                ret = proxy->SetSystemDateAndTime(&req, resp);
            }
            if (ret != SOAP_OK) {
                WarnL << "Set camera time failed, ret " << ret;
            } else {
                InfoL << "Onvif device " << _strDeviceIp << " clock set, it was "
                      << (_camTimeInfo.cam_sys_time - now) << " s off";
                // The camera clock just moved, so the offset measured above no
                // longer describes it. Let the next operation measure it again.
                invalidateClockOffset();
                _reading_server_time.store(0, std::memory_order_release);
            }
        }
    }
}

bool OnvifControl::setCredentials() {
    soap_wsse_delete_Security(_m_soap);
    // Access with username, password and lifetime.
    // The offset is cached (onvif.clockOffsetTtlSec), so the common path costs no
    // round trip. It used to call getCameraTime() unconditionally, which made
    // GetSystemDateAndTime exactly half of all ONVIF traffic: one extra request
    // for each of the ~40 operations that authenticate through here.
    time_t offset_time = 0;
    const bool have_offset = clockOffset(offset_time);
    const time_t drift = offset_time < 0 ? -offset_time : offset_time;
    const bool need_update_time = have_offset && drift >= 5;
    _signed_offset = need_update_time ? offset_time : 0;

    // The hook rewrites wsu:Created/wsu:Expires on the way out, so it has to stay
    // in step with the timestamp the digest was computed over: refreshed when we
    // sign with the camera clock, switched off when we sign with the local one.
    // Installing it only above the threshold left an already-installed hook
    // stamping with a stale offset forever once the drift dropped below it.
    // HTTP Digest with qop=auth-int hashes the body before the hook rewrites it.
    if (need_update_time && !(_http_auth && _http_auth->signsBody())) {
        installSoapHook(_m_soap, offset_time);
    } else {
        disableSoapHook(_m_soap);
    }

    // Recomputed from the offset rather than cached: cam_time is an absolute
    // instant and also seeds the digest, so it must be current on every request.
    const time_t cam_time = time(nullptr) + offset_time;

    const int ret = need_update_time ? (soap_wsse_add_UsernameTokenDigest_at(_m_soap, "Auth", _strUsername.c_str(), _strPassword.c_str(), cam_time))
                                     : (soap_wsse_add_UsernameTokenDigest(_m_soap, "Auth", _strUsername.c_str(), _strPassword.c_str()));

    if (soap_wsse_add_Timestamp(_m_soap, "Time", 10) || ret) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::PTZ_AbsoluteMove(float pan, float tilt, float zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    DebugL << "Execute PTZ AbsoluteMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "). Use default speed of device";

    _tptz__AbsoluteMove *AbsoluteMove = soap_new__tptz__AbsoluteMove(_m_soap);
    _tptz__AbsoluteMoveResponse AbsoluteMoveResponse;

    AbsoluteMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (AbsoluteMove->Position == nullptr)
        AbsoluteMove->Position = soap_new_tt__PTZVector(_m_soap);
    if (AbsoluteMove->Position->PanTilt == nullptr)
        AbsoluteMove->Position->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (AbsoluteMove->Position->Zoom == nullptr)
        AbsoluteMove->Position->Zoom = soap_new_tt__Vector1D(_m_soap);
    AbsoluteMove->Position->PanTilt->x = pan;
    AbsoluteMove->Position->PanTilt->y = tilt;
    AbsoluteMove->Position->Zoom->x = zoom;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->AbsoluteMove(AbsoluteMove, AbsoluteMoveResponse); })) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    DebugL << "Execute PTZ AbsoluteMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "), "
            << "panSpeed(" << panSpeed << "), tiltSpeed(" << tiltSpeed << "), zoomSpeed(" << zoomSpeed << ")";

    _tptz__AbsoluteMove *AbsoluteMove = soap_new__tptz__AbsoluteMove(_m_soap);
    _tptz__AbsoluteMoveResponse AbsoluteMoveResponse;

    AbsoluteMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (AbsoluteMove->Position == nullptr)
        AbsoluteMove->Position = soap_new_tt__PTZVector(_m_soap);
    if (AbsoluteMove->Position->PanTilt == nullptr)
        AbsoluteMove->Position->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (AbsoluteMove->Position->Zoom == nullptr)
        AbsoluteMove->Position->Zoom = soap_new_tt__Vector1D(_m_soap);
    AbsoluteMove->Position->PanTilt->x = pan;
    AbsoluteMove->Position->PanTilt->y = tilt;
    AbsoluteMove->Position->Zoom->x = zoom;

    if (AbsoluteMove->Speed == nullptr)
        AbsoluteMove->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (AbsoluteMove->Speed->PanTilt == nullptr)
        AbsoluteMove->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (AbsoluteMove->Speed->Zoom == nullptr)
        AbsoluteMove->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    AbsoluteMove->Speed->PanTilt->x = panSpeed;
    AbsoluteMove->Speed->PanTilt->y = tiltSpeed;
    AbsoluteMove->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->AbsoluteMove(AbsoluteMove, AbsoluteMoveResponse); })) {
        reportError();
        return false;
    }
    return true;
}

PtzMoveStatus OnvifControl::PTZ_GetStatus(float &pan, float &tilt, float &zoom) {
    switch (PTZ_GetMoveStatus(pan, tilt, zoom)) {
        case tt__MoveStatus__IDLE: return PtzMoveStatus::IDLE;
        case tt__MoveStatus__MOVING: return PtzMoveStatus::MOVING;
        default: return PtzMoveStatus::UNKNOWN;
    }
}

tt__MoveStatus OnvifControl::PTZ_GetMoveStatus(float &pan, float &tilt, float &zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return tt__MoveStatus__UNKNOWN;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });

    _tptz__GetStatus *GetStatus = soap_new__tptz__GetStatus(_m_soap);
    _tptz__GetStatusResponse GetStatusResponse;
    GetStatus->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (!setCredentials())
        return tt__MoveStatus__UNKNOWN;
    if (callWithAuth([&] { return _proxyPTZ->GetStatus(GetStatus, GetStatusResponse); })) {
        reportError();
        return tt__MoveStatus__UNKNOWN;
    }
    DebugL << "Pan: " << GetStatusResponse.PTZStatus->Position->PanTilt->x;
    DebugL << "Tilt:" << GetStatusResponse.PTZStatus->Position->PanTilt->y;
    DebugL << "Zoom:" << GetStatusResponse.PTZStatus->Position->Zoom->x;

    pan = GetStatusResponse.PTZStatus->Position->PanTilt->x;
    tilt = GetStatusResponse.PTZStatus->Position->PanTilt->y;
    zoom = GetStatusResponse.PTZStatus->Position->Zoom->x;

    if (GetStatusResponse.PTZStatus->MoveStatus->PanTilt && GetStatusResponse.PTZStatus->MoveStatus->Zoom) {
        if (*GetStatusResponse.PTZStatus->MoveStatus->PanTilt == tt__MoveStatus__UNKNOWN ||
            *GetStatusResponse.PTZStatus->MoveStatus->Zoom == tt__MoveStatus__UNKNOWN) {
            return tt__MoveStatus__UNKNOWN;
        }
        if (*GetStatusResponse.PTZStatus->MoveStatus->PanTilt == tt__MoveStatus__MOVING ||
            *GetStatusResponse.PTZStatus->MoveStatus->Zoom == tt__MoveStatus__MOVING) {
            return tt__MoveStatus__MOVING;
        }
        if (*GetStatusResponse.PTZStatus->MoveStatus->PanTilt == tt__MoveStatus__IDLE &&
            *GetStatusResponse.PTZStatus->MoveStatus->Zoom == tt__MoveStatus__IDLE) {
            return tt__MoveStatus__IDLE;
        }
    }
    return tt__MoveStatus__UNKNOWN;
}

bool OnvifControl::PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    DebugL << "Execute PTZ ContinuousMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "). Use default speed of device";

    _tptz__ContinuousMove *ContinuosMove = soap_new__tptz__ContinuousMove(_m_soap);
    _tptz__ContinuousMoveResponse ContinuosMoveResponse;

    ContinuosMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (ContinuosMove->Velocity == nullptr)
        ContinuosMove->Velocity = soap_new_tt__PTZSpeed(_m_soap);
    if (ContinuosMove->Velocity->PanTilt == nullptr)
        ContinuosMove->Velocity->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (ContinuosMove->Velocity->Zoom == nullptr)
        ContinuosMove->Velocity->Zoom = soap_new_tt__Vector1D(_m_soap);
    ContinuosMove->Velocity->PanTilt->x = pan;
    ContinuosMove->Velocity->PanTilt->y = tilt;
    ContinuosMove->Velocity->Zoom->x = zoom;
    if (timeout > 0) {
        ostringstream ss;
        ss << "PT" << timeout << "S";
        ContinuosMove->Timeout = soap_new_std__string(_m_soap, -1);
        *ContinuosMove->Timeout = ss.str();
    }

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->ContinuousMove(ContinuosMove, ContinuosMoveResponse); })) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::PTZ_Stop(bool panTilt, bool zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ ContinuousMove Stop: PanTilt(" << panTilt << "), Zoom(" << zoom << ")";

    _tptz__Stop *Stop = soap_new__tptz__Stop(_m_soap);
    _tptz__StopResponse StopResponse;

    Stop->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (Stop->PanTilt == nullptr) {
        Stop->PanTilt = soap_new_bool(_m_soap, -1);
        *Stop->PanTilt = panTilt;
    }

    if (Stop->Zoom == nullptr) {
        Stop->Zoom = soap_new_bool(_m_soap, -1);
        *Stop->Zoom = zoom;
    }

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->Stop(Stop, StopResponse); })) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::PTZ_RelativeMove(float pan, float tilt, float zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    DebugL << "Execute PTZ RelativeMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "). Use default speed of device";

    _tptz__RelativeMove *RelativeMove = soap_new__tptz__RelativeMove(_m_soap);
    _tptz__RelativeMoveResponse RelativeMoveResponse;

    RelativeMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (RelativeMove->Translation == nullptr)
        RelativeMove->Translation = soap_new_tt__PTZVector(_m_soap);
    if (RelativeMove->Translation->PanTilt == nullptr)
        RelativeMove->Translation->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (RelativeMove->Translation->Zoom == nullptr)
        RelativeMove->Translation->Zoom = soap_new_tt__Vector1D(_m_soap);
    RelativeMove->Translation->PanTilt->x = pan;
    RelativeMove->Translation->PanTilt->y = tilt;
    RelativeMove->Translation->Zoom->x = zoom;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->RelativeMove(RelativeMove, RelativeMoveResponse); })) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    DebugL << "Execute PTZ RelativeMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "), "
            << "panSpeed(" << panSpeed << "), tiltSpeed(" << tiltSpeed << "), zoomSpeed(" << zoomSpeed << ")";

    _tptz__RelativeMove *RelativeMove = soap_new__tptz__RelativeMove(_m_soap);
    _tptz__RelativeMoveResponse RelativeMoveResponse;

    RelativeMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (RelativeMove->Translation == nullptr)
        RelativeMove->Translation = soap_new_tt__PTZVector(_m_soap);
    if (RelativeMove->Translation->PanTilt == nullptr)
        RelativeMove->Translation->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (RelativeMove->Translation->Zoom == nullptr)
        RelativeMove->Translation->Zoom = soap_new_tt__Vector1D(_m_soap);
    RelativeMove->Translation->PanTilt->x = pan;
    RelativeMove->Translation->PanTilt->y = tilt;
    RelativeMove->Translation->Zoom->x = zoom;

    if (RelativeMove->Speed == nullptr)
        RelativeMove->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (RelativeMove->Speed->PanTilt == nullptr)
        RelativeMove->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (RelativeMove->Speed->Zoom == nullptr)
        RelativeMove->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    RelativeMove->Speed->PanTilt->x = panSpeed;
    RelativeMove->Speed->PanTilt->y = tiltSpeed;
    RelativeMove->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->RelativeMove(RelativeMove, RelativeMoveResponse); })) {
        reportError();
        return false;
    }
    return true;
}

vector<OnvifMediaProfile> OnvifControl::selectStreamUrls(bool include_secondary) {
    vector<OnvifMediaProfile> ret;
    bool found_primary = false;
    // find primary stream that eligible for
    for (const auto &it : _mediaProfile) {
        if (eligibleForPrimaryStream(it.vcodec, it.width, it.height, it.bitrate, it.fps)) {
            ret.push_back(it);
            found_primary = true;
            break;
        }
    }

    // if can not find any profile eligible for primary stream or require finding profile eligible for secordary stream
    if (!found_primary || include_secondary) {
        for (const auto &it : _mediaProfile) {
            if (eligibleForSecondaryStream(it.vcodec, it.width, it.height, it.bitrate, it.fps)) {
                ret.push_back(it);
                break;
            }
        }
    }

    return ret;
}

bool OnvifControl::PTZ_GotoPreset(const string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    DebugL << "Execute PTZ GotoPreset: presetToken(" << presetToken << "), panSpeed(" << panSpeed << "), tiltSpeed(" << tiltSpeed << "), zoomSpeed(" << zoomSpeed << ")";

    _tptz__GotoPreset *GotoPreset = soap_new__tptz__GotoPreset(_m_soap);
    _tptz__GotoPresetResponse GotoPresetResponse;

    GotoPreset->ProfileToken = _ptzProfile.strMediaProfileToken;
    GotoPreset->PresetToken = presetToken;
    if (GotoPreset->Speed == nullptr)
        GotoPreset->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (GotoPreset->Speed->PanTilt == nullptr)
        GotoPreset->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (GotoPreset->Speed->Zoom == nullptr)
        GotoPreset->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    GotoPreset->Speed->PanTilt->x = panSpeed;
    GotoPreset->Speed->PanTilt->y = tiltSpeed;
    GotoPreset->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->GotoPreset(GotoPreset, GotoPresetResponse); })) {
        reportError();
        return false;
    }

    return true;
}

bool OnvifControl::PTZ_SetPreset(const string &presetName, const string &presetToken, float &pan, float &tilt, float &zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });

    pan = 0.0f, tilt = 0.0f, zoom = 0.0f;
    tt__MoveStatus moveStatus = PTZ_GetMoveStatus(pan, tilt, zoom);
    if (moveStatus == tt__MoveStatus__MOVING) {
        WarnL << "PTZ is moving, can not set preset";
        return false;
    }

    DebugL << "Execute PTZ SetPreset: presetName(" << presetName << "), presetToken(" << presetToken << "), "
           << "current pan(" << pan << "), current tilt(" << tilt << "), current zoom(" << zoom << ")";

    _tptz__SetPreset *SetPreset = soap_new__tptz__SetPreset(_m_soap);
    _tptz__SetPresetResponse SetPresetResponse;

    SetPreset->ProfileToken = _ptzProfile.strMediaProfileToken;
    SetPreset->PresetToken = soap_new_std__string(_m_soap);
    SetPreset->PresetName  = soap_new_std__string(_m_soap);
    *SetPreset->PresetToken = presetToken;
    *SetPreset->PresetName = presetName;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->SetPreset(SetPreset, SetPresetResponse); })) {
        reportError();
        return false;
    }
    // todo: update preset list after set preset since some devices may change preset token after reboot
    // todo: update home preset if current preset is home preset
    // todo: store user defined preset token and name in local storage
    return true;
}

bool  OnvifControl::PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });

    if (_ptzProfile.isPresetEnable && _ptzProfile.isHomePresetEnable) {
        DebugL << "Goto Home preset: " << _ptzProfile.homePresetToken;
        return PTZ_GotoPreset(_ptzProfile.homePresetToken, panSpeed, tiltSpeed, zoomSpeed);
    }

    WarnL << "Home preset is not supported. Try execute GotoHomePosition command directly";
    _tptz__GotoHomePosition *GotoHomePosition = soap_new__tptz__GotoHomePosition(_m_soap);
    _tptz__GotoHomePositionResponse GotoHomePositionResponse;
    GotoHomePosition->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (GotoHomePosition->Speed == nullptr)
        GotoHomePosition->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (GotoHomePosition->Speed->PanTilt == nullptr)
        GotoHomePosition->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (GotoHomePosition->Speed->Zoom == nullptr)
        GotoHomePosition->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    GotoHomePosition->Speed->PanTilt->x = panSpeed;
    GotoHomePosition->Speed->PanTilt->y = tiltSpeed;
    GotoHomePosition->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyPTZ->GotoHomePosition(GotoHomePosition, GotoHomePositionResponse); })) {
        reportError();
        return false;
    }

    return true;
}

bool OnvifControl::getPTZPresets() {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    _tptz__GetPresets *GetPresets = soap_new__tptz__GetPresets(_m_soap);
    _tptz__GetPresetsResponse GetPresetsResponse;
    GetPresets->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (!setCredentials()) {
        return false;
    }
    if (callWithAuth([&] { return _proxyPTZ->GetPresets(GetPresets, GetPresetsResponse); })) {
        reportError();
        return false;
    }

    _ptzProfile.isPresetEnable = true;
    _ptzProfile.presetMap.clear();

    bool isHomePresetEnable = false;
    string homePresetToken;

    // note: reset preset list every time when get capabilities since preset token may change after device reboot
    if (!GetPresetsResponse.Preset.empty()) {

        for (const auto &preset : GetPresetsResponse.Preset) {
            if (!preset || !preset->token || preset->token->empty() || !preset->Name || preset->Name->empty() || !preset->PTZPosition) {
                continue;
            }
            string pToken = *preset->token;
            string pName = *preset->Name;
            auto pAbsPan = preset->PTZPosition->PanTilt ? preset->PTZPosition->PanTilt->x : 0.0f;
            auto pAbsTilt = preset->PTZPosition->PanTilt ? preset->PTZPosition->PanTilt->y : 0.0f;
            auto pAbsZoom = preset->PTZPosition->Zoom ? preset->PTZPosition->Zoom->x : 0.0f;

            TraceL << "Preset token: " << pToken << " name: " << pName << " pan: " << pAbsPan << " tilt: " << pAbsTilt << " zoom: " << pAbsZoom;
            OnvifPTZProfile::PTZPreset p;
            p.Token = pToken;
            p.Name = pName;
            p.absPan = pAbsPan;
            p.absTilt = pAbsTilt;
            p.absZoom = pAbsZoom;
            _ptzProfile.presetMap.emplace(pToken, std::move(p));

            if (pToken == "home" || pToken == "Home" || pToken == "1") {
                isHomePresetEnable = true;
                homePresetToken = pToken;
            }
        }
    } else {
        // float pan = 0.0f, tilt = 0.0f, zoom = 0.0f;
        // string homePresetToken = "1";
        // string homePresetName = "home";
        // if (PTZ_SetPreset(homePresetName, homePresetToken, pan, tilt, zoom)) {
        //     _ptzProfile.isPresetEnable = true;
        //     _ptzProfile.isHomePresetEnable = true;
        //     OnvifPTZProfile::PTZPreset homePreset;
        //     homePreset.Token = homePresetToken;
        //     homePreset.Name = homePresetName;
        //     homePreset.absPan = pan;
        //     homePreset.absTilt = tilt;
        //     homePreset.absZoom = zoom;
        //     _ptzProfile.homePresetToken = homePresetToken;
        //     _ptzProfile.presetMap.emplace(homePresetToken, std::move(homePreset));
        //     TraceL << "Home preset is supported by setting preset with token: " << homePresetToken << " and name: " << homePresetName;
        // }
    }

    _ptzProfile.isHomePresetEnable = isHomePresetEnable;
    _ptzProfile.homePresetToken = homePresetToken;
    TraceL << (isHomePresetEnable ? "Home preset is supported" : "Home preset can be not found");
    return true;
}

bool OnvifControl::getImagingCapabilities() {
    if (_proxyImaging == nullptr || _imageProfile.videoSourceToken.empty()) {
        return false;
    }
    if (!setCredentials()) {
        return false;
    }

    _timg__GetImagingSettings *GetImagingSettings = soap_new__timg__GetImagingSettings(_m_soap);
    GetImagingSettings->VideoSourceToken = _imageProfile.videoSourceToken;

    _timg__GetImagingSettingsResponse GetImagingSettingsResponse;
    if (callWithAuth([&] { return _proxyImaging->GetImagingSettings(GetImagingSettings, GetImagingSettingsResponse); }) != SOAP_OK) {
        reportError();
        return false;
    }

    // device support focus if ImagingSettings contains Focus element, but it may not support move focus control. need check move options to confirm whether continuous/absolute/relative focus move is supported.
    if (GetImagingSettingsResponse.ImagingSettings && GetImagingSettingsResponse.ImagingSettings->Focus) {
        _imageProfile.isFocusAutoSupported = GetImagingSettingsResponse.ImagingSettings->Focus->AutoFocusMode == tt__AutoFocusMode__AUTO;

        _timg__GetMoveOptions *GetMoveOptions = soap_new__timg__GetMoveOptions(_m_soap);
        GetMoveOptions->VideoSourceToken = _imageProfile.videoSourceToken;

        _timg__GetMoveOptionsResponse GetMoveOptionsResponse;
        if (callWithAuth([&] { return _proxyImaging->GetMoveOptions(GetMoveOptions, GetMoveOptionsResponse); }) != SOAP_OK) {
            reportError();
            return false;
        }
        if (GetMoveOptionsResponse.MoveOptions && GetMoveOptionsResponse.MoveOptions->Continuous) {
            _imageProfile.isFocusConsEnable = true;
            TraceL << "Imaging: focus continuous move supported";
        }
        if (GetMoveOptionsResponse.MoveOptions && GetMoveOptionsResponse.MoveOptions->Absolute) {
            _imageProfile.isFocusAbsEnable = true;
            TraceL << "Imaging: focus absolute move supported";
        }
        if (GetMoveOptionsResponse.MoveOptions && GetMoveOptionsResponse.MoveOptions->Relative) {
            _imageProfile.isFocusRelEnable = true;
            TraceL << "Imaging: focus relative move supported";
        }

        _imageProfile.isFocusEnable = true;
        TraceL << "Imaging focus enabled: " << _imageProfile.isFocusEnable;
    }

    // device support iris control if ImagingSettings contains Iris element, but it may not support move iris control. need check move options to confirm whether continuous/absolute/relative iris move is supported.
    if (GetImagingSettingsResponse.ImagingSettings && GetImagingSettingsResponse.ImagingSettings->Exposure) {
        _imageProfile.isIrisAutoSupported = GetImagingSettingsResponse.ImagingSettings->Exposure->Mode == tt__ExposureMode__AUTO;
        _imageProfile.irisMin = GetImagingSettingsResponse.ImagingSettings->Exposure->MinIris ? *GetImagingSettingsResponse.ImagingSettings->Exposure->MinIris : 0.0f;
        _imageProfile.irisMax = GetImagingSettingsResponse.ImagingSettings->Exposure->MaxIris ? *GetImagingSettingsResponse.ImagingSettings->Exposure->MaxIris : 0.0f;

        _imageProfile.isIrisEnable = _imageProfile.irisMax > _imageProfile.irisMin;
        TraceL << "Imaging iris enabled: " << _imageProfile.isIrisEnable;
    }

    return true;
}

bool OnvifControl::getRelayOutputCapabilities() {
    if (_proxyDevice == nullptr) {
        _soapErrMsg = "Device proxy not ready";
        WarnL << _soapErrMsg;
        return false;
    }
    if (!setCredentials()) {
        return false;
    }

    _tds__GetRelayOutputs *GetRelayOutputs = soap_new__tds__GetRelayOutputs(_m_soap);
    _tds__GetRelayOutputsResponse GetRelayOutputsResponse;
    if (callWithAuth([&] { return _proxyDevice->GetRelayOutputs(GetRelayOutputs, GetRelayOutputsResponse); }) != SOAP_OK) {
        reportError();
        return false;
    }
    // todo: implement getRelayOutputCapabilities() to query relay output capabilities from the device
     _relayOutputProfiles.clear();
    for (const auto *relay : GetRelayOutputsResponse.RelayOutputs) {
        if (relay && !relay->token.empty()) {
            OnvifRelayOutputProfile relayProfile;
            relayProfile.token = relay->token;
            _relayOutputProfiles.push_back(relayProfile);
        }
    }
    TraceL << "Relay output count: " << _relayOutputProfiles.size();

    return true;
}

bool OnvifControl::Imaging_FocusMove(float speed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyImaging == nullptr) {
        _soapErrMsg = "Imaging proxy not ready";
        WarnL << _soapErrMsg;
        return false;
    }
    if (_imageProfile.videoSourceToken.empty()) {
        _soapErrMsg = "Video source token is empty";
        WarnL << _soapErrMsg;
        return false;
    }

    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });

    if (!setCredentials()) {
        return false;
    }

    _timg__Move *Move = soap_new__timg__Move(_m_soap);
    _timg__MoveResponse MoveResponse;
    Move->VideoSourceToken = _imageProfile.videoSourceToken;
    Move->Focus = soap_new_tt__FocusMove(_m_soap);
    if (_imageProfile.isFocusConsEnable) {
        Move->Focus->Continuous = soap_new_tt__ContinuousFocus(_m_soap);
        Move->Focus->Continuous->Speed = speed;
    } else if (_imageProfile.isFocusAbsEnable) {
        Move->Focus->Absolute = soap_new_tt__AbsoluteFocus(_m_soap);
        Move->Focus->Absolute->Position = 0.0f;
        Move->Focus->Absolute->Speed = soap_new_float(_m_soap, -1);
        Move->Focus->Absolute->Speed = &speed;
    } else if (_imageProfile.isFocusRelEnable) {
        Move->Focus->Relative = soap_new_tt__RelativeFocus(_m_soap);
        Move->Focus->Relative->Distance = 0.0f;
        Move->Focus->Relative->Speed = soap_new_float(_m_soap, -1);
        Move->Focus->Relative->Speed = &speed;
    } else {
        _soapErrMsg = "Focus move is not supported";
        WarnL << _soapErrMsg;
        return false;
    }

    if (callWithAuth([&] { return _proxyImaging->Move(Move, MoveResponse); }) != SOAP_OK) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::Imaging_FocusStop() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyImaging == nullptr) {
        _soapErrMsg = "Imaging proxy not ready";
        WarnL << _soapErrMsg;
        return false;
    }
    if (_imageProfile.videoSourceToken.empty()) {
        _soapErrMsg = "Video source token is empty";
        WarnL << _soapErrMsg;
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    if (!setCredentials()) {
        return false;
    }

    _timg__Stop *Stop = soap_new__timg__Stop(_m_soap);
    _timg__StopResponse StopResponse;
    Stop->VideoSourceToken = _imageProfile.videoSourceToken;
    if (callWithAuth([&] { return _proxyImaging->Stop(Stop, StopResponse); }) != SOAP_OK) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::Imaging_IrisMove(float /*speed*/) {
    // Iris continuous move is not supported by the current gSOAP ONVIF binding.
    // _timg__Move only exposes a Focus field; there is no Iris field in this binding.
    _soapErrMsg = "Iris continuous move is not supported by this ONVIF binding";
    WarnL << _soapErrMsg;
    return false;
}

bool OnvifControl::Imaging_IrisStop() {
    _soapErrMsg = "Iris control is not supported by this ONVIF binding";
    return false;
}

bool OnvifControl::PTZ_AuxiliaryCommand(const std::string &auxiliaryData) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        _soapErrMsg = "PTZ proxy not ready";
        WarnL << _soapErrMsg;
        return false;
    }
    if (_ptzProfile.strMediaProfileToken.empty()) {
        _soapErrMsg = "PTZ profile token is empty";
        WarnL << _soapErrMsg;
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    if (!setCredentials()) {
        return false;
    }

    _tptz__SendAuxiliaryCommand *SendAuxiliaryCommand = soap_new__tptz__SendAuxiliaryCommand(_m_soap);
    SendAuxiliaryCommand->ProfileToken  = _ptzProfile.strMediaProfileToken;
    SendAuxiliaryCommand->AuxiliaryData = auxiliaryData;

    _tptz__SendAuxiliaryCommandResponse SendAuxiliaryCommandResponse;
    if (callWithAuth([&] { return _proxyPTZ->SendAuxiliaryCommand(SendAuxiliaryCommand, SendAuxiliaryCommandResponse); }) != SOAP_OK) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::setSynchronizationPoint(const std::string& token) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyMedia2 == nullptr) {
        WarnL << "Unknown proxyMedia2";
        return false;
    }

    _ns1__SetSynchronizationPoint *SetSynchronizationPoint = soap_new__ns1__SetSynchronizationPoint(_m_soap);
    SetSynchronizationPoint->ProfileToken = token;
    _ns1__SetSynchronizationPointResponse SetSynchronizationPointResponse;
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyMedia2->SetSynchronizationPoint(SetSynchronizationPoint, SetSynchronizationPointResponse); })) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::Relay_SetOutputState(const std::string &relayToken, bool active) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyDevice == nullptr) {
        _soapErrMsg = "Device proxy not ready";
        WarnL << _soapErrMsg;
        return false;
    }

    bool found = false;
    for (const auto &p : _relayOutputProfiles) {
        if (p.token == relayToken) {
            found = true;
            break;
        }
    }
    if (!found) {
        _soapErrMsg = "Relay token not found: " + relayToken;
        WarnL << _soapErrMsg;
        return false;
    }
    onceToken token([&]() { _isControlled++; }, [&]() { _isControlled--; });
    if (!setCredentials()) {
        return false;
    }

    _tds__SetRelayOutputState *SetRelayOutputState = soap_new__tds__SetRelayOutputState(_m_soap);
    SetRelayOutputState->RelayOutputToken = relayToken;
    SetRelayOutputState->LogicalState = active ? tt__RelayLogicalState__active : tt__RelayLogicalState__inactive;

    _tds__SetRelayOutputStateResponse SetRelayOutputStateResponse;
    if (callWithAuth([&] { return _proxyDevice->SetRelayOutputState(SetRelayOutputState, SetRelayOutputStateResponse); }) != SOAP_OK) {
        reportError();
        return false;
    }
    return true;
}

std::string OnvifControl::getReplayUri(const std::string& recordingToken) {
    std::string uri;
    if (!_proxyReplay) {
        return uri;
    }
    _trp__GetReplayUri *GetReplayUri = soap_new__trp__GetReplayUri(_m_soap);
    GetReplayUri->RecordingToken = recordingToken;
    GetReplayUri->StreamSetup = soap_new_tt__StreamSetup(_m_soap);
    if (!GetReplayUri->StreamSetup->Transport)
        GetReplayUri->StreamSetup->Transport = soap_new_tt__Transport(_m_soap);
    GetReplayUri->StreamSetup->Transport->Protocol = tt__TransportProtocol__RTSP;
    GetReplayUri->StreamSetup->Stream = tt__StreamType__RTP_Unicast;
    _trp__GetReplayUriResponse GetReplayUriResponse;
    if (!setCredentials()) {
        return uri;
    }

    if (callWithAuth([&] { return _proxyReplay->GetReplayUri(GetReplayUri, GetReplayUriResponse); })) {
        reportError();
        return uri;
    }

    uri = GetReplayUriResponse.Uri;
    return uri;
}

void OnvifControl::getRecordingSearchResults() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_proxySearch) {
        return;
    }

    _tse__FindRecordings *FindRecordings = soap_new__tse__FindRecordings(_m_soap);
    FindRecordings->KeepAliveTime = "PT60S";
    _tse__FindRecordingsResponse FindRecordingsResponse;
    if (!setCredentials()) {
        return;
    }

    if (callWithAuth([&] { return _proxySearch->FindRecordings(FindRecordings, FindRecordingsResponse); })) {
        reportError();
        return;
    }

    if(FindRecordingsResponse.SearchToken.empty()) {
        return;
    }

    _tse__GetRecordingSearchResults *GetRecordingSearchResults = soap_new__tse__GetRecordingSearchResults(_m_soap);
    GetRecordingSearchResults->SearchToken = FindRecordingsResponse.SearchToken;
    _tse__GetRecordingSearchResultsResponse GetRecordingSearchResultsResponse;
    if (!setCredentials()) {
        return;
    }

    if (callWithAuth([&] { return _proxySearch->GetRecordingSearchResults(GetRecordingSearchResults, GetRecordingSearchResultsResponse); })) {
        endSearch(GetRecordingSearchResults->SearchToken);
        reportError();
        return;
    }

    endSearch(GetRecordingSearchResults->SearchToken);

    if (GetRecordingSearchResultsResponse.ResultList) {
        for (auto info : GetRecordingSearchResultsResponse.ResultList->RecordingInformation) {
            for (auto track : info->Track) {
                if (track->TrackType == tt__TrackType__Video) {
                    RecordingInformation rec_info;
                    rec_info.recordingToken = info->RecordingToken;
                    rec_info.trackToken = track->TrackToken;
                    if (info->EarliestRecording) {
                        rec_info.earliestRecording = *info->EarliestRecording;
                    }
                    if (info->LatestRecording) {
                        rec_info.latestRecording = *info->LatestRecording;
                    }
                    rec_info.uri = getReplayUri(rec_info.recordingToken);
                    rec_info.hasRecord = rec_info.earliestRecording != 0 && rec_info.latestRecording != 0 && !rec_info.uri.empty();
                    if (rec_info.hasRecord) {
                        rec_info.vEncoder = GetRecordingVideoEncoder(rec_info.recordingToken, rec_info.latestRecording);
                    }
                    _recordingInformations.push_back(rec_info);
                }
            }
        }
    }

    std::sort(_recordingInformations.begin(), _recordingInformations.end(),
        [](const RecordingInformation& a, const RecordingInformation& b) {
            if (a.recordingToken != b.recordingToken)
                return a.recordingToken < b.recordingToken;
            return a.trackToken < b.trackToken;
        });

    if (_recordingInformations.size() > 2)
        _recordingInformations.erase(_recordingInformations.begin() + 2, _recordingInformations.end());

    _recordingInformations[0].streamType = 0;

    if (_recordingInformations.size() == 2)
        _recordingInformations[1].streamType = 1;

}

bool OnvifControl::findEvents(time_t& startTime, time_t& endTime, const RecordingInformation &info, std::string& outSearchToken) {
    if (!_proxySearch) {
        return false;
    }

    time_t sys_start = build_camera_utc(_camTimeInfo.TZ, startTime);
    time_t sys_end = build_camera_utc(_camTimeInfo.TZ, endTime);

    TraceL << "Camera timezone=" << _camTimeInfo.TZ
        << ", cameraTime=" << _camTimeInfo.cam_time
        << ", cameraSystemTime=" << _camTimeInfo.cam_sys_time
        << ", startTime=" << startTime
        << ", systemStartTime=" << sys_start
        << ", endTime=" << endTime
        << ", systemEndTime=" << sys_end;

    startTime = std::max(sys_start, info.earliestRecording);
    endTime = std::min(sys_end, info.latestRecording);

    _tse__FindEvents *FindEvents = soap_new__tse__FindEvents(_m_soap);

    FindEvents->StartPoint = startTime;
    FindEvents->EndPoint   = &endTime;
    FindEvents->Scope = soap_new_tt__SearchScope(_m_soap);
    FindEvents->Scope->IncludedRecordings.push_back(info.recordingToken);
    auto sr = soap_new_tt__SourceReference(_m_soap);
    sr->Token = info.recordingToken;
    FindEvents->Scope->IncludedSources.push_back(sr);
    FindEvents->SearchFilter = soap_new_tt__EventFilter(_m_soap);
    FindEvents->IncludeStartState = true;
    int maxMatches = 100;
    FindEvents->MaxMatches = &maxMatches;
    FindEvents->KeepAliveTime = "PT60S";
    _tse__FindEventsResponse FindEventsResponse;

    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxySearch->FindEvents(FindEvents, FindEventsResponse); })) {
        reportError();
        return false;
    }
    outSearchToken = FindEventsResponse.SearchToken;
    if (outSearchToken.empty()) {
        return false;
    }

    return true;
}

bool OnvifControl::getEventSearchResults(const std::string& searchToken, const RecordingInformation &info, std::vector<TrackStateEvent>& outEvents) {
    if (!_proxySearch) {
        return false;
    }
    int totalProcessed = 0;

    while(true) {
        _tse__GetEventSearchResults *GetEventSearchResults = soap_new__tse__GetEventSearchResults(_m_soap);
        GetEventSearchResults->SearchToken = searchToken;
        GetEventSearchResults->MinResults = nullptr;
        int max = 100;
        GetEventSearchResults->MaxResults = &max;
        std::string waitTime = "PT60S";
        GetEventSearchResults->WaitTime = &waitTime;
        _tse__GetEventSearchResultsResponse GetEventSearchResultsResponse;
        if (!setCredentials()) {
            return false;
        }

        if (callWithAuth([&] { return _proxySearch->GetEventSearchResults(GetEventSearchResults, GetEventSearchResultsResponse); })) {
            endSearch(searchToken);
            reportError();
            return false;
        }

        auto resultList = GetEventSearchResultsResponse.ResultList;
        if (!resultList) {
            InfoL << "[GetEventSearchResults] Null ResultList";
            break;
        }

        for (auto result : resultList->Result)
        {
            TrackStateEvent ev;
            ev.recordingToken = result->RecordingToken;
            ev.trackToken = result->TrackToken;
            if (ev.trackToken != info.trackToken) {
                continue;
            }
            ev.time = result->Time;
            // true = virtual event do IncludeStartState=true
            ev.isStartState = result->StartStateEvent;
            // Parse IsDataPresent from XML Event
            std::string isDataPresent = "";
            if (!result || !result->Event || !result->Event->Message.__any) {
                continue;
            }
            if (!parseIsDataPresentFromXml(result->Event->Message.__any, "IsDataPresent", isDataPresent)) {
                TraceL << "[ParseEvent] Cannot parse IsDataPresent: "
                        << ev.recordingToken << "|" << ev.trackToken
                        << " t=" << ev.time
                        << " startState=" << ev.isStartState;
                continue;
            }

            ev.isDataPresent = isDataPresent == "true";
            outEvents.push_back(ev);
            ++totalProcessed;
        }
        InfoL << "[GetEventSearchResults] Received " << resultList->Result.size()
              << " totalProcessed " << totalProcessed
              << " SearchState " << resultList->SearchState;
        if (resultList->SearchState == tt__SearchState__Searching) {
            continue;
        }
        break;
    }

    endSearch(searchToken);
    return outEvents.size() > 0;
}

void OnvifControl::reconstructVideoSegments(const std::vector<TrackStateEvent>& events, const time_t& startTime, const time_t& endTime, const int &min_segment_sec, std::vector<VideoSegment>& outSegments) {
    if (events.empty()) {
        DebugL << "Events is EMPTY, nothing to do";
        return;
    }

    // Group events (recordingToken + trackToken)
    using GroupKey = std::string;
    std::map<GroupKey, std::vector<const TrackStateEvent*>> groups;

    for (const TrackStateEvent& ev : events) {
        GroupKey key = ev.recordingToken + "|" + ev.trackToken;
        groups[key].push_back(&ev);
    }

    for (auto& group : groups) {
        std::stable_sort(group.second.begin(), group.second.end(),
            [](const TrackStateEvent* a, const TrackStateEvent* b) {
                if (a->time != b->time) return a->time < b->time;
                if (a->isStartState != b->isStartState) return a->isStartState;
                return false;
            });

        const std::string& recToken = group.second.front()->recordingToken;
        const std::string& trackToken = group.second.front()->trackToken;

        bool intervalOpen  = false;
        time_t intervalStart = 0;

        for (const TrackStateEvent* ev : group.second) {

            if (ev->isStartState) {
                // Virtual event at startTime:
                //   IsDataPresent=true  -> the track contains data when the connection is lost
                //   IsDataPresent=false -> the track contains no data when the connection is lost
                if (ev->isDataPresent && !intervalOpen) {
                    intervalOpen = true;
                    intervalStart = startTime; // clamp start to startTime
                }
                // If false: do not open the interval, wait for the next event
                continue;
            }

            if (ev->isDataPresent) {
                // The camera starts recording to this track
                if (!intervalOpen) {
                    intervalOpen  = true;
                    intervalStart = ev->time;
                }
                // If already opened: ignore (redundant event)
            } else {
                // The camera stops recording to this track
                if (intervalOpen) {
                    time_t segStart = intervalStart;
                    time_t segEnd   = ev->time;

                    // Clamp to [startTime, endTime]
                    if (segStart < startTime) segStart = startTime;
                    if (segEnd   > endTime)  segEnd = endTime;

                    int dur = static_cast<int>(segEnd - segStart);

                    // keep only segments with duration >= minSegmentSec
                    if (segStart < segEnd && dur >= min_segment_sec) {
                        VideoSegment seg;
                        seg.recordingToken = recToken;
                        seg.trackToken = trackToken;
                        seg.start_time = segStart;
                        seg.end_time = segEnd;
                        outSegments.push_back(seg);

                        InfoL << "[Segment] " << recToken << "|" << trackToken << " dur=" << dur << "s";
                    } else {
                        InfoL << "[Skip] " << trackToken << " dur=" << dur << "s";
                    }

                    intervalOpen = false;
                }
            }
        }

        // The interval is still open after all events are processed
        // -> camera is still recording at endTime -> close the interval at endTime
        if (intervalOpen) {
            time_t segStart = intervalStart;
            time_t segEnd   = endTime;

            if (segStart < startTime) segStart = startTime;

            int dur = static_cast<int>(segEnd - segStart);
            if (segStart < segEnd && dur >= min_segment_sec) {
                VideoSegment seg;
                seg.recordingToken = recToken;
                seg.trackToken = trackToken;
                seg.start_time = segStart;
                seg.end_time = segEnd;
                outSegments.push_back(seg);

                InfoL << "[Segment] " << recToken << "|" << trackToken << " dur=" << dur << "s (open->close at reconnect)";
            } else {
                DebugL  << "[Skip] " << recToken << "|" << trackToken
                        << " open->close at endTime, clampedStart=" << segStart
                        << " clampedEnd=" << segEnd << " dur=" << dur << "s"
                        << " reason=" << (segStart >= segEnd ? "invalid_range(<=0)" : "too_short(<min_segment_sec)");
            }
        }
    }

    // Sort output by startTime
    std::sort(outSegments.begin(), outSegments.end(),
        [](const VideoSegment& a, const VideoSegment& b) {
            return a.start_time < b.start_time;
        });
}

void OnvifControl::endSearch(const std::string& searchToken) {
    _tse__EndSearch *EndSearch = soap_new__tse__EndSearch(_m_soap);
    EndSearch->SearchToken = searchToken;
    _tse__EndSearchResponse EndSearchResponse;

    if (!setCredentials()) {
        reportError();
        return;
    }

    int ret = callWithAuth([&] { return _proxySearch->EndSearch(EndSearch, EndSearchResponse); });
    if (ret != SOAP_OK) {
        TraceL << "EndSearch error " << ret << " but ok if already completed";
    }
}

VideoEncoderConfig OnvifControl::GetRecordingVideoEncoder(const std::string& recordingToken, const time_t& time) {
    VideoEncoderConfig config;
    if (!_proxySearch) {
        return config;
    }

    _tse__GetMediaAttributes *GetMediaAttributes = soap_new__tse__GetMediaAttributes(_m_soap);
    GetMediaAttributes->RecordingTokens.push_back(recordingToken);
    GetMediaAttributes->Time = time;
    _tse__GetMediaAttributesResponse GetMediaAttributesResponse;
    if (!setCredentials()) {
        return config;
    }

    if (callWithAuth([&] { return _proxySearch->GetMediaAttributes(GetMediaAttributes, GetMediaAttributesResponse); })) {
        reportError();
        return config;
    }

    auto attributes = GetMediaAttributesResponse.MediaAttributes[0];
    if (attributes && !attributes->TrackAttributes.empty()) {
        for (const auto trackAttribute : attributes->TrackAttributes) {
            if (!trackAttribute->TrackInformation) continue;
            if (!trackAttribute->TrackInformation->TrackType != tt__TrackType__Video) continue;
            if (!trackAttribute->VideoAttributes) continue;

            if (trackAttribute->VideoAttributes->Bitrate) {
                config.bitrate = *trackAttribute->VideoAttributes->Bitrate;
            }
            config.vcodec = trackAttribute->VideoAttributes->Encoding;
            config.width = trackAttribute->VideoAttributes->Width;
            config.height = trackAttribute->VideoAttributes->Height;
            config.fps = trackAttribute->VideoAttributes->Framerate;
        }
    }
    return config;
}

std::unordered_map<int, std::vector<VideoSegment>> OnvifControl::findVideoSegments(const time_t startTime, const time_t endTime, const int &min_segment_sec) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    std::unordered_map<int, std::vector<VideoSegment>> outSegments;

    std::vector<VideoSegment> segments;

    if (startTime >= endTime) {
        DebugL << "Invalid input time range: startTime >= endTime";
        return outSegments;
    }

    InfoL << " Searching ["<< startTime << ", " << endTime << "]";

    _recordingInformations.clear();
    getRecordingSearchResults();
    if (_recordingInformations.empty()) {
        DebugL << "No recording information found";
        return outSegments;
    }

    for (auto &info : _recordingInformations) {
        if (!info.hasRecord) {
            continue;
        }
        std::vector<TrackStateEvent> events;
        // Step 1: FindEvents
        std::string searchToken;
        time_t effStart = startTime;
        time_t effEnd = endTime;
        if (!findEvents(effStart, effEnd, info, searchToken))
            continue;

        // Step 2: Collect events
        if (!getEventSearchResults(searchToken, info, events))
            continue;

        // Step 4: Reconstruct + filter
        reconstructVideoSegments(events, effStart, effEnd, min_segment_sec, segments);
    }

    for (const auto &info : _recordingInformations) {
        std::vector<VideoSegment> segs;
        for (const auto &seg : segments) {
            if (info.recordingToken == seg.recordingToken && info.trackToken == seg.trackToken) {
                segs.push_back(seg);
            }
        }
        outSegments.emplace(info.streamType, segs);
    }

    return outSegments;
}

void OnvifControl::getRecordingSummary() {
    if (!_proxySearch) {
        return;
    }

    _tse__GetRecordingSummary *GetRecordingSummary = soap_new__tse__GetRecordingSummary(_m_soap);
    _tse__GetRecordingSummaryResponse GetRecordingSummaryResponse;
    if (!setCredentials()) {
        return;
    }

    if (callWithAuth([&] { return _proxySearch->GetRecordingSummary(GetRecordingSummary, GetRecordingSummaryResponse); })) {
        reportError();
        return;
    }

    auto Summary = GetRecordingSummaryResponse.Summary;
    if (!Summary) return;

    _sdCardInfo.DataFrom = static_cast<uint64_t>(Summary->DataFrom);
    _sdCardInfo.DataUntil = static_cast<uint64_t>(Summary->DataUntil);
    _sdCardInfo.NumberRecordings = Summary->NumberRecordings;
}

void OnvifControl::getStorageConfiguration() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_proxyDevice) {
        return;
    }

    _tds__GetStorageConfigurations *GetStorageConfigurations = soap_new__tds__GetStorageConfigurations(_m_soap);
    _tds__GetStorageConfigurationsResponse GetStorageConfigurationsResponse;
    if (!setCredentials()) {
        return;
    }

    if (callWithAuth([&] { return _proxyDevice->GetStorageConfigurations(GetStorageConfigurations, GetStorageConfigurationsResponse); })) {
        reportError();
    } else {
        auto StorageConfig = GetStorageConfigurationsResponse.StorageConfigurations[0];
        if (StorageConfig->Data) {
            _sdCardInfo.type = StorageConfig->Data->type;
        }
    }
}

bool OnvifControl::setupAudioBackchannel(AudioBackchannelInfo &out, bool sharedDspPipeline) {
    std::string uri;
    if (!setupAudioBackchannel(uri, sharedDspPipeline) || uri.empty()) {
        return false;
    }
    out = AudioBackchannelInfo();
    out.transport = AudioBackchannelInfo::RTSP_ONVIF;
    out.url = uri;
    return true;
}

bool OnvifControl::setupAudioBackchannel(std::string &backchannelUri, const bool &sharedDspPipeline) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyMedia == nullptr || !supportAudioBackchannel()) {
        return false;
    }

    // Special case: the DSP hardware shares same pipeline for audio input and output
    if (sharedDspPipeline) {
        auto checkAudioEncoding = [](const std::vector<std::string>& supportedDecodingEncodings, const std::string& encoding) -> std::pair<bool, tt__AudioEncoding> {
            bool exists = std::find(supportedDecodingEncodings.begin(),
                                    supportedDecodingEncodings.end(),
                                    encoding) != supportedDecodingEncodings.end();

            if (exists) {
                return { false, tt__AudioEncoding__G711 };
            }

            const std::string& acodec = supportedDecodingEncodings.front();

            tt__AudioEncoding enc;
            if (acodec == "G711")      enc = tt__AudioEncoding__G711;
            else if (acodec == "G726") enc = tt__AudioEncoding__G726;
            else if (acodec == "AAC")  enc = tt__AudioEncoding__AAC;
            else enc = tt__AudioEncoding__G711;

            return { true, enc };
        };
        auto check = checkAudioEncoding(_audioOutputProfile.supportedDecodingEncodings, _audioInputProfile.acodec);
        if (check.first) {
            if (_audioInputProfile.encoderToken.empty() || _audioInputProfile.sampleBit.empty() || _audioInputProfile.sampleRate.empty()) return false;

            _trt__SetAudioEncoderConfiguration *SetAudioEncoderConfiguration = soap_new__trt__SetAudioEncoderConfiguration(_m_soap);
            SetAudioEncoderConfiguration->Configuration = soap_new_tt__AudioEncoderConfiguration(_m_soap);
            SetAudioEncoderConfiguration->Configuration->token = _audioInputProfile.encoderToken;
            SetAudioEncoderConfiguration->Configuration->Encoding = check.second;
            SetAudioEncoderConfiguration->Configuration->Bitrate = std::stoi(_audioInputProfile.sampleBit);
            SetAudioEncoderConfiguration->Configuration->SampleRate = std::stoi(_audioInputProfile.sampleRate);
            SetAudioEncoderConfiguration->ForcePersistence = true;
            _trt__SetAudioEncoderConfigurationResponse SetAudioEncoderConfigurationResponse;
            if (!setCredentials()) {
                return false;
            }

            if (callWithAuth([&] { return _proxyMedia->SetAudioEncoderConfiguration(SetAudioEncoderConfiguration, SetAudioEncoderConfigurationResponse); })) {
                reportError();
                return false;
            }
        }
    }

    static std::string sendPrimacy = "www.onvif.org/ver20/HalfDuplex/Client";
    if (_audioOutputProfile.sendPrimacy != sendPrimacy) {
        _trt__GetAudioOutputConfiguration *GetAudioOutputConfiguration = soap_new__trt__GetAudioOutputConfiguration(_m_soap);
        GetAudioOutputConfiguration->ConfigurationToken = _audioOutputProfile.token;
        _trt__GetAudioOutputConfigurationResponse GetAudioOutputConfigurationResponse;
        if (!setCredentials()) {
            return false;
        }

        if (callWithAuth([&] { return _proxyMedia->GetAudioOutputConfiguration(GetAudioOutputConfiguration, GetAudioOutputConfigurationResponse); })) {
            reportError();
            return false;
        }

        auto outputTokenConfiguration = GetAudioOutputConfigurationResponse.Configuration;
        outputTokenConfiguration->SendPrimacy = &sendPrimacy;

        _trt__SetAudioOutputConfiguration *SetAudioOutputConfiguration = soap_new__trt__SetAudioOutputConfiguration(_m_soap);
        SetAudioOutputConfiguration->Configuration = soap_new_tt__AudioOutputConfiguration(_m_soap);
        SetAudioOutputConfiguration->Configuration = outputTokenConfiguration;
        SetAudioOutputConfiguration->ForcePersistence = true;
        _trt__SetAudioOutputConfigurationResponse SetAudioOutputConfigurationResponse;
        if (!setCredentials()) {
            return false;
        }

        if (callWithAuth([&] { return _proxyMedia->SetAudioOutputConfiguration(SetAudioOutputConfiguration, SetAudioOutputConfigurationResponse); })) {
            reportError();
            return false;
        }
    }

    _trt__GetStreamUri *GetStreamUri = soap_new__trt__GetStreamUri(_m_soap);
    _trt__GetStreamUriResponse GetStreamUriResponse;
    GetStreamUri->ProfileToken = _audioOutputProfile.profileToken;
    GetStreamUri->StreamSetup = soap_new_tt__StreamSetup(_m_soap, -1);
    GetStreamUri->StreamSetup->Stream = tt__StreamType__RTP_Unicast;
    GetStreamUri->StreamSetup->Transport = soap_new_tt__Transport(_m_soap, -1);
    GetStreamUri->StreamSetup->Transport->Protocol = tt__TransportProtocol__RTSP;
    if (!setCredentials()) {
        return false;
    }

    if (callWithAuth([&] { return _proxyMedia->GetStreamUri(GetStreamUri, GetStreamUriResponse); })) {
        reportError();
        return false;
    }
    backchannelUri = GetStreamUriResponse.MediaUri->Uri;
    TraceL << "Backchannel Uri=" << backchannelUri;
    return true;
}

} // namespace managerkit

// namespace {

// DriverId getDriver() {
//     return OnvifDriver;
// }

// Controller::Ptr getControllerByDriverId(std::string ip, int port, std::string username, std::string password) {
//     return std::make_shared<OnvifController>();
// }

// } // namespace

// DriverPlugin onvif_plugin = {
//     getDriver,
//     getControllerByDriverId
// }
