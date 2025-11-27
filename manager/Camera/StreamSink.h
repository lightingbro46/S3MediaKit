#ifndef CAMERA_STREAMSINK_H
#define CAMERA_STREAMSINK_H

#include "StreamSource.h"

namespace managerkit {

class StreamSink {
public:
    using Ptr = std::shared_ptr<StreamSink>;

    StreamSink();

    ~StreamSink();

    bool isStreamLive(int type);

    std::string getStreamStatus(int type);

    mediakit::TranslationInfo getStreamInfo(int type);

    void setupMonitor(int type, const StreamTuple &tuple, bool start_record, int rtp_type, int media_port);

    void stopMonitor(int type);

private:
    virtual void onStreamChange(int type) = 0;

    void onManager();

private:
    std::recursive_mutex _mtx_sink;
    std::unordered_map<int, StreamSource::Ptr> _monitor_map;
    toolkit::Timer::Ptr _timer_sink;
};

} // namespace managerkit

#endif // CAMERA_STREAMSINK_H