#ifndef COMMON_STRTIME_H_
#define COMMON_STRTIME_H_

#include <string>

namespace managerkit {

/**
 * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD"
 * Chuỗi có định dạng "YYYY-MM-DD" sẽ được hiểu là "YYYY-MM-DD 00:00:00"
 * @param str Chuỗi ngày giờ
 * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
 */
uint64_t getTsFromDateStr(const std::string &str);

/**
 * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD/HH-MM-SS(-extra)"
 * @param str Chuỗi ngày giờ
 * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
 */
uint64_t getTsFromDateTimeStr(const std::string &str);

/**
 * Lấy timestamp từ chuỗi ngày giờ định dạng "YYYY-MM-DD/YYYY-MM-DD-HH-MM-SS(-extra)"
 * @param str Chuỗi ngày giờ
 * @return Timestamp tương ứng, hoặc 0 nếu không hợp lệ
 */
uint64_t getTsFromDateTimeStr2(const std::string &str);

} // namespace managerkit

#endif // COMMON_STRTIME_H_