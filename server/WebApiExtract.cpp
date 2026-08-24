#include "WebApi.h"
#include "WebApiErrCode.h"

#include "Common/config.h"
#include "Util/MD5.h"
#include "Manager.h"
#include "Local/ExtractJobManager.h"

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
            bool enable_source_stamp = allArgs["stampSource"];

            if (!findDeviceSource(camera_id)) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            if (!end_with(filename, ".mp4") && !end_with(filename, ".mkv") && !end_with(filename, ".avi")) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_EXTENSION, "Only support file extension: .mp4, .mkv, .avi");
                return;
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
            ExtractOptions options = { start_time, end_time, filename, description, user_id, user_name, jwt_token, enable_source_stamp };

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

        auto status = ffmpeg->status();
        if (status.finished && !status.success) {
            auto err_detail = "Extract video failed: " + status.err_msg;
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_FAILED, err_detail.data());
            return;
        }

        val["data"]["progress"] = sanitize_for_json(status.progress);
        val["data"]["ready"] = status.finished && status.success;
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/extractArchived/download", [](API_ARGS_MAP_ASYNC) {
        CHECK_USER_AUTHOR("key");

        auto key = allArgs["key"];
        auto ffmpeg = s_ffmpeg_extractor.find(allArgs["key"]);
        if (!ffmpeg) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_KEY_NOT_FOUND, "Key not found");
            return;
        }
        auto status = ffmpeg->status();
        if (!status.finished || !status.success) {
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
            auto status = src->status();
            item["progress"] = status.progress;
            item["ready"] = status.finished && status.success;
            val["data"].append(item);
        });
    });

    api_regist("/media/esc/extractArchived/job", [](API_ARGS_JSON_ASYNC) {
        CHECK_API_KEY();
        CHECK_ARGS_("fileId", "cameraId", "startTime", "endTime", "uploadUrl", "format");

        const Json::Value &request = allArgs.args;
        if (!request.isObject() || !request["fileId"].isString() || !request["cameraId"].isString() ||
            !request["startTime"].isIntegral() || !request["endTime"].isIntegral() ||
            !request["uploadUrl"].isString() || !request["format"].isString() ||
            (request.isMember("streamId") && !request["streamId"].isString())) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "Invalid extract job field types");
            return;
        }

        ExtractJobSubmitRequest submit_request;
        submit_request.file_id = request["fileId"].asString();
        submit_request.camera_id = request["cameraId"].asString();
        submit_request.stream_id = request.get("streamId", "").asString();
        submit_request.start_time = request["startTime"].asInt64();
        submit_request.end_time = request["endTime"].asInt64();
        submit_request.upload_url = request["uploadUrl"].asString();
        submit_request.format = request["format"].asString();

        if (submit_request.file_id.empty() ||
            submit_request.camera_id.empty() || submit_request.camera_id.size() > 256 ||
            submit_request.stream_id.size() > 256 || submit_request.upload_url.size() > 8192) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "Invalid fileId, cameraId");
            return;
        }

        GET_CONFIG(int, max_duration_sec, ExtractJobConfig::kMaxDurationSec);
        const int64_t duration = submit_request.end_time - submit_request.start_time;
        if (submit_request.start_time <= 0 || duration <= 0 || (max_duration_sec > 0 && duration > max_duration_sec)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_TIME_RANGE, "Invalid time range");
            return;
        }
        if (submit_request.format != "mp4" && submit_request.format != "mkv" && submit_request.format != "avi") {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_EXTENSION, "Only mp4, mkv, and avi formats are supported");
            return;
        }
        if (!ExtractJobManager::isAllowedUploadUrl(submit_request.upload_url)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "uploadUrl must be an allowed HTTPS URL without userinfo");
            return;
        }

        if (!findDeviceSource(submit_request.camera_id, GENERIC_RTSP_CAMERA_SCHEMA)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
            return;
        }

        auto result = ExtractJobManager::Instance().submit(submit_request);
        if (result.result == ExtractJobRepository::CreateResult::CONFLICT) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_JOB_CONFLICT, "fileId already exists with a different payload");
            return;
        }
        if (result.result == ExtractJobRepository::CreateResult::FAILED) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_JOB_CREATE_FAILED, "Failed to persist extract job");
            return;
        }

        val["data"]["fileId"] = result.job.file_id;
        val["data"]["status"] = result.job.status;
        invoker(202, headerOut, val.toStyledString());
    });

    DebugL << "Extraction APIs registered";
}


void unregisterExtractionApis() {
    s_ffmpeg_extractor.clear();

    DebugL << "Extraction APIs unregistered";
}

} // namespace managerkit