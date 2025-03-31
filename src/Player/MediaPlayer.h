/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_PLAYER_MEDIAPLAYER_H_
#define SRC_PLAYER_MEDIAPLAYER_H_

#include <memory>
#include <string>
#include "PlayerBase.h"

namespace mediakit {

class MediaPlayer : public PlayerImp<PlayerBase, PlayerBase> {
public:
    using Ptr = std::shared_ptr<MediaPlayer>;

    MediaPlayer(const toolkit::EventPoller::Ptr &poller = nullptr);

    void play(const std::string &url) override;
    toolkit::EventPoller::Ptr getPoller();
    void setOnCreateSocket(toolkit::Socket::onCreateSocket cb);

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Socket::onCreateSocket _on_create_socket;
};

} /* namespace mediakit */

#endif /* SRC_PLAYER_MEDIAPLAYER_H_ */
