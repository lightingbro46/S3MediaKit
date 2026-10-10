#include "HanwhaCamera.h"
#include <map>
#include <sstream>
#include "json/json.h"
#include "Common/StrUtil.h"

using namespace std;

namespace managerkit {

static string trim(const string &s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == string::npos) {
        return "";
    }
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

bool HanwhaCamera::parseSunapiDeviceInfo(const string &body, IDeviceInfo &info) {
    map<string, string> fields;
    const string text = trim(body);
    if (!text.empty() && text[0] == '{') {
        Json::Value obj;
        if (!StrJsonUtils::readJsonString(text, obj) || !obj.isObject()) {
            return false;
        }
        for (const auto &key : obj.getMemberNames()) {
            if (obj[key].isString()) {
                fields[key] = obj[key].asString();
            }
        }
    } else {
        istringstream lines(text);
        string line;
        while (getline(lines, line)) {
            size_t eq = line.find('=');
            if (eq != string::npos) {
                fields[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
        }
    }
    if (fields["Model"].empty()) {
        return false;
    }
    info.manufacturer    = "Hanwha Vision";
    info.model           = fields["Model"];
    info.firmwareVersion = fields["FirmwareVersion"];
    info.serialNumber    = fields["SerialNumber"];
    info.macAddress      = fields["ConnectedMACAddress"].empty() ? fields["MACAddress"] : fields["ConnectedMACAddress"];
    return true;
}

} // namespace managerkit
