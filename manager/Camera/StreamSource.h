#ifndef COMMON_STREAMSOURCE_H
#define COMMON_STREAMSOURCE_H

#include "Util/util.h"
#include "Player/PlayerProxy.h"
#include "Common/DeviceSource.h"
#include "Server/ResourceMonitor.h"
#include "Record/EventRecordSession.h"

namespace managerkit {

typedef enum {
    PrimaryStream = 0,
    SecondaryStream = 1,
    StreamMax
} StreamType;

const std::string getStreamTypeString(int type);

bool isValidStreamType(int type);

struct StreamTuple : public DeviceTuple {
    std::string stream_id;
    std::string full_url;
    std::string shortUrl() const { 
        return vhost + '/' + device_id + '/' + stream_id; 
    }

    bool empty() const {
        return stream_id.empty() && full_url.empty();
    }

    bool operator==(const StreamTuple& other) const{
        return stream_id == other.stream_id &&
            full_url == other.full_url &&
            vhost == other.vhost &&
            device_id == other.device_id &&
            name == other.name;
    }
};

// All configuration needed to create and identify a StreamSource instance.
struct StreamOption {
    StreamTuple tuple;
    mediakit::ProtocolOption protocol;
    bool record_mp4   = false;
    int  rtp_type     = 0;
    int  media_port   = 0;
    std::string username;
    std::string password;
    float timeout_sec = 0.0f;

    bool operator==(const StreamOption &o) const {
        return tuple                   == o.tuple
            && record_mp4             == o.record_mp4
            && rtp_type               == o.rtp_type
            && media_port             == o.media_port
            && username               == o.username
            && password               == o.password
            && protocol.enable_audio  == o.protocol.enable_audio
            && protocol.enable_motion == o.protocol.enable_motion
            && protocol.roi_mask      == o.protocol.roi_mask
            && protocol.record_motion == o.protocol.record_motion;
    }
};

class StreamSource : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<StreamSource> {
public:
    using Ptr = std::shared_ptr<StreamSource>;

    StreamSource(int type, const StreamOption &option);

    ~StreamSource();

    void setListener(std::shared_ptr<DeviceSourceEvent> listener);

    void start();

    bool isLive() const { return _live.load(); }

    const StreamOption &getOption() const { return _option; }

    std::string getStatus() const {
        auto status_ptr = std::atomic_load_explicit(&_status, std::memory_order_acquire);
        return status_ptr ? *status_ptr : std::string();
    }

    mediakit::TranslationInfo getTranslationInfo();

    bool setupRecord(int type, bool start, bool replay_gop = true);

    /**
     * Start an infinite event-based recording clip for the primary stream.
     * Records pre_record_ms of buffered history (GOP backfill) before the
     * event and writes frames continuously thereafter.  Call stopRecord()
     * when the event ends so the clip is finalised with a post-event tail.
     * @return session handle (nullptr on failure).
     */
    mediakit::EventRecordSession::Ptr startEventRecord();

    /**
     * Extend the active event-recording window while the event is still
     * ongoing.  Pushes the stop deadline to (now + post_record_ms).
     * No-op if no session is active.
     */
    void extendEventRecord();

    /**
     * Signal end of the event.  Records (post_record_ms + extra_overlap_ms) more ms
     * then closes the file automatically.  No-op if no session is active.
     * @param extra_overlap_ms Additional ms to add on top of post_record_ms so the
     *        secondary recorder has time to receive its first IDR before primary ends.
     */
    void stopEventRecord(uint32_t extra_overlap_ms = 0);

    /**
     * True if an event-recording session is currently active (file is still
     * being written).  Used to decide whether a new motion event should
     * extend the current clip or start a fresh one.
     */
    bool hasActiveEventSession() const {
        return _event_session && _event_session->isActive();
    }

    /**
     * Immediately cancel any active event-recording session.
     * Calls stop(0) so the ring-reader lambda terminates on the very next
     * incoming frame and closes the file cleanly.  Must be called when
     * leaving RecordLowResAndMotion so the session does not outlive the mode
     * that started it (and does not conflict with recorders started by the
     * new mode).
     */
    void cancelEventRecord();

    /**
     * Returns the measured GOP interval (ms) of the video track on this stream.
     * Uses VideoTrack::getVideoGopInterval() which tracks wall-clock time between
     * consecutive IDR frames.  Returns 0 if no video track / not yet measured.
     */
    uint32_t getVideoGopIntervalMs() const;

private:
    void setState(bool live, std::string status);

    void createPlayer();

    void closePlayer();

    void onStreamReady(bool ready, const std::string &status, const mediakit::TranslationInfo *info);

private:
    int _type;
    StreamOption _option;
    std::string _full_url;
    std::atomic_bool _live {false};
    std::shared_ptr<const std::string> _status {std::make_shared<const std::string>("init")};
    std::weak_ptr<mediakit::PlayerProxy> _player;
    mediakit::EventRecordSession::Ptr _event_session;
};

} // namespace managerkit

#endif // COMMON_STREAMSOURCE_H