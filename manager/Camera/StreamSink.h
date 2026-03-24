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

    bool setupRecord(int archive_mode, bool start);

    void onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) override;

    void setStreamRegist(int type, bool regist);

private:
    void onManager();

private:
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    std::unordered_map<int, StreamSource::Ptr> _monitor_map;
    toolkit::Timer::Ptr _timer_sink;
    // Delayed-stop task used to keep the outgoing stream recording until the
    // incoming stream has written its first IDR (seamless stream switch).
    toolkit::EventPoller::DelayTask::Ptr _switch_delay_task;
    int _archive_mode = 0;
    std::array<bool, StreamType::StreamMax> _stream_ready{{false, false}};
};

} // namespace managerkit

#endif // CAMERA_STREAMSINK_H