#ifndef SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_
#define SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_

#include "RtspMediaSource.h"
#include "RtspDemuxer.h"
#include "Common/MultiMediaSourceMuxer.h"

namespace mediakit {
class RtspDemuxer;
class RtspMediaSourceImp final : public RtspMediaSource, private TrackListener, public MultiMediaSourceMuxer::Listener  {
public:
    using Ptr = std::shared_ptr<RtspMediaSourceImp>;

    /**
     * Constructor
     * @param vhost Virtual host
     * @param app Application name
     * @param id Stream id
     * @param ringSize Ring buffer size
     */
    RtspMediaSourceImp(const MediaTuple& tuple, int ringSize = RTP_GOP_SIZE);

    /**
     * Set sdp
     */
    void setSdp(const std::string &strSdp) override;

    /**
     * Input rtp and parse
     */
    void onWrite(RtpPacket::Ptr rtp, bool key_pos) override;

    /**
     * Get total number of viewers, including (hls/rtsp/rtmp)
     */
    int totalReaderCount() override {
        return readerCount() + (_muxer ? _muxer->totalReaderCount() : 0);
    }

    /**
     * Set protocol conversion options
     */
    void setProtocolOption(const ProtocolOption &option);

    const ProtocolOption &getProtocolOption() const {
        return _option;
    }

    /**
     * _demuxer triggered add Track event
     */
    bool addTrack(const Track::Ptr &track) override {
        if (_muxer) {
            if (_muxer->addTrack(track)) {
                track->addDelegate(_muxer);
                return true;
            }
        }
        return false;
    }

    /**
     * _demuxer triggered Track add complete event
     */
    void addTrackCompleted() override {
        if (_muxer) {
            _muxer->addTrackCompleted();
        }
    }

    void resetTracks() override {
        if (_muxer) {
            _muxer->resetTracks();
        }
    }

    /**
     * _muxer triggered all Track ready event
     */
    void onAllTrackReady() override{
        _all_track_ready = true;
    }

    /**
     * Set event listener
     * @param listener Listener
     */
    void setListener(const std::weak_ptr<MediaSourceEvent> &listener) override{
        if (_muxer) {
            // _muxer object cannot handle the event, then give it to the listener
            _muxer->setMediaListener(listener);
        } else {
            // The _muxer object is not created, all events are given to the listener
            MediaSource::setListener(listener);
        }
    }

    RtspMediaSource::Ptr clone(const std::string& stream) override;
private:
    bool _all_track_ready = false;
    ProtocolOption _option;
    RtspDemuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;
};
} /* namespace mediakit */

#endif /* SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_ */
