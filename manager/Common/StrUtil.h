#ifndef COMMON_STRUTIL_H
#define COMMON_STRUTIL_H

#include <string>
#include <vector>

namespace managerkit {

class UriUtils {
public:
    static std::string replaceIp(const std::string &in_url, const std::string &nat_ip);

    static std::string replacePort(const std::string &in_url, int nat_port);

    static std::string replaceCredentials(const std::string &in_url, const std::string &username, const std::string &password);

    static std::vector<std::string> getUriList(const std::string &domain, const std::string &ip, int http_port, int https_port, bool prefer_ssl);
};

class StrTimeUtils {
public:
    /**
     * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD"
     * Chuỗi có định dạng "YYYY-MM-DD" sẽ được hiểu là "YYYY-MM-DD 00:00:00"
     * @param str Chuỗi ngày giờ
     * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
     */
    static uint64_t getTsFromDateStr(const std::string &str);

    /**
     * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD/HH-MM-SS(-extra)"
     * @param str Chuỗi ngày giờ
     * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
     */
    static uint64_t getTsFromDateTimeStr(const std::string &str);

    /**
     * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD/YYYY-MM-DD-HH-MM-SS(-extra)"
     * @param str Chuỗi ngày giờ
     * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
     */
    static uint64_t getTsFromDateTimeStr2(const std::string &str);

    struct WeekTime {
        int day_of_week; // 0 = Sunday, 1 = Monday, ... 6 = Saturday
        int hour;        // 0-23
    };

    static WeekTime getWeekTime(uint64_t stamp);
};

} // namespace managerkit

#endif // COMMON_STRUTIL_H