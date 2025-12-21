#include <ctime>
#include <iomanip>
#include <algorithm>
#include "strTime.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

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

uint64_t getTsFromDateStr(const std::string &str) {
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

uint64_t getTsFromDateTimeStr(const std::string &str) {
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

uint64_t getTsFromDateTimeStr2(const std::string &str) {
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

} // namespace managerkit