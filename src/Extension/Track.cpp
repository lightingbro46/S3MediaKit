/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

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