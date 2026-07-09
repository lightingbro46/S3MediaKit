#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Util/logger.h"

#include "Camera/GenericRtspCameraImp.h"
#include "Camera/SdCardSyncManager.h"
#include "Manager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

void registerHistoricalSDCardSyncApis() {
    api_regist("/media/mserver/device/historical/getSDInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        impl->getSDCardInfo([=](const SockException &ex, SDCardInformation &info) mutable {
                            if (ex) {
                                RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                            } else {
                                val["data"]["type"] = info.type;
                                val["data"]["DataFrom"] = info.DataFrom;
                                val["data"]["DataUntil"] = info.DataUntil;
                                val["data"]["NumberRecordings"] = info.NumberRecordings;
                                auto gap_data = SdCardSyncManager::Instance().makeRemainingSegmentsJson(deviceId);
                                val["data"]["gap"]["data"] = gap_data;
                                val["data"]["gap"]["size"] = gap_data.size();
                                val["data"]["SDSupport"] = info.isSDSupport();
                                invoker(200, headerOut, val.toStyledString());
                            }
                        });
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/historical/getAllSessionsInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        // TODO: Implement SD card synchronization logic in GenericRtspCameraImp
                        val["data"] = SdCardSyncManager::Instance().makeDisconnectSessionsJson(deviceId);
                        invoker(200, headerOut, val.toStyledString());
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/historical/getSessionInfo", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "session_id");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string session_id = allArgs["session_id"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            ret->getOwnerPoller()->async([=]() mutable {
                auto weak_listener = ret->getListener();
                if (auto strong_listener = weak_listener.lock()) {
                    auto impl = dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
                    if (impl) {
                        // TODO: Implement SD card synchronization logic in GenericRtspCameraImp
                        val["data"] = SdCardSyncManager::Instance().makeDisconnectSessionJson(deviceId, session_id);
                        invoker(200, headerOut, val.toStyledString());
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/historical/resetFailedSession", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "session_id");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string session_id = allArgs["session_id"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
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
                        // TODO: Implement SD card synchronization logic in GenericRtspCameraImp
                        val["data"] = SdCardSyncManager::Instance().resetFailedSession(deviceId, session_id);
                        invoker(200, headerOut, val.toStyledString());
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/historical/resetFailedSegment", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "session_id", "stream_id", "segment_index");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string session_id = allArgs["session_id"];
            string stream_id = allArgs["stream_id"];
            int32_t segment_index = allArgs["segment_index"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
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
                        // TODO: Implement SD card synchronization logic in GenericRtspCameraImp
                        val["data"] = SdCardSyncManager::Instance().resetFailedSegment(deviceId, session_id, stream_id, segment_index);
                        invoker(200, headerOut, val.toStyledString());
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/historical/stopSession", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
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
                        // TODO: Implement SD card synchronization logic in GenericRtspCameraImp
                        val["data"] = SdCardSyncManager::Instance().stopSession(deviceId);
                        invoker(200, headerOut, val.toStyledString());
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });

    api_regist("/media/mserver/device/historical/stopSegment", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(ADD_CAMERA_PERMISSION_CODE);
        CHECK_ARGS_("deviceId", "stream_id", "segment_index");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string deviceId = allArgs["deviceId"];
            string stream_id = allArgs["stream_id"];
            int32_t segment_index = allArgs["segment_index"];
            auto ret = findDeviceSource(deviceId, GENERIC_RTSP_CAMERA_SCHEMA);
            if (!ret) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
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
                        // TODO: Implement SD card synchronization logic in GenericRtspCameraImp
                        val["data"] = SdCardSyncManager::Instance().stopSegment(deviceId, stream_id, segment_index);
                        invoker(200, headerOut, val.toStyledString());
                    } else {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device is not a camera");
                    }
                } else {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_OFFLINE, "Device is offline");
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["deviceId"], on_access);
    });
}

} // namespace managerkit