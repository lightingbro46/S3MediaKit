#include "guid.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

std::string format_guid(const std::string& s) {
    if (s.length() != 32) return "";
    return s.substr(0,8) + "-" + s.substr(8,4) + "-" + s.substr(12,4) + "-" +
           s.substr(16,4) + "-" + s.substr(20,12);
}

std::string generate_guid() {
    return format_guid(strToLower(makeRandStr(32)));
}

} // namespace managerkit 
