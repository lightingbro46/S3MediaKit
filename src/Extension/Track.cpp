#include "Track.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

Sdp::Ptr AudioTrackImp::getSdp(uint8_t payload_type) const {
    return std::make_shared<DefaultSdp>(payload_type, *this);
}
Sdp::Ptr VideoTrackImp::getSdp(uint8_t payload_type) const {
    return std::make_shared<DefaultSdp>(payload_type, *this);
}

} // namespace mediakit