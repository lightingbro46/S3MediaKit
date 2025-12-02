#ifndef COMMON_STRTIME_H_
#define COMMON_STRTIME_H_

#include <string>

namespace managerkit {

uint64_t findTimestampFromPath(const std::string &time_path);

} // namespace managerkit

#endif // COMMON_STRTIME_H_