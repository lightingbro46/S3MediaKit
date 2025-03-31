/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_FLVPLAYER_H
#define ZLMEDIAKIT_FLVPLAYER_H

#include "FlvSplitter.h"
#include "Http/HttpClientImp.h"
#include "Player/PlayerBase.h"

namespace mediakit {

class FlvPlayer : public PlayerBase, public HttpClientImp, private FlvSplitter {
public:
    FlvPlayer(const toolkit::EventPoller::Ptr &poller);

    void play(const std::string &url) override;
    void teardown() override;

protected:
    void onResponseHeader(const std::string &status, const HttpHeader &header) override;
    void onResponseCompleted(const toolkit::SockException &ex) override;
    void onResponseBody(const char *buf, size_t size) override;

protected:
    virtual void onRtmpPacket(RtmpPacket::Ptr packet) = 0;
    virtual bool onMetadata(const AMFValue &metadata) = 0;

private:
    bool onRecvMetadata(const AMFValue &metadata) override;
    void onRecvRtmpPacket(RtmpPacket::Ptr packet) override;

private:
    bool _play_result = false;
    bool _benchmark_mode = false;
};

using FlvPlayerImp = FlvPlayerBase<FlvPlayer>;

}//namespace mediakit
#endif //ZLMEDIAKIT_FLVPLAYER_H
