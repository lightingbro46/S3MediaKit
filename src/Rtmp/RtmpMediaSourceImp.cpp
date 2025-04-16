#include "RtmpDemuxer.h"
#include "RtmpMediaSourceImp.h"

namespace mediakit {

uint32_t RtmpMediaSource::getTimeStamp(TrackType trackType) {
    assert(trackType >= TrackInvalid && trackType < TrackMax);
    if (trackType != TrackInvalid) {
        // Get the timestamp of a track
        return _track_stamps[trackType];
    }

    // Get the minimum timestamp of all tracks
    uint32_t ret = UINT32_MAX;
    for (auto &stamp : _track_stamps) {
        if (stamp > 0 && stamp < ret) {
            ret = stamp;
        }
    }
    return ret;
}

void RtmpMediaSource::setMetaData(const AMFValue &metadata) {
    {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        _metadata = metadata;
        _metadata.set("title", std::string("Streamed by ") + kServerName);
    }

    _have_video = _metadata["videocodecid"];
    _have_audio = _metadata["audiocodecid"];
    if (_ring) {
        regist();

        AMFEncoder enc;
        enc << "onMetaData" << _metadata;
        RtmpPacket::Ptr packet = RtmpPacket::create();
        packet->buffer = enc.data();
        packet->type_id = MSG_DATA;
        packet->time_stamp = 0;
        packet->chunk_id = CHUNK_CLIENT_REQUEST_AFTER;
        packet->stream_index = STREAM_MEDIA;
        onWrite(std::move(packet));
    }
}

void RtmpMediaSource::onWrite(RtmpPacket::Ptr pkt, bool /*= true*/) {
    bool is_video = pkt->type_id == MSG_VIDEO;
    _speed[is_video ? TrackVideo : TrackAudio] += pkt->size();
    // Save the current timestamp
    switch (pkt->type_id) {
        case MSG_VIDEO: _track_stamps[TrackVideo] = pkt->time_stamp, _have_video = true; break;
        case MSG_AUDIO: _track_stamps[TrackAudio] = pkt->time_stamp, _have_audio = true; break;
        default: break;
    }

    if (pkt->isConfigFrame()) {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        _config_frame_map[pkt->type_id] = pkt;
        if (!_ring) {
            // After registration, receive the config frame and update it to each player
            return;
        }
    }

    if (!_ring) {
        std::weak_ptr<RtmpMediaSource> weak_self = std::static_pointer_cast<RtmpMediaSource>(shared_from_this());
        auto lam = [weak_self](int size) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->onReaderChanged(size);
        };

        // GOP defaults to buffering 512 groups of RTMP packets, each group of RTMP packets has the same timestamp (if merge writing is enabled, then each group is the RTMP packet within the merge writing time),
        // Every time a key frame's first RTMP packet is encountered, the GOP cache will be cleared (because there is a new key frame, which can also achieve instant opening)
        _ring = std::make_shared<RingType>(_ring_size, std::move(lam));
        if (_metadata) {
            regist();
        }
    }
    bool key = pkt->isVideoKeyFrame();
    auto stamp = pkt->time_stamp;
    PacketCache<RtmpPacket>::inputPacket(stamp, is_video, std::move(pkt), key);
}

RtmpMediaSourceImp::RtmpMediaSourceImp(const MediaTuple &tuple, int ringSize)
    : RtmpMediaSource(tuple, ringSize) {
    _demuxer = std::make_shared<RtmpDemuxer>();
    _demuxer->setTrackListener(this);
}

void RtmpMediaSourceImp::setMetaData(const AMFValue &metadata) {
    if (!_demuxer->loadMetaData(metadata)) {
        // This metadata is invalid and needs to be regenerated
        _metadata = metadata;
        _recreate_metadata = true;
    }
    RtmpMediaSource::setMetaData(metadata);
}

void RtmpMediaSourceImp::onWrite(RtmpPacket::Ptr pkt, bool /*= true*/) {
    if (!_all_track_ready || _muxer->isEnabled()) {
        // If all Tracks are not obtained, or protocol conversion is enabled, then demultiplexing rtmp is required
        _demuxer->inputRtmp(pkt);
    }
    GET_CONFIG(bool, directProxy, Rtmp::kDirectProxy);
    if (directProxy) {
        // Only direct proxy mode uses the original rtmp directly
        RtmpMediaSource::onWrite(std::move(pkt));
    }
}

int RtmpMediaSourceImp::totalReaderCount() {
    return readerCount() + (_muxer ? _muxer->totalReaderCount() : 0);
}

void RtmpMediaSourceImp::setProtocolOption(const ProtocolOption &option) {
    GET_CONFIG(bool, direct_proxy, Rtmp::kDirectProxy);
    _option = option;
    _option.enable_rtmp = !direct_proxy;
    _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, _demuxer->getDuration(), _option);
    _muxer->setMediaListener(getListener());
    _muxer->setTrackListener(std::static_pointer_cast<RtmpMediaSourceImp>(shared_from_this()));
    // Let the _muxer object intercept some events (such as recording related events)
    MediaSource::setListener(_muxer);

    for (auto &track : _demuxer->getTracks(false)) {
        _muxer->addTrack(track);
        track->addDelegate(_muxer);
    }
}

bool RtmpMediaSourceImp::addTrack(const Track::Ptr &track) {
    if (_muxer) {
        if (_muxer->addTrack(track)) {
            track->addDelegate(_muxer);
            return true;
        }
    }
    return false;
}

void RtmpMediaSourceImp::addTrackCompleted() {
    if (_muxer) {
        _muxer->addTrackCompleted();
    }
}

void RtmpMediaSourceImp::resetTracks() {
    if (_muxer) {
        _muxer->resetTracks();
    }
}

void RtmpMediaSourceImp::onAllTrackReady() {
    _all_track_ready = true;

    if (_recreate_metadata) {
        // Update metadata
        for (auto &track : _muxer->getTracks()) {
            Metadata::addTrack(_metadata, track);
        }
        RtmpMediaSource::setMetaData(_metadata);
    }
}

void RtmpMediaSourceImp::setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    if (_muxer) {
        // Events that the _muxer object cannot handle are then handled by the listener
        _muxer->setMediaListener(listener);
    } else {
        // If the _muxer object is not created, all events are handled by the listener
        MediaSource::setListener(listener);
    }
}

} // namespace mediakit
