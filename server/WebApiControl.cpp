#include "Common/config.h"
#include "Util/logger.h"
#include "Util/MD5.h"
#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Control/SubnetScan.h"
#include "Manager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

// Subnet scan proxy list
static ServiceController<SubnetScan> s_subnet_scan;

void registerControlApis() {
    // Register the Web API control endpoints here
    api_regist("/media/mserver/device/discovery", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("address","port", "defaultPort");

        string address = allArgs["address"];
        int port = allArgs["port"];
        bool defaultPort = allArgs["defaultPort"];
        string username = allArgs["username"];
        string password = allArgs["password"];

        SubnetScan::discovery_device(address, port, defaultPort, username, password, [=](const SockException &ex, const DeviceScanResult &data) mutable {
            if (ex) {
                val["data"] = toJsonValue(data);
                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                return;
            }
            val["data"] = toJsonValue(data);
            invoker(200, headerOut, val.toStyledString());
        });
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

        val["data"] = arrayValue;
        auto ip_range = SockUtil::get_ipv4_range(startIp, endIp);
        for (auto &ip : ip_range) {
            SubnetScan::discovery_device(ip, port, defaultPort, username, password, [&](const SockException &ex, const DeviceScanResult &data) {
                if (!ex) {
                    val["data"].append(toJsonValue(data));
                }
            });
        }
        invoker(200, headerOut, val.toStyledString());
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
        string jwt_token = allArgs["_jwt_token"];
        
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

        addSubnetScan(option, [invoker, val, headerOut, jwt_token](const SockException &ex, const string &key) mutable {
            if (ex) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_SUBNETSCAN_FAILED, ex.what());
            } else {
                UserAuthorManager::Instance().addAuthorCache(key, jwt_token, true, 600);
                val["data"]["key"] = key;
                invoker(201, headerOut, val.toStyledString());
            }
        });
    });

    api_regist("/media/mserver/device/subnetScan/progress", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        std::string key = allArgs["key"];
        auto scanner = s_subnet_scan.find(key);
        if (!scanner) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_SCAN_KEY_NOT_FOUND, "Scan key not found");
            return;
        }

        bool isFinished = scanner->finished();
        double  progress = scanner->progress();
        auto devices = scanner->result();
        Json::Value ret = Json::arrayValue;
        for (auto &d : devices) {
            ret.append(toJsonValue(d));
        }

        val["data"]["finished"] = isFinished;
        val["data"]["progress"] = progress;
        val["data"]["devices"] = ret;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/subnetScan/delete", [](API_ARGS_MAP) {
        CHECK_USER_AUTHOR("key");

        val["data"]["flag"] = s_subnet_scan.erase(allArgs["key"]) == 1;
    });

    
    api_regist("/media/mserver/device/ptz_control", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "direct", "speed");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            int speed = allArgs["speed"];

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
                        impl->PTZMove(strDirect, speed, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
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

    api_regist("/media/mserver/device/ptz_control/goto_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "presetToken", "isUserPreset");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            bool isUserPreset = allArgs["isUserPreset"];

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
                        impl->PTZGotoPreset(presetToken, isUserPreset, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
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

            val["data"] = makeDevicePTZPresetJson(ret);
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/ptz_control/set_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "presetToken", "presetName");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            string presetName = allArgs["presetName"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->addUserPTZPreset(presetToken, presetName, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
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

    api_regist("/media/mserver/device/ptz_control/remove_preset", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "presetToken", "presetName");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string presetToken = allArgs["presetToken"];
            string presetName = allArgs["presetName"];

            auto ret = findDeviceSource(deviceId);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->removeUserPTZPreset(presetToken, presetName, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
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

    api_regist("/media/mserver/device/imageMoveControl", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "direct", "speed");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            int speed = allArgs["speed"];

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
                        impl->ImageMoveControl(strDirect, speed, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
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

    api_regist("/media/mserver/device/relayOutputControl", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PTZ_CONTROL_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "direct", "token");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string strDirect = allArgs["direct"];
            string relayToken = allArgs["token"];

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
                        impl->RelayOutputControl(strDirect, relayToken, [=](const SockException &ex) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["msg"] = ex.what();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
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

    DebugL << "Control APIs registered";
}


void unregisterControlApis() {
    s_subnet_scan.clear();

    DebugL << "Control APIs unregistered";
}

} // namespace managerkit