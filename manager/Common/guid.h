#ifndef COMMON_GUID_H
#define COMMON_GUID_H

#include <string>

namespace managerkit {
/**
 * Format by uuid/guid form
 */
std::string format_guid(const std::string& s);

/**
 * Generate uuid/guid
 */
std::string generate_guid();

} // namespace managerkit
#endif //