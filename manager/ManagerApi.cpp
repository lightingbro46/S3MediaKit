#include "../server/WebApi.h"
#include "ManagerApi.h"

void handleServerResourceJson(Json::Value &data) {
    // if (data.isMember("mediaServer")) {

    // }

    // if (data.isMember("devices") && data["devices"].isArray()) {
    //     for (auto &dev : data["devices"]) {
    //         auto device_json = dev;
    //         addCameraResource(device_json, [](const string &camera_id, const string &stream_id, const string &url) {
    //             auto tuple = MediaTuple { DEFAULT_VHOST, camera_id, stream_id, "" };
    //             mINI args;
    //             args["vhost"] = DEFAULT_VHOST;
    //             args["app"] = tuple.app;
    //             args["stream_id"] = tuple.stream;
    
    //             ProtocolOption option;
    
    //             std::cout << "Add stream proxy: "<< tuple.app << "/" << tuple.stream << " " << url << std::endl;
    //             addStreamProxy(tuple, url, 0, option, Rtsp::RTP_TCP, 10.0, args, [](const SockException &ex, const string &key) {
    //                 if (ex) {
    //                     WarnL << "Add stream failed: " << ex.what();
    //                 } else {
    //                     InfoL << "Add stream success: " << key;
    //                 }
    //             });
    //         });
    //     }
    // }

    // todo: delCameraResource
}

void getServerStatisticJson(const std::function<void(Json::Value &val)> &cb) {

}