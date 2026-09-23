#include "Common/config.h"
#include "Util/logger.h"
#include "Util/MD5.h"
#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Control/SubnetScan.h"
#include "Speaker/SpeakerController.h"
#include "Camera/VideoConfigJobManager.h"
#include "Manager.h"
#include "ApiWorkDispatcher.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

// Subnet scan proxy list
static ServiceController<SubnetScan> s_subnet_scan;

// One interactive device command (PTZ, preset, imaging, relay output). The six endpoints below all
// have the same shape, so they share this dispatcher:
//   - admission through the `device.control` group: every command is a blocking ONVIF round trip,
//     serialized per camera on the `device` pool, so over the limit the client is answered 503 right
//     away instead of getting a reply long after it gave up;
//   - the answer always goes back through the request poller. The camera callbacks run on a
//     `device worker` thread, and calling the invoker from there is what the rest of the API layer
//     carefully avoids;
//   - `require_ownership` covers the endpoints that actually move the camera. As before, the
//     ownership token is only a "nobody else is driving it" check: it is released right away, so
//     commands to the same camera still queue instead of being rejected.
using DeviceCommandCb = std::function<void(const toolkit::SockException &ex)>;
using DeviceCommand = std::function<void(const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb)>;

static void runDeviceCommand(const EventPoller::Ptr &owner, const std::weak_ptr<SocketHelper> &session,
                             HttpSession::KeyValue headerOut, Json::Value val,
                             const HttpSession::HttpResponseInvoker &invoker, const string &device_id,
                             bool require_ownership, const DeviceCommand &command) {
    runApiCallbackWork(owner, session, invoker,
        [headerOut, val, invoker, device_id, require_ownership, command](const ApiReply &reply) mutable {
            // Answers the request once, on the request poller, whatever thread gets here.
            // `mutable` all the way down: a copy captured from a const lambda body is itself const,
            // and RETURN_API_RESPONSE writes into `val`.
            DeviceCommandCb respond = [reply, headerOut, val, invoker](const SockException &ex) mutable {
                reply([headerOut, val, invoker, ex]() mutable {
                    if (ex) {
                        RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                        return;
                    }
                    val["msg"] = ex.what();
                    invoker(200, headerOut, val.toStyledString());
                });
            };

            auto ret = findDeviceSource(device_id);
            if (!ret) {
                respond(SockException(Err_other, "Device not found", ApiErrCode::CODE_DEVICE_NOT_FOUND));
                return;
            }
            if (require_ownership && !ret->getOwnership()) {
                respond(SockException(Err_other, "Device is controlled by other user", ApiErrCode::CODE_DEVICE_OWNERSHIP_BY_OTHER));
                return;
            }
            // The camera methods assert they run on the device's owner poller.
            ret->getOwnerPoller()->async([ret, command, respond]() {
                auto strong_listener = ret->getListener().lock();
                if (!strong_listener) {
                    /* Unreachable */
                    respond(SockException(Err_other, "Device is offline", ApiErrCode::CODE_DEVICE_OFFLINE));
                    return;
                }
                auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                if (!impl) {
                    respond(SockException(Err_other, "Device is not a camera", ApiErrCode::CODE_DEVICE_NOT_FOUND));
                    return;
                }
                command(impl, respond);
            });
        }, apiWorkLimit("device.control"));
}

