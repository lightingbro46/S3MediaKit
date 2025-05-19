// #include "Camera.h"
// #include "Util/logger.h"

// // Get url from camera
// std::string getUrl(const CameraTuple &camera) {
//     std::string url;
//     if (camera.use_https) {
//         url = HTTPS_PROTOCOL_SCHEMA"://" + camera.ip + (camera.sslport != 0 ? ":" + std::to_string(camera.sslport) : "") + camera.path;
//     } else {
//         url = HTTP_PROTOCOL_SCHEMA"://" + camera.ip + (camera.port != 0 ? ":" + std::to_string(camera.port) : "") + camera.path;
//     }
//     return url;
// }

// // Get stream url from camera and stream
// std::string getStreamUrl(const CameraTuple &camera, const StreamTuple &stream) {
//     if (stream.protocol.empty()) {
//         WarnL << "Protocol of stream "<< camera.id << ":"<< stream.id << " is empty";
//         return "";
//     }

//     std::string url;
//     url = stream.protocol + "://";
//     if (!camera.username.empty()) {
//         url += camera.username + ":" + camera.password + "@";
//     }
//     url += camera.ip;
//     if (stream.protocol == RTSP_PROTOCOL_SCHEMA) {
//         url += camera.rtsp_port != 0 ? ":" + std::to_string(camera.rtsp_port) : "";
//     } else if (stream.protocol == RTSPS_PROTOCOL_SCHEMA) {
//         url += camera.rtsp_sslport != 0 ? ":" + std::to_string(camera.rtsp_sslport) : "";
//     } else if (stream.protocol == RTMP_PROTOCOL_SCHEMA) {
//         url += camera.rtmp_port != 0 ? ":" + std::to_string(camera.rtmp_port) : "";
//     } else if (stream.protocol == RTMPS_PROTOCOL_SCHEMA) {
//         url += camera.rtmp_sslport != 0 ? ":" + std::to_string(camera.rtmp_sslport) : "";
//     }
//     return url + stream.path;
// }

// CameraProxy::CameraProxy(const std::string &guid, const CameraTuple &tuple, const CameraOption &option) {
        
// };

// CameraProxy::~CameraProxy() {
//     disconnect();
// }

// void CameraProxy::connect() {
//     if (_on_connect) {
//         _on_connect(_id);
//     }
// }

// void CameraProxy::disconnect() {
//     if (_on_disconnect) {
//         _on_disconnect();
//     }
// }


