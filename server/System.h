#ifndef S3MEDIAKIT_SYSTEM_H
#define S3MEDIAKIT_SYSTEM_H

#include <string>

class System {
public:
    static std::string execute(const std::string &cmd);
    static void startDaemon(bool &kill_parent_if_failed);
    static void systemSetup();
};

#endif //S3MEDIAKIT_SYSTEM_H
