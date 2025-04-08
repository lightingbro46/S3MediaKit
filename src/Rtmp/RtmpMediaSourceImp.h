#ifndef SRC_RTMP_RTMPTORTSPMEDIASOURCE_H_
#define SRC_RTMP_RTMPTORTSPMEDIASOURCE_H_

#include <mutex>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include "amf.h"
#include "Rtmp.h"
#include "RtmpDemuxer.h"
#include "RtmpMediaSource.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {

class RtmpMediaSourceImp final : public RtmpMediaSource, private TrackListener, public MultiMediaSourceMuxer::Listener {
public:
    using Ptr = std::shared_ptr<RtmpMediaSourceImp>;

    /**
     * Constructor
     * @param vhost Virtual host
     * @param app Application name
     * @param id Stream id
     * @param ringSize Ring buffer size
     */
    RtmpMediaSourceImp(const MediaTuple& tuple, int ringSize = RTMP_GOP_SIZE);

    /**
     * Set metadata
     */
    void setMetaData(const AMFValue &metadata) override;

    /**
     * Input rtmp and parse
     */
    void onWrite(RtmpPacket::Ptr pkt, bool = true) override;

    /**
     * Get total number of viewers, including (hls/rtsp/rtmp)
     */
    int totalReaderCount() override;

    /**
     * Set protocol conversion
     */
    void setProtocolOption(const ProtocolOption &option);

    const ProtocolOption &getProtocolOption() const {
        return _option;
    }

    /**
     * _demuxer triggered add Track event
     */
    bool addTrack(const Track::Ptr &track) override;

    /**
     * _demuxer triggered Track add complete event
     */
    void addTrackCompleted() override;

    void resetTracks() override;

    /**
     * _muxer triggered all Track ready event
     */
    void onAllTrackReady() override;

    /**
     * Set event listener
     * @param listener Listener
     */
    void setListener(const std::weak_ptr<MediaSourceEvent> &listener) override;

private:
    bool _all_track_ready = false;
    bool _recreate_metadata = false;
    ProtocolOption _option;
    AMFValue _metadata;
    RtmpDemuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;

};
} /* namespace mediakit */

#endif /* SRC_RTMP_RTMPTORTSPMEDIASOURCE_H_ */
