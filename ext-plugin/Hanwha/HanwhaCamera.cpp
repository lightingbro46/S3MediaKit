#include "HanwhaCamera.h"
#include <sstream>
#include <strings.h>
#include "json/json.h"
#include "Common/StrUtil.h"

using namespace std;

namespace managerkit {

// ONVIF switch of SUNAPI 2.x; names checked against sunapi_mock.py only, not a real camera yet.
static const char kOnvifViewPath[] = "/stw-cgi/network.cgi?msubmenu=onvif&action=view";
static const char kOnvifEnablePath[] = "/stw-cgi/network.cgi?msubmenu=onvif&action=set&Enable=True";

static string trim(const string &s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == string::npos) {
        return "";
    }
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

bool HanwhaCamera::parseSunapiFields(const string &body, map<string, string> &fields) {
    const string text = trim(body);
    if (!text.empty() && text[0] == '{') {
        Json::Value obj;
        if (!StrJsonUtils::readJsonString(text, obj) || !obj.isObject()) {
            return false;
        }
        for (const auto &key : obj.getMemberNames()) {
            if (obj[key].isString()) {
                fields[key] = obj[key].asString();
            } else if (obj[key].isBool()) {
                fields[key] = obj[key].asBool() ? "True" : "False";
            }
        }
        return true;
    }
    istringstream lines(text);
    string line;
    while (getline(lines, line)) {
        size_t eq = line.find('=');
        if (eq != string::npos) {
            fields[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    return !fields.empty();
}

bool HanwhaCamera::parseSunapiDeviceInfo(const string &body, IDeviceInfo &info) {
    map<string, string> fields;
    if (!parseSunapiFields(body, fields) || fields["Model"].empty()) {
        return false;
    }
    info.manufacturer    = "Hanwha Vision";
    info.model           = fields["Model"];
    info.firmwareVersion = fields["FirmwareVersion"];
    info.serialNumber    = fields["SerialNumber"];
    info.macAddress      = fields["ConnectedMACAddress"].empty() ? fields["MACAddress"] : fields["ConnectedMACAddress"];
    return true;
}

void HanwhaCamera::prepareOnvif(const std::string &username, const std::string &, OnDeviceResult cb) {
    if (!username.empty()) {
        setLastFailure(DeviceFailure::Other);
        cb(false, "Hanwha cameras have no ONVIF users of their own");
        return;
    }
    // The pending request holds `cb`, which the caller makes hold this plugin.
    std::weak_ptr<HanwhaCamera> weak_self = std::static_pointer_cast<HanwhaCamera>(shared_from_this());
    request("GET", kOnvifViewPath, "", "", [weak_self, cb](const VendorHttpAnswer &answer) {
        auto self = weak_self.lock();
        if (!self) {
            return;
        }
        map<string, string> fields;
        if (answer.status != 200 || !parseSunapiFields(answer.body, fields) || fields.find("Enable") == fields.end()) {
            self->failPrepare(answer, true, "read the ONVIF switch", cb);
            return;
        }
        if (strcasecmp(fields["Enable"].c_str(), "True") == 0) {
            self->setLastFailure(DeviceFailure::None);
            cb(true, "ONVIF on");
            return;
        }
        self->request("GET", kOnvifEnablePath, "", "", [weak_self, cb](const VendorHttpAnswer &answer) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            // SUNAPI answers a set with "OK", or "NG" + Error Code on failure.
            if (answer.status != 200 || trim(answer.body).compare(0, 2, "NG") == 0) {
                self->failPrepare(answer, false, "turn ONVIF on", cb);
                return;
            }
            self->setLastFailure(DeviceFailure::None);
            cb(true, "ONVIF turned on");
        });
    });
}

} // namespace managerkit
