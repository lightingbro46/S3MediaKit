// #ifndef MANAGER_MONITORPROXY_H
// #define MANAGER_MONITORPROXY_H

// #include "Monitor/ResourceMonitor.h"
// #include "Util/TimeTicker.h"
// #include <memory>

// namespace managerkit {

// struct StreamInfo {
//     xTrackType track_type;
//     std::string track_name;

//     StreamInfo() { 
//         track_type = xTrackInvalid;
//         track_name = "none"; 
//     }
// };

// struct TranslationInfo {
//     std::vector<StreamInfo> stream_info;
//     int byte_speed;
//     uint64_t start_time_stamp;

//     TranslationInfo() {
//         byte_speed = -1;
//         start_time_stamp = 0;
//     }
// };

// class MonitorProxy
//     : public ResourceMonitor
//     , public ResourceEvent
//     , public std::enable_shared_from_this<MonitorProxy> {
// public:
//     using Ptr = std::shared_ptr<MonitorProxy>;

//     // If retry_count < 0, then retry playing indefinitely; otherwise, retry retry_count times
//     // Default to retrying indefinitely
//     MonitorProxy(const ResourceTuple &tuple, const ResourceOption &option, int retry_count = -1,
//         const toolkit::EventPoller::Ptr &poller = nullptr,
//         int reconnect_delay_min = 2, int reconnect_delay_max = 60, int reconnect_delay_step = 3);

//     ~MonitorProxy() override;

//     void setPlayCallbackOnce(std::function<void(const toolkit::SockException &ex)> &cb);

//     void setOnClose(std::function<void(const toolkit::SockException &ex)> cb);

//     void setOnDisconnect(std::function<void()> cb);

//     void setOnConnect(std::function<void(const TranslationInfo&)> cb);

//     void play(const std::string &strUrl) override;

//     int totalReaderCount();

//     int getStatus();
//     uint64_t getLiveSecs();
//     uint64_t getRePullCount();

//     // Using this only makes sense after a successful connection to the server
//     TranslationInfo getTranslationInfo();

//     const std::string& getUrl() const { return _pull_url; }
//     const MediaTuple& getMediaTuple() const { return _tuple; }
//     const ProtocolOption& getOption() const { return _option; }

// private:
//     // ResourceEvent override
//     bool close(Resource &sender) override;
//     int totalReaderCount(Resource &sender) override;
//     ResourceOriginType getOriginType(Resource &sender) const override;
//     std::string getOriginUrl(Resource &sender) const override;
//     std::shared_ptr<toolkit::SockInfo> getOriginSock(Resource &sender) const override;

//     void rePlay(const std::string &strUrl, int iFailedCnt);
//     void onPlaySuccess();
//     void setDirectProxy();
//     void setTranslationInfo();

// private:
//     int _retry_count;
//     int _reconnect_delay_min;
//     int _reconnect_delay_max;
//     int _reconnect_delay_step;
//     ResourceTuple _tuple;
//     ResourceOption _option;
// std::string _pull_url;
//     toolkit::Ticker::Ptr _timer;
//     std::function<void()> _on_disconnect;
//     std::function<void(const TranslationInfo &info)> _on_connect;
//     std::function<void(const toolkit::SockException &ex)> _on_close;
//     std::function<void(const toolkit::SockException &ex)> _on_play;
//     TranslationInfo _transtalion_info;

//     toolkit::Ticker _live_ticker;
//     // 0 indicates normal, 1 indicates attempting to stream
//     std::atomic<int> _live_status;
//     std::atomic<uint64_t> _live_secs;

//     std::atomic<uint64_t> _repull_count;
// };

// } // namespace managerkit

// #endif // MANAGER_MONITORPROXY_H