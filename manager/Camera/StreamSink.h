#ifndef CAMERA_STREAMSINK_H
#define CAMERA_STREAMSINK_H

#include "StreamSource.h"

namespace managerkit {

class StreamSink {
public:
    using Ptr = std::shared_ptr<StreamSink>;

    StreamSink(const std::unordered_map<int, StreamTuple> &stream_map);

    ~StreamSink();

    bool hasStreamTuple(int type) { 
        return _stream_map.find(type) != _stream_map.end(); 
    }

    const StreamTuple getStreamTuple(int type) {
        auto it = _stream_map.find(type);
        if (it == _stream_map.end()) {
            throw std::runtime_error("No stream at index " + type);
        }
        return it->second.first;
    }

    bool isStreamLive(int type);

    std::string getStreamStatus(int type);

    mediakit::TranslationInfo getStreamInfo(int type);

    void setupMonitor(int type, bool start_record, int rtp_type, int media_port);

    void stopMonitor(int type);

private:
    bool onStreamReady(int type);

    void checkStreamIfReady();

    void emitAllStreamReady();

    virtual void onAllStreamReady() {};

    virtual void onStreamChange(int type) {};

    void onManager();

private:
    std::recursive_mutex _mtx_sink;
    bool _all_stream_ready = false;
    std::unordered_map<int, std::pair<StreamTuple, bool>> _stream_map;
    std::unordered_map<int, StreamSource::Ptr> _monitor_map;
    toolkit::Timer::Ptr _timer_sink;
};

} // namespace managerkit

#endif // CAMERA_STREAMSINK_H