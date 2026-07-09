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
    const PlayerBase::Ptr& getDelegate() const { return _delegate; }
    float getProgress() const override;

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Socket::onCreateSocket _on_create_socket;
};

} /* namespace mediakit */

#endif /* SRC_PLAYER_MEDIAPLAYER_H_ */
