#ifndef ZLMEDIAKIT_SYSTEM_H
#define ZLMEDIAKIT_SYSTEM_H

#include <string>

class System {
public:
    static std::string execute(const std::string &cmd);
    static void startDaemon(bool &kill_parent_if_failed);
    static void systemSetup();
};

#endif //ZLMEDIAKIT_SYSTEM_H
