#ifndef CAMERA_STREAMSINK_H
#define CAMERA_STREAMSINK_H

#include "GenericRtspCamera.h"
#include "StreamSource.h"
#include <array>

namespace managerkit {

/**
 * StreamSink is responsible for managing the stream source of a camera,
 * including starting and stopping the stream source, and monitoring the stream status. 
 * It also provides an interface for StreamSource to report stream status updates to its listener, which is generally the DeviceSourceEvent of the camera.
 * Note: StreamSink is designed to be used in a single thread, which is the manager thread, and it is not thread-safe. If you want to use it in a multi-threaded environment, please make sure to synchronize the access to StreamSink.
 */
class StreamSink : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<StreamSink> {
public:
    using Ptr = std::shared_ptr<StreamSink>;
    
    StreamSink(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    ~StreamSink();

    void setListener(const std::weak_ptr<DeviceSourceEvent> &listener);

    void createTimer();

    void setupMonitor(int type, const StreamTuple &tuple, const CameraOption &option);

    void stopMonitor(int type);

    /**
     * Setup recording for the stream, which will be called when the recording mode of the camera changes.
     * The recording mode includes normal recording, motion detection recording, etc. 
     * The specific recording policy is determined by the RecordPolicy class.
     * @param archive_mode The recording mode, which is defined in RecordPolicy::ArchiveMode
     * @param event_active Whether the recording is triggered by an event, which is used to determine the recording policy when the recording mode is motion detection recording. 
     * If event_active is true, it means the recording is triggered by a motion event, and the recording policy will be to record the secondary stream immediately and record the primary stream after a delay; 
     * if event_active is false, it means the recording is triggered by a non-motion event, such as a schedule, and the recording policy will be to record both primary and secondary stream immediately.
     * @return Whether the recording setup is successful
     */
    bool setupRecord(int archive_mode, bool event_active);

    void onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) override;

    void setStreamRegist(int type, bool regist, bool event_active = false);

private:
    void onManager();

private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    std::unordered_map<int, StreamSource::Ptr> _monitor_map;
    toolkit::Timer::Ptr _timer_sink;
    int _archive_mode = 0;
    std::array<bool, StreamType::StreamMax> _stream_ready{{false, false}};
    std::string _primary_stream_id; // stream_id of the PrimaryStream monitor (cached for motion filter)
};

} // namespace managerkit

#endif // CAMERA_STREAMSINK_H