void registerControlApis() {
    // Register the Web API control endpoints here
    api_regist("/media/mserver/device/discovery", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("address","port", "defaultPort");

        string address = allArgs["address"];

        SubnetScanOption option;
        option.port = allArgs["port"];
        option.defaultPort = allArgs["defaultPort"];
        option.username = (string)allArgs["username"];
        option.password = (string)allArgs["password"];

        auto respond = [val, headerOut, invoker](const SockException &ex, const DeviceScanResult &data) mutable {
            val["data"] = toJsonValue(data);
            if (ex) {
                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                return;
            }
            invoker(200, headerOut, val.toStyledString());
        };

        // Probing a device blocks whatever the address is (ONVIF SOAP calls, or waiting for
        // ffprobe): always run it on the API worker pool and answer from the request poller. The
        // handler never inspects the address to pick a thread -- discovery_device owns that.
        struct DiscoveryResult {
            SockException ex;
            DeviceScanResult data;
        };
        auto result = std::make_shared<DiscoveryResult>();
        auto session = apiRequestSession(sender);
        runApiWork(apiRequestPoller(sender), session,
            [result, option, address]() {
                SubnetScan::discovery_device(option, address,
                    [result](const SockException &ex, const DeviceScanResult &data) {
                        result->ex = ex;
                        result->data = data;
                    });
            },
            [result, respond, session, invoker](const std::exception_ptr &error) mutable {
                if (error) {
                    responseApiException(error, invoker, session);
                    return;
                }
                respond(result->ex, result->data);
            }, apiWorkLimit("device.discovery"));
    });

    api_regist("/media/mserver/device/subnetScan", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("startIp", "endIp", "port", "defaultPort");

        string startIp = allArgs["startIp"];
        string endIp = allArgs["endIp"];
        int port = allArgs["port"];
        bool defaultPort = allArgs["defaultPort"];
        string username = allArgs["username"];
        string password = allArgs["password"];

        if (!SockUtil::is_ipv4(startIp.data()) || !SockUtil::is_ipv4(endIp.data())) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_IP, "startIp or endIp must be a IPv4");
            return;
        }

        SubnetScanOption option;
        option.startIp = startIp;
        option.endIp = endIp;
        option.port = port;
        option.defaultPort = defaultPort;
        option.username = username;
        option.password = password;

        // Synchronous scan of the whole range: the API worker waits while the IPs are probed in
        // parallel (bounded by searcher.max_parallel) on the discovery pool; devices keep IP order.
        dispatchApiWork(sender, headerOut, val, invoker, [option](Json::Value &val) {
            val["data"] = arrayValue;
            for (const auto &device : SubnetScan::scanRange(option)) {
                val["data"].append(toJsonValue(device));
            }
        }, nullptr, apiWorkLimit("device.discovery"));
    });

    static auto addSubnetScan = [](SubnetScanOption &option, const function<void(const SockException &ex, const string &key)> &cb) {
        string full_key = (StrPrinter << option.startIp << "/" << option.endIp << "/" << option.port << "/" << option.username << "/" << option.password);
        string key = MD5(full_key).hexdigest();
        if (s_subnet_scan.find(key)){
            // Already create
            cb(SockException(Err_success), key);
            return;
        }
        auto scanner = s_subnet_scan.make(key, option);
        scanner->setOnClose([key]() { s_subnet_scan.erase(key); });

        scanner->makeScan(key, [cb, key](const SockException &ex) {
            if (ex) {
                s_subnet_scan.erase(key);
            }
            cb(ex, key);
        });
    };

    api_regist("/media/mserver/device/subnetScan/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("startIp", "endIp", "port", "defaultPort");

        string startIp = allArgs["startIp"];
        string endIp = allArgs["endIp"];
        int port = allArgs["port"];
        bool defaultPort = allArgs["defaultPort"];
        string username = allArgs["username"];
        string password = allArgs["password"];
        
        if (!SockUtil::is_ipv4(startIp.data()) || !SockUtil::is_ipv4(endIp.data())) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_IP, "startIp or endIp must be a IPv4");
            return;
        }

        SubnetScanOption option;
        option.startIp = startIp;
        option.endIp = endIp;
        option.port = port;
        option.defaultPort = defaultPort;
        option.username = username;
        option.password = password;

        addSubnetScan(option, [invoker, val, headerOut](const SockException &ex, const string &key) mutable {
            if (ex) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_SUBNETSCAN_FAILED, ex.what());
            } else {
                val["data"]["key"] = key;
                invoker(201, headerOut, val.toStyledString());
            }
        });
    });

    api_regist("/media/mserver/device/subnetScan/progress", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("key");

        std::string key = allArgs["key"];
        auto scanner = s_subnet_scan.find(key);
        if (!scanner) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_SCAN_KEY_NOT_FOUND, "Scan key not found");
            return;
        }

        auto status = scanner->status();
        Json::Value ret = Json::arrayValue;
        for (auto &d : status.result) {
            ret.append(toJsonValue(d));
        }

        val["data"]["finished"] = status.finished;
        val["data"]["progress"] = sanitize_for_json(status.progress);
        val["data"]["devices"] = ret;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/subnetScan/delete", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("key");

        auto scanner = s_subnet_scan.find(allArgs["key"]);
        if (!scanner) {
            val["data"]["flag"] = false;
            return;
        }
        scanner->cancel();
        val["data"]["flag"] = true;
    });

    
    api_regist("/media/mserver/device/ptz_control", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "direct", "speed");

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            int speed = allArgs["speed"];
            runDeviceCommand(owner, session, headerOut, val, invoker, deviceId, true,
                [strDirect, speed](const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb) {
                    impl->PTZMove(strDirect, speed, cb);
                });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/goto_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "presetToken", "isUserPreset");

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            bool isUserPreset = allArgs["isUserPreset"];
            runDeviceCommand(owner, session, headerOut, val, invoker, deviceId, true,
                [presetToken, isUserPreset](const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb) {
                    impl->PTZGotoPreset(presetToken, isUserPreset, cb);
                });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/get_presets", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            // Deliberately not a `device.control` command: this only reads the preset list the
            // controller already cached, with no ONVIF round trip, so it stays synchronous on the
            // request poller and must not be charged against the command budget.
            val["data"] = makeDevicePTZPresetJson(ret);
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/set_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "presetToken", "presetName");

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            string presetName = allArgs["presetName"];
            runDeviceCommand(owner, session, headerOut, val, invoker, deviceId, false,
                [presetToken, presetName](const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb) {
                    impl->addUserPTZPreset(presetToken, presetName, cb);
                });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/remove_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "presetToken", "presetName");

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            string presetName = allArgs["presetName"];
            runDeviceCommand(owner, session, headerOut, val, invoker, deviceId, false,
                [presetToken, presetName](const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb) {
                    impl->removeUserPTZPreset(presetToken, presetName, cb);
                });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/imageMoveControl", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "direct", "speed");

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            int speed = allArgs["speed"];
            runDeviceCommand(owner, session, headerOut, val, invoker, deviceId, true,
                [strDirect, speed](const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb) {
                    impl->ImageMoveControl(strDirect, speed, cb);
                });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/relayOutputControl", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "direct", "token");

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            string relayToken = allArgs["token"];
            runDeviceCommand(owner, session, headerOut, val, invoker, deviceId, true,
                [strDirect, relayToken](const std::shared_ptr<GenericRtspCameraImp> &impl, const DeviceCommandCb &cb) {
                    impl->RelayOutputControl(strDirect, relayToken, cb);
                });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });
    
    api_regist("/media/mserver/device/onvifSetVideoConfigs", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string token = allArgs["token"];
            string encoding = allArgs["encoding"];
            int width = allArgs["width"];
            int height = allArgs["height"];
            float fps = allArgs["fps"];
            int bitrate = allArgs["bitrate"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            auto ownership = ret->getOwnership();
            if (!ownership) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OWNERSHIP_BY_OTHER, "Device is controlled by other user");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        VideoEncoderConfig vConfigSet;
                        vConfigSet.vcodec = encoding;
                        vConfigSet.width = width;
                        vConfigSet.height = height;
                        vConfigSet.bitrate = bitrate;
                        vConfigSet.fps = fps;

                        auto callback = [=](const SockException &ex, VideoEncoderConfig &vOldConfig) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                if (vConfigSet == vOldConfig) {
                                    val["data"]["flag"] = false;
                                    RETURN_API_RESPONSE(ApiErrCode::CODE_ONVIF_SET_CONFIG_NOT_CHANGE, "New config is the same as old config");
                                    return;
                                }

                                impl->setMediaProfile(token, vConfigSet, [=](const SockException &ex) mutable {
                                    if (ex) {
                                        RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                                    } else {
                                        val["msg"] = ex.what();
                                        val["data"]["flag"] = true;
                                        val["data"]["config"]["encoding"] = vConfigSet.vcodec;
                                        val["data"]["config"]["fps"] = vConfigSet.fps;
                                        val["data"]["config"]["bitrate"] = vConfigSet.bitrate;
                                        val["data"]["config"]["resolution"]["width"] = vConfigSet.width;
                                        val["data"]["config"]["resolution"]["height"] = vConfigSet.height;
                                        invoker(200, headerOut, val.toStyledString());
                                    }
                                });
                            }
                        };
                        // load current config and compare with new config, if same then return not change
                        VideoEncoderConfig vConfig;
                        auto stats_imp = impl->getCameraStatisticImp();
                        if (stats_imp) {
                            stats_imp->loadVideoEncoderConfig(token, vConfig);
                            if (!vConfig.vcodec.empty()) {
                                callback(SockException(Err_success), vConfig);
                                return;
                            }
                        }
                        if (vConfig.vcodec.empty()) {
                            WarnL << "Failed to get current video encoder config, use media profile as fallback";
                            impl->getMediaProfile(token, callback);
                        }
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    /* Unreachable */
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/onvifGetConfigProfiles", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                val["data"] = makeDeviceMediaProfileJson(ret);
                invoker(200, headerOut, val.toStyledString());
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/speaker/playAudioFile", [](API_ARGS_JSON_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ARGS_("fileId");

        const auto &json = allArgs.args;
        std::string fileId = json["fileId"].asString();

        auto deviceIds = json["deviceIds"];
        if (!deviceIds.isArray() || deviceIds.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "deviceIds is empty or invalid");
            return;
        }
        for (const auto &deviceId : deviceIds) {
            std::string speakerId = deviceId.asString();
            auto ret = findDeviceSource(speakerId, GENERIC_IP_SPEAKER_SCHEMA);
            if (!ret) {
                WarnL << "Device not found: " << speakerId;
                continue;
            }

            auto ownership = ret->getOwnership();
            if (!ownership) {
                WarnL << "Device is controlled by other user";
                continue;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericIPSpeakerImp>(strong_listener);
                    if (impl) {
                        impl->playAudioFile(speakerId, fileId, [](const SockException &ex) mutable {
                            InfoL << ex.what();
                        });
                    }
                }
            });
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/validateCredential", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            std::string deviceId = allArgs["deviceId"];
            std::string username = allArgs["deviceUsername"];
            std::string password = allArgs["devicePassword"];
            int port = allArgs["devicePort"];

            auto ret = findDeviceSource(deviceId, GENERIC_IP_SPEAKER_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Speaker not found");
                return;
            }

            auto ownership = ret->getOwnership();
            if (!ownership) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OWNERSHIP_BY_OTHER, "Device is controlled by other user");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericIPSpeakerImp>(strong_listener);
                    if (impl) {
                        impl->validateCredential(username, password, port, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a speaker");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    // Applies a time-boxed video encoder override to a batch of cameras; each camera is rolled back
    // to its baseline when the job expires. Accepted per camera; the result of each accepted camera
    // (kBroadcastVideoConfigRuleResult) goes to hook.on_video_config_rule_result and to
    // video_config_rule_result, read back with onvifGetVideoConfigRuleResult.
    // Called service-to-service without a JWT: authenticated by manager.apiSecret only, with no
    // user or per-device permission check.
    api_regist("/media/mserver/device/onvifApplyVideoConfigRule", [](API_ARGS_JSON_ASYNC) {
        CHECK_API_KEY();
        CHECK_ARGS_("logId");

        string logId = allArgs["logId"];
        auto camerasReq = allArgs.args["cameras"];
        if (!camerasReq.isArray() || camerasReq.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "cameras must be a non-empty array");
            return;
        }

        Json::Value results(Json::arrayValue);
        vector<function<void()>> accepted;
        vector<string> acceptedIds;

        for (auto &camReq : camerasReq) {
            string deviceId = camReq.get("deviceId", "").asString();
            Json::Value item;
            item["deviceId"] = deviceId;

            if (deviceId.empty()) {
                item["code"] = ApiErrCode::CODE_INVALID_ARGS;
                item["msg"] = "deviceId is required";
                results.append(item);
                continue;
            }

            if (std::find(acceptedIds.begin(), acceptedIds.end(), deviceId) != acceptedIds.end()) {
                // One result per (logId, deviceId): a second entry would get its own.
                item["code"] = ApiErrCode::CODE_INVALID_ARGS;
                item["msg"] = "Duplicate deviceId in cameras";
                results.append(item);
                continue;
            }

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                item["code"] = ApiErrCode::CODE_DEVICE_NOT_FOUND;
                item["msg"] = "Device not found";
                results.append(item);
                continue;
            }
            auto ownership = ret->getOwnership();
            if (!ownership) {
                item["code"] = ApiErrCode::CODE_DEVICE_OWNERSHIP_BY_OTHER;
                item["msg"] = "Device is controlled by other user";
                results.append(item);
                continue;
            }

            int width         = camReq["width"].asInt();
            int height        = camReq["height"].asInt();
            float fps         = camReq["fps"].asFloat();
            int bitrate       = camReq["bitrate"].asInt();
            int applyDuration = camReq["applyDuration"].asInt();

            if (width <= 0 || height <= 0 || fps <= 0.0F || bitrate <= 0 || applyDuration <= 0) {
                item["code"] = ApiErrCode::CODE_INVALID_ARGS;
                item["msg"] = "Invalid video config or applyDuration";
                results.append(item);
                continue;
            }

            item["code"] = 0;
            item["msg"] = "accepted";
            results.append(item);

            accepted.emplace_back([ret, logId, deviceId, width, height, fps, bitrate, applyDuration]() {
                ret->getOwnerPoller()->async([ret, logId, deviceId, width, height, fps, bitrate, applyDuration]() mutable {
                    // Accepted already: every way out reports a result for (logId, deviceId).
                    auto strong_listener = ret->getListener().lock();
                    if (!strong_listener) {
                        VideoConfigJobManager::reportFailure(logId, deviceId, "DEVICE_OFFLINE", "Device is offline");
                        return;
                    }
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (!impl) {
                        VideoConfigJobManager::reportFailure(logId, deviceId, "NOT_A_CAMERA", "Device is not a camera");
                        return;
                    }
                    auto statistic = impl->getCameraStatisticImp();
                    if (!statistic) {
                        VideoConfigJobManager::reportFailure(logId, deviceId, "DEVICE_OFFLINE", "Camera statistic released");
                        return;
                    }
                    auto token = statistic->getPrimaryProfileToken();
                    if (token.empty()) {
                        VideoConfigJobManager::reportFailure(logId, deviceId, "PROFILE_TOKEN_NOT_FOUND", "Failed to determine the profile token");
                        return;
                    }
                    VideoConfigJobManager::Instance().handleRuleRequest(impl, logId, deviceId, token, width,
                                                                       height, fps, bitrate, applyDuration);
                });
            });
            acceptedIds.push_back(deviceId);
        }

        // PENDING rows are queued before any camera can report, so a result never lands before them.
        VideoConfigJobManager::Instance().recordAccepted(logId, acceptedIds);
        for (auto &start : accepted) {
            start();
        }

        val["code"] = 0;
        val["msg"] = "success";
        val["data"]["results"] = results;
        invoker(200, headerOut, val.toStyledString());
    });

    // Results of an onvifApplyVideoConfigRule request by logId: PENDING until the camera reports.
    // Same authentication as onvifApplyVideoConfigRule.
    api_regist("/media/mserver/device/onvifGetVideoConfigRuleResult", [](API_ARGS_MAP_ASYNC) {
        CHECK_API_KEY();
        CHECK_ARGS("logId");

        string logId = allArgs["logId"];
        // Read on the rule DB strand, after the PENDING rows of every request accepted before.
        runApiCallbackWork(apiRequestPoller(sender), apiRequestSession(sender), invoker,
            [logId, val, invoker, headerOut](const ApiReply &reply) mutable {
                VideoConfigJobManager::Instance().queryResults(logId,
                    [val, invoker, headerOut, reply](const vector<VideoConfigRuleResult> &rows, const string &error) mutable {
                        if (!error.empty()) {
                            val["code"] = static_cast<int>(ApiErrCode::CODE_OTHER_EXCEPTION);
                            val["msg"] = "Failed to read video config rule results";
                            const string body = val.toStyledString();
                            const int status = getStatusCode(ApiErrCode::CODE_OTHER_EXCEPTION);
                            reply([invoker, headerOut, body, status]() { invoker(status, headerOut, body); });
                            return;
                        }
                        Json::Value items(Json::arrayValue);
                        for (const auto &row : rows) {
                            items.append(row.toJson());
                        }
                        val["code"] = 0;
                        val["msg"] = "success";
                        val["data"]["results"] = items;
                        const string body = val.toStyledString();
                        reply([invoker, headerOut, body]() { invoker(200, headerOut, body); });
                    });
            });
    });

    DebugL << "Control APIs registered";
}


void unregisterControlApis() {
    s_subnet_scan.clear();

    DebugL << "Control APIs unregistered";
}

} // namespace managerkit
