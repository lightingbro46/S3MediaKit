/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_SRtPLAYERIMP_H
#define ZLMEDIAKIT_SRtPLAYERIMP_H

#include "SrtPlayer.h"

namespace mediakit {

class SrtPlayerImp
    : public PlayerImp<SrtPlayer, PlayerBase>
    , private TrackListener {
public:
    using Ptr = std::shared_ptr<SrtPlayerImp>;
    using Super = PlayerImp<SrtPlayer, PlayerBase>;

    SrtPlayerImp(const toolkit::EventPoller::Ptr &poller) : Super(poller) {}
    ~SrtPlayerImp() override { DebugL; }

private:
    //// SrtPlayer override////
    void onSRTData(SRT::DataPacket::Ptr pkt) override;

    //// PlayerBase override////
    void onPlayResult(const toolkit::SockException &ex) override;
    std::vector<Track::Ptr> getTracks(bool ready = true) const override;

private:
    //// TrackListener override////
    bool addTrack(const Track::Ptr &track) override { return true; }
    void addTrackCompleted() override;

private:
    // for player
    DecoderImp::Ptr _decoder;
    MediaSinkInterface::Ptr _demuxer;

    // for pusher
    TSMediaSource::RingType::RingReader::Ptr _ts_reader;
};

} /* namespace mediakit */
#endif /* ZLMEDIAKIT_SRtPLAYERIMP_H */
