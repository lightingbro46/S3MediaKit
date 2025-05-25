#ifndef S3MEDIAKIT_OSINFO_H
#define S3MEDIAKIT_OSINFO_H

#include <string>

namespace mediakit {

/**
 * Get hardware uuid
 */
std::string getHardwareUUID();

/**
 * Format by uuid/guid form
 */
std::string format_guid(const std::string& s);

} // namespace mediakit

#endif // S3MEDIAKIT_OSINFO_H