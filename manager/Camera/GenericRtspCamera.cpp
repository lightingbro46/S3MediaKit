#include "GenericRtspCamera.h"
#include "../server/WebApi.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

bool equalCameraInfo(const CameraInfo &a, const CameraInfo &b) {
#define EQUAL_INFO_PROPERTY(name) if (a.name != b.name) return false;
    EQUAL_INFO_PROPERTY(ip)
    EQUAL_INFO_PROPERTY(port)
    EQUAL_INFO_PROPERTY(username)
    EQUAL_INFO_PROPERTY(password)
    EQUAL_INFO_PROPERTY(manufacturer)
    EQUAL_INFO_PROPERTY(model)

    return true;
}

CameraOption::CameraOption() {
    // todo: load default value from database
}

bool equalCameraOption(const CameraOption &a, const CameraOption& b) {
#define EQUAL_OPTION_PROPERTY(name) if (a.name != b.name) return false;
    EQUAL_OPTION_PROPERTY(doNotRecordPrimaryStream)
    EQUAL_OPTION_PROPERTY(doNotRecordSecondaryStream)
    EQUAL_OPTION_PROPERTY(enableRecord)
    EQUAL_OPTION_PROPERTY(keepArchivedMinForAuto)
    EQUAL_OPTION_PROPERTY(keepArchivedMinFor)
    EQUAL_OPTION_PROPERTY(keepArchivedMaxForAuto)
    EQUAL_OPTION_PROPERTY(keepArchivedMaxFor)
    EQUAL_OPTION_PROPERTY(enableActive)
    EQUAL_OPTION_PROPERTY(mediaPort)
    EQUAL_OPTION_PROPERTY(autoMediaPort)
    EQUAL_OPTION_PROPERTY(rtpTransport)

    return true;
}

} // namespace managerkit