#ifndef ZLMEDIAKIT_MEDIASINK_H
#define ZLMEDIAKIT_MEDIASINK_H

#include <mutex>
#include <memory>
#include "Util/TimeTicker.h"
#include "Extension/Frame.h"
#include "Extension/Track.h"

namespace mediakit{

class TrackListener {
public:
    virtual ~TrackListener() = default;

    /**
     * Add track, internally calls the clone method of Track
     * Only clones sps pps information, not the Delegate relationship
     * @param track
     */
    virtual bool addTrack(const Track::Ptr & track) = 0;

    /**
     * Track added
     */
    virtual void addTrackCompleted() {};

    /**
     * Reset track
     */
    virtual void resetTracks() {};
};

class MediaSinkInterface : public FrameWriterInterface, public TrackListener {
public:
    using Ptr = std::shared_ptr<MediaSinkInterface>;
};

/**
 * AAC mute audio adder
 */
class MuteAudioMaker : public FrameDispatcher {
public:
    using Ptr = std::shared_ptr<MuteAudioMaker>;
    bool inputFrame(const Frame::Ptr &frame) override;

private:
    int _track_index = -1;
    uint64_t _audio_idx = 0;
};

/**
 * The role of this class is to wait for Track ready() to return true, that is, ready, and then notify the derived class to perform the next operation.
 * The purpose is to intercept and process the input Frame by Track before inputting the Frame, so as to obtain valid information (such as sps pps aa_cfg)
 */
class MediaSink : public MediaSinkInterface, public TrackSource{
public:
    using Ptr = std::shared_ptr<MediaSink>;
    /**
     * Input frame
     * @param frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Add track, internally calls the clone method of Track
     * Only clones sps pps information, not the Delegate relationship
     * @param track
     */
    bool addTrack(const Track::Ptr & track) override;

    /**
     * Track added, if it is a single Track, it will wait for a maximum of 3 seconds before triggering onAllTrackReady
     * This will increase the delay in generating the stream. If you add both audio and video tracks, you can skip this method.
     * Otherwise, to reduce the stream registration delay, please call this method manually.
     */
    void addTrackCompleted() override;

    /**
     * Set the maximum number of tracks, the value range is >=1; this method is of the addTrackCompleted type;
     * When setting a single track, it can speed up media registration
     */
    void setMaxTrackCount(size_t i);

    /**
     * Reset track
     */
    void resetTracks() override;

    /**
     * Get all Tracks
     * @param trackReady Whether to get the ready Track
     */
    std::vector<Track::Ptr> getTracks(bool trackReady = true) const override;

    /**
     * Determine whether the onAllTrackReady event has been triggered
     */
    bool isAllTrackReady() const;

    /**
     * Set whether to enable audio
     */
    void enableAudio(bool flag);

    /**
     * Set single audio
     */
    void setOnlyAudio();

    /**
     * Set whether to enable adding mute audio
     */
    void enableMuteAudio(bool flag);

    /**
     * Whether there is a video track
     */
    bool haveVideo() const;

protected:
    /**
     * A certain track is ready, its ready() status returns true,
     * This means that you can get its related information such as sps pps
     * @param track
     */
    virtual bool onTrackReady(const Track::Ptr & track) { return false; };

    /**
     * All Tracks are ready,
     */
    virtual void onAllTrackReady() {};

    /**
     * A certain Track outputs a frame, this method will be called only after onAllTrackReady is triggered
     * @param frame
     */
    virtual bool onTrackFrame(const Frame::Ptr &frame) { return false; };

private:
    /**
     * Trigger the onAllTrackReady event
     */
    void emitAllTrackReady();

    /**
     * Check if the track is ready
     */
    void checkTrackIfReady();
    void onAllTrackReady_l();
    /**
     * Add AAC mute track
     */
    bool addMuteAudioTrack();

private:
    bool _audio_add = false;
    bool _have_video = false;
    bool _enable_audio = true;
    bool _only_audio = false;
    bool _add_mute_audio = true;
    bool _all_track_ready = false;
    size_t _max_track_size = 2;

    toolkit::Ticker _ticker;
    MuteAudioMaker::Ptr _mute_audio_maker;

    std::unordered_map<int, toolkit::List<Frame::Ptr> > _frame_unread;
    std::unordered_map<int, std::function<void()> > _track_ready_callback;
    std::unordered_map<int, std::pair<Track::Ptr, bool/*got frame*/> > _track_map;
};


class MediaSinkDelegate : public MediaSink {
public:
    /**
     * Set track listener
     */
    void setTrackListener(TrackListener *listener);

protected:
    void resetTracks() override;
    bool onTrackReady(const Track::Ptr & track) override;
    void onAllTrackReady() override;

private:
    TrackListener *_listener = nullptr;
};

class Demuxer : protected TrackListener, public TrackSource {
public:
    void setTrackListener(TrackListener *listener, bool wait_track_ready = false);
    std::vector<Track::Ptr> getTracks(bool trackReady = true) const override;

protected:
    bool addTrack(const Track::Ptr &track) override;
    void addTrackCompleted() override;
    void resetTracks() override;

private:
    MediaSink::Ptr _sink;
    TrackListener *_listener = nullptr;
    std::vector<Track::Ptr> _origin_track;
};

}//namespace mediakit

#endif //ZLMEDIAKIT_MEDIASINK_H
