#ifndef CAMERA_STREAMSINK_H
#define CAMERA_STREAMSINK_H

#include "GenericRtspCamera.h"
#include "StreamSource.h"

namespace managerkit {

class StreamSink : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<StreamSink> {
public:
    using Ptr = std::shared_ptr<StreamSink>;
    using OnStreamUpdate = std::function<void(int type, bool live, const std::string &status, const mediakit::TranslationInfo *info)>;
    
    StreamSink(const DeviceTuple &tuple, const toolkit::EventPoller::Ptr &poller);

    ~StreamSink();

    void start();

    void setOnStreamUpdate(const OnStreamUpdate &cb) {
        _on_stream_update = std::move(cb);
    }

    void setupMonitor(int type, const StreamTuple &tuple, const CameraOption &option);

    void stopMonitor(int type);

    bool setupRecord(int archive_mode, bool start);

private:
    void onManager();

private:
    std::mutex _mtx_sink;
    DeviceTuple _tuple;
    toolkit::EventPoller::Ptr _poller;
    bool _stream_ready[2] = { false , false };
    std::unordered_map<int, StreamSource::Ptr> _monitor_map;
    toolkit::Timer::Ptr _timer_sink;
    OnStreamUpdate _on_stream_update;
};

} // namespace managerkit

#endif // CAMERA_STREAMSINK_H