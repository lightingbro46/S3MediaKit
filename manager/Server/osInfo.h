#ifndef S3MANAGERKIT_OSINFO_H
#define S3MANAGERKIT_OSINFO_H

#include <string>

namespace managerkit {

/**
 * Get hardware uuid
 */
std::string getHardwareUUID();

struct OSInfo {
    std::string platform;         // "Windows", "Linux", "macOS"
    std::string variant;          // "Ubuntu", "Debian", etc.
    std::string variant_version;  // "22.04", "10.0.19045", etc.
};

/**
 * Get OS platform , such as Window, Ubuntu
 */
OSInfo get_os_info();

} // namespace managerkit

#endif // S3MANAGERKIT_OSINFO_H