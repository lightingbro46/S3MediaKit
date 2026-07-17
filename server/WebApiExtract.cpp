#include "WebApi.h"
#include "WebApiErrCode.h"

#include "Common/config.h"
#include "Util/MD5.h"
#include "Manager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

// FFmpeg extractor proxy list
static ServiceController<FFmpegExtractor> s_ffmpeg_extractor;

void registerExtractionApis() {
    // Register the Web API extraction endpoints here
    static auto addFFmpegExtractor = [](MediaTuple &tuple, ExtractOptions &options, managerkit::UserSessionCache::Ptr &session, const function<void(const SockException &ex, const string &key)> &cb) {
        auto full_key = tuple.shortUrl() + "/" + to_string(options.start_time) + "/" + to_string(options.end_time) + "/" + options.filename;
        auto key = MD5(full_key).hexdigest();
        if (s_ffmpeg_extractor.find(key)) {
            // Already create
            cb(SockException(Err_success), key);
            return;
        }

        auto ffmpeg = s_ffmpeg_extractor.make(key, tuple, options);

        ffmpeg->setSessionCache(session);
        ffmpeg->setOnClose([key]() { s_ffmpeg_extractor.erase(key); });

        GET_CONFIG(string, extract_path, API::kExtractRoot)
        ffmpeg->makeExtract(key, extract_path, [cb, key](const SockException &ex) {
            if (ex) {
                s_ffmpeg_extractor.erase(key);
            }
            cb(ex, key);
        });
    };

    api_regist("/media/esc/extractArchived/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(EXTRACT_PERMISSION_CODE);
        CHECK_ARGS_("cameraId", "startTime", "endTime", "filename");

        auto on_access = [allArgs, val, invoker, headerOut, token_cache]() mutable {
            auto camera_id = allArgs["cameraId"];
            auto stream_id = allArgs["streamId"];
            auto start_time = allArgs["startTime"];
            auto end_time = allArgs["endTime"];
            auto filename = allArgs["filename"];
            auto description = allArgs["description"];
            auto user_id = allArgs["_user_id"];
            auto user_name = allArgs["_user_name"];
            auto jwt_token = allArgs["_jwt_token"];

            if (!findDeviceSource(camera_id)) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            if (!end_with(filename, ".mp4") && !end_with(filename, ".mkv") && !end_with(filename, ".avi")) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_EXTENSION, "Only support file extension: .mp4, .mkv, .avi");
                return;
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
            ExtractOptions options = { start_time, end_time, filename, description, user_id, user_name };

            addFFmpegExtractor(tuple, options, token_cache, [invoker, val, headerOut, jwt_token](const SockException &ex, const string &key) mutable {
                if (ex) {
                    RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                } else {
                    UserAuthorManager::Instance().addAuthorCache(key, jwt_token, true, 600);
                    val["data"]["key"] = key;
                    invoker(201, headerOut, val.toStyledString());
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    api_regist("/media/esc/extractArchived/progress", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        auto ffmpeg = s_ffmpeg_extractor.find(allArgs["key"]);
        if (!ffmpeg) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_KEY_NOT_FOUND, "Key not found");
            return;
        }

        if (ffmpeg->finished() && !ffmpeg->success()) {
            auto err_detail = "Extract video failed: " + ffmpeg->errMsg();
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_FAILED, err_detail.data());
            return;
        }

        val["data"]["progress"] = ffmpeg->progress();
        val["data"]["ready"] = ffmpeg->finished() && ffmpeg->success() ? true : false;
        invoker(202, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/extractArchived/download", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        auto key = allArgs["key"];
        auto ffmpeg = s_ffmpeg_extractor.find(allArgs["key"]);
        if (!ffmpeg || !ffmpeg->finished() || !ffmpeg->success()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_KEY_NOT_FOUND, "Key not found");
            return;
        }

        StrCaseMap res_header;
        auto save_name = ffmpeg->getFilename();
        auto save_path = ffmpeg->getSavePath();
        if (!save_name.empty()) {
            res_header.emplace("Content-Disposition", "attachment;filename=\"" + save_name + "\"");
        }
        invoker.responseFile(allArgs.parser.getHeader(), res_header, save_path);
    });

    api_regist("/media/esc/extractArchived/delete", [](API_ARGS_MAP) {
        CHECK_USER_AUTHOR("key");

        val["data"]["flag"] = s_ffmpeg_extractor.erase(allArgs["key"]) == 1;
    });

    api_regist("/media/esc/extractArchived/list", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        auto jwt_token = allArgs["_jwt_token"];

        s_ffmpeg_extractor.for_each([&](const std::string &key, const FFmpegExtractor::Ptr &src) {
            if (!checkUserAuthor(key, jwt_token)) {
                return;
            }
            Json::Value item;
            item["key"] = key;
            item["progress"] = src->progress();
            item["ready"] = src->finished() && src->success() ? true : false;
            val["data"].append(item);
        });
    });

    DebugL << "Extraction APIs registered";
}


void unregisterExtractionApis() {
    s_ffmpeg_extractor.clear();

    DebugL << "Extraction APIs unregistered";
}

} // namespace managerkit