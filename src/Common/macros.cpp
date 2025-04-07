#include "macros.h"

using namespace toolkit;

#if defined(ENABLE_VERSION)
#include "S3MVersion.h"
#endif

namespace mediakit {

/**
 * This project adopts a class MIT license. Users, while fulfilling the obligations of the MIT license, should also follow the obligation to retain the copyright information of S3MediaKit software.
 * Users may not remove the "S3MediaKit" information from the various services provided by S3MediaKit, including but not limited to the "title", "Server", "User-Agent" fields.
 * Otherwise, the main rights holder of this project (project initiator, main author) reserves the right to claim and sue.
 */
#if !defined(ENABLE_VERSION)
const char kServerName[] =  "S3MediaKit-3.0(build in " __DATE__ " " __TIME__ ")";
#else
const char kServerName[] = "S3MediaKit(git hash:" COMMIT_HASH "/" COMMIT_TIME ",branch:" BRANCH_NAME ",build time:" BUILD_TIME ")";
#endif

}//namespace mediakit
