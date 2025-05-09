#include <iostream>
#include "Util/logger.h"
#include "Util/SqlitePool.h"

using namespace std;
using namespace toolkit;

namespace xDatabase
{
extern const std::string krdbms;
extern const std::string kDbHost;
extern const std::string kDbPort;
extern const std::string kDbUser;
extern const std::string kDbPasswd;
extern const std::string kDbName;
extern const std::string kDbFilename;
extern const std::string kDbTimeoutSec;
} // namespace xDatabase

typedef enum {
    ECS,
    MSERVER,
    TIMELINE,
} DbOriginType;

void installDbManager();
