#include <ctime>
#include <iomanip>
#include <algorithm>
#include "strTime.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

uint64_t findTimestampFromPath(const string &time_path) {
    // Hỗ trợ các định dạng:
    // 1. "YYYY-MM-DD/HH-MM-SS[-anything]" (định dạng cũ, ví dụ: 2025-07-16/14-38-34-1)
    // 2. "YYYY-MM-DD" (mới: trả về mốc 00:00:00 local time của ngày đó)
    // 3. (mở rộng nhẹ) "YYYY-MM-DD HH:MM:SS" nếu xuất hiện (dùng dấu cách)

    if (time_path.empty()) {
        return 0;
    }

    auto isDate = [](const std::string &s) -> bool {
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

    std::tm tm = {};
    tm.tm_isdst = -1; // let mktime determine DST

    // Trường hợp chỉ có ngày: "YYYY-MM-DD"
    if (isDate(time_path)) {
        std::istringstream ds(time_path + " 00:00:00");
        ds >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (ds.fail()) return 0;
        time_t t = mktime(&tm);
        return t > 0 ? static_cast<uint64_t>(t) : 0;
    }

    // Nếu chứa dấu cách và có vẻ là dạng "YYYY-MM-DD HH:MM:SS"
    if (time_path.size() >= 19 && time_path[10] == ' ') {
        std::istringstream fs(time_path);
        fs >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
        if (fs.fail()) return 0;
        time_t t = mktime(&tm);
        return t > 0 ? static_cast<uint64_t>(t) : 0;
    }

    // Định dạng cũ: "YYYY-MM-DD/HH-MM-SS(-extra)"
    size_t pos = time_path.find('/');
    if (pos == std::string::npos) {
        // Không phù hợp định dạng nào
        return 0;
    }

    std::string datePart = time_path.substr(0, pos);       // YYYY-MM-DD
    std::string timePart = time_path.substr(pos + 1);      // HH-MM-SS(-extra?)

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
}

} // namespace managerkit