#include "StrUtil.h"
#include "Common/Parser.h"
#include "Common/strCoding.h"
#include <ctime>
#include <iomanip>
#include <algorithm>
#include "Util/logger.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

string UriUtils::replaceIp(const string &in_url, const string &nat_ip) {
    auto schema = findSubString(in_url.data(), nullptr, "://");
    auto middle_url = findSubString(in_url.data(), "://", "/");
    string _middle_url;
    auto it = middle_url.rfind(":");
    if (it != string::npos) {
        _middle_url = middle_url.substr(it);
        _middle_url = nat_ip + _middle_url;
    } else {
        _middle_url = nat_ip;
    }
    auto path = findSubString(in_url.data() + schema.size() + middle_url.size() + 4, nullptr, nullptr);
    return (StrPrinter << schema << "://" << _middle_url << "/" << path);
}

string UriUtils::replacePort(const string &in_url, int nat_port) {
    auto schema = findSubString(in_url.data(), nullptr, "://");
    auto middle_url = findSubString(in_url.data(), "://", "/");
    auto _middle_url = middle_url;
    auto it = middle_url.rfind(":");
    if (it != string::npos) {
        _middle_url = middle_url.substr(0, it);
    }
    _middle_url += ":" + to_string(nat_port);
    auto path = findSubString(in_url.data() + schema.size() + middle_url.size() + 4, nullptr, nullptr);
    return (StrPrinter << schema << "://" << _middle_url << "/" << path);
}

string UriUtils::replaceCredentials(const string &in_url, const string &username, const string &password) {
    auto schema = findSubString(in_url.data(), nullptr, "://");
    auto middle_url = findSubString(in_url.data(), "://", "/");
    auto _middle_url = middle_url;
    auto it = middle_url.rfind("@");
    if (it != string::npos) {
        _middle_url = middle_url.substr(it + 1);
    }
    if (!username.empty() && !password.empty()) {
        _middle_url = strCoding::UrlDecodeUserOrPass(username) + ":" + strCoding::UrlDecodeUserOrPass(password) + "@" + _middle_url;
    }
    auto path = findSubString(in_url.data() + schema.size() + middle_url.size() + 4, nullptr, nullptr);
    return (StrPrinter << schema << "://" << _middle_url << "/" << path);
}

vector<string> UriUtils::getUriList(const std::string &domain, const std::string &ip, int http_port, int https_port, bool prefer_ssl) {
    vector<string> uri_list;

    // add https url with domain if prefer_ssl is true and domain is not empty
    if (prefer_ssl && !domain.empty()) {
        string url = StrPrinter << "https://" << domain << ":" << https_port;
        uri_list.emplace_back(url);
    }

    // add http url with domain if domain is not empty
    if (!domain.empty()) {
        string url = StrPrinter << "http://" << domain << ":" << http_port;
        uri_list.emplace_back(url);
    }

    // add http url with ip if ip is not empty
    if (!ip.empty()) {
        string url = StrPrinter << "http://" << ip << ":" << http_port;
        uri_list.emplace_back(url);
    }

    return uri_list;
}

//////////////////////////////////////////////////////////////////////////////////////////////

static auto isDate = [](const std::string &s) -> bool {
    // YYYY-MM-DD
    if (s.size() != 10) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) {
            if (s[i] != '-') return false;
        } else if (!isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
};

uint64_t StrTimeUtils::getTsFromDateStr(const std::string &str) {
    if (str.empty()) {
        return 0;
    }

    std::tm tm = {};
    tm.tm_isdst = -1; // let mktime determine DST

    // Trường hợp chỉ có ngày: "YYYY-MM-DD"
    if (isDate(str)) {
        std::istringstream ds(str + " 00:00:00");
        ds >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ds.fail()) return 0;
        time_t t = mktime(&tm);
        return t > 0 ? static_cast<uint64_t>(t) : 0;
    }

    return 0;
}

uint64_t StrTimeUtils::getTsFromDateTimeStr(const std::string &str) {
    if (str.empty()) {
        return 0;
    }

    std::tm tm = {};
    tm.tm_isdst = -1; // let mktime determine DST

    size_t pos = str.find('/');
    if (pos == std::string::npos) {
        // Không phù hợp định dạng nào
        return 0;
    }

    std::string datePart = str.substr(0, pos);       // YYYY-MM-DD
    std::string timePart = str.substr(pos + 1);      // HH-MM-SS(-extra?)

    if (!isDate(datePart)) {
        return 0;
    }

    // Loại bỏ phần đầu '.' nếu có
    if (start_with(timePart, ".")) {
        timePart.erase(0, 1);
    }

    // Cắt bỏ phần hậu tố không thuộc HH-MM-SS (ví dụ '-1')
    // Chiến lược: lấy đúng 3 nhóm đầu tiên ngăn cách bởi '-'
    {
        auto parts = split(timePart, "-");
        if (parts.size() >= 3) {
            timePart = parts[0] + "-" + parts[1] + "-" + parts[2];
        } else {
            return 0;
        }
    }

    // Ghép sang định dạng parse: YYYY-MM-DD HH:MM:SS
    std::string full = datePart + " " + timePart;
    // Đổi dấu '-' trong phần giờ thành ':'
    std::replace(full.begin() + 11, full.end(), '-', ':');

    std::istringstream ss(full);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return 0;

    time_t t = mktime(&tm);
    return t > 0 ? static_cast<uint64_t>(t) : 0;

    return 0;
}

uint64_t StrTimeUtils::getTsFromDateTimeStr2(const std::string &str) {
    if (str.empty()) {
        return 0;
    }

    std::tm tm = {};
    tm.tm_isdst = -1; // let mktime determine DST

    size_t pos = str.find('/');
    if (pos == std::string::npos) {
        // Không phù hợp định dạng nào
        return 0;
    }

    std::string datePart = str.substr(0, pos);       // YYYY-MM-DD
    std::string timePart = str.substr(pos + 1);      // YYYY-MM-DD-HH-MM-SS(-extra?)

    if (!isDate(datePart)) {
        return 0;
    }

    // Loại bỏ phần đầu '.' nếu có
    if (start_with(timePart, ".")) {
        timePart.erase(0, 1);
    }

    // Cắt bỏ phần hậu tố không thuộc YYYY-MM-DD-HH-MM-SS (ví dụ '-1')
    // Chiến lược: lấy đúng 5 nhóm đầu tiên ngăn cách bởi '-'
    {
        auto parts = split(timePart, "-");
        if (parts.size() >= 6) {
            timePart = parts[0] + "-" + parts[1] + "-" + parts[2] + "-" + parts[3] + "-" + parts[4] + "-" + parts[5];
        } else {
            return 0;
        }
    }

    // Ghép sang định dạng parse: YYYY-MM-DD HH:MM:SS
    std::string full = timePart;
    full[10] = ' ';
    // Đổi dấu '-' trong phần giờ thành ':'
    std::replace(full.begin() + 11, full.end(), '-', ':');

    std::istringstream ss(full);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return 0;

    time_t t = mktime(&tm);
    return t > 0 ? static_cast<uint64_t>(t) : 0;

    return 0;
}

StrTimeUtils::WeekTime StrTimeUtils::getWeekTime(uint64_t stamp) {
    time_t ts = (time_t)stamp;
    std::tm *lt = std::localtime(&ts);
    WeekTime wt;
    wt.day_of_week = lt->tm_wday; // day of week
    wt.hour = lt->tm_hour;        // hour of day
    return wt;
}

//////////////////////////////////////////////////////////////////////////////////////////////

bool StrJsonUtils::readJsonString(const string &json_str, Json::Value &out) {
    // parse json string to json var
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    Json::Value data;
    string errs;

    unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json_str.c_str(), json_str.c_str() + json_str.size(), &data, &errs)) {
        WarnL << "Parse json string failed: " << errs;
        return false;
    }
    // get stream information from json var
    TraceL << "Json data: " << data.toStyledString();
    out = data;
    return true;
}

string StrJsonUtils::writeJsonString(const Json::Value &in) {
    // parse json string to json var
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    string output = Json::writeString(writer, in);
    return output;
}

} // namespace managerkit