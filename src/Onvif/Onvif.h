#ifndef S3MEDIAKIT_ONVIF_H
#define S3MEDIAKIT_ONVIF_H

#include <vector>
#include <map>
#include "Network/Socket.h"
#include "Network/Buffer.h"

class OnvifSearcher : public std::enable_shared_from_this<OnvifSearcher> {
public:
    //Returning false means no longer listening to the event
    using onDevice = std::function<bool(const std::map<std::string, std::string> &device_info, const std::string &onvif_url)>;
    OnvifSearcher();

    static OnvifSearcher &Instance();
    void sendSearchBroadcast(onDevice cb = nullptr, uint64_t timeout_ms = 10 * 1000);

private:
    void onDeviceResponse(const toolkit::Buffer::Ptr &buf, struct sockaddr *addr, int addr_len);
    void onGotDevice(const std::string &uuid, std::map<std::string, std::string> &device_info, const std::string &onvif_url);
    void sendSearchBroadcast_l(onDevice cb, uint64_t timeout_ms);

private:
    struct onDeviceCB{
        onDevice cb;
        toolkit::Ticker ticker;
        uint64_t timeout_ms;

        bool expired() const;
        bool operator()(std::map<std::string, std::string> &device_info, const std::string &onvif_url);
    };

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    std::vector<toolkit::Socket::Ptr> _sock_list;
    std::unordered_map<std::string/*uuid*/, onDeviceCB> _cb_map;
};

#endif //S3MEDIAKIT_ONVIF_H
