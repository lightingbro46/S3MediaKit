#ifndef COMMON_STRUTIL_H
#define COMMON_STRUTIL_H

#include <string>

namespace managerkit {

class UriUtils {
public:
    static std::string replaceIp(const std::string &in_url, const std::string &nat_ip);

    static std::string replacePort(const std::string &in_url, int nat_port);

    static std::string replaceCredentials(const std::string &in_url, const std::string &username, const std::string &password);

};

} // namespace managerkit

#endif // COMMON_STRUTIL_H