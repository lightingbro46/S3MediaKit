#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Common/config.h"
#include "Local/SearchEngine.h"
#include "Storage/Bookmark.h"
#include "Manager.h"
#include "Util/base64.h"
#include "Http/HttpClient.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

void registerBookmarkApis() {
    // Register the Web API bookmark endpoints here
    api_regist("/media/esc/bookmark/search", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("start_time", "end_time", "page", "size", "sort");

        string camera_id   = allArgs["camera_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time   = allArgs["end_time"];
        string search      = allArgs["search"];
        int page           = MAX((int)allArgs["page"], 0);
        int size           = MAX((int)allArgs["size"], 1);
        string sort        = allArgs["sort"];
        string user_id     = allArgs["_user_id"];
        string jwt_token   = allArgs["_jwt_token"];
        bool edge          = allArgs["edge"];

        SearchEngine::findBookmarks(camera_id, start_time, end_time, search, user_id,
            page, size, sort, jwt_token, edge,
            [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
                if (ex) {
                    val["code"] = -1;
                    val["msg"]  = ex.what();
                    invoker(500, headerOut, val.toStyledString());
                    return;
                }
                val["data"]        = data["data"];
                val["currentPage"] = data["currentPage"];
                val["totalItems"]  = data["totalItems"];
                val["totalPages"]  = data["totalPages"];
                val["partial"]     = data["partial"];
                invoker(200, headerOut, val.toStyledString());
            });
    });

    // Internal endpoint: fetch full bookmark detail for a comma-separated list of
    // bookmark GUIDs stored on this node.  Called by remote aggregators via
    // SearchEngine::findBookmarks(); should not be exposed to external clients.
    api_regist("/media/esc/bookmark/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ARGS_("ids");

        string ids_str = allArgs["ids"];
        auto   guids   = toolkit::split(ids_str, ",");
        bool   edge      = allArgs["edge"];

        SearchEngine::getBookmarkDetail(guids, edge, [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
            if (ex) {
                val["code"] = -1;
                val["msg"]  = ex.what();
                invoker(500, headerOut, val.toStyledString());
                return;
            }
            val["data"] = data["data"];
            invoker(200, headerOut, val.toStyledString());
        });
    });

    api_regist("/media/esc/bookmark/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("name", "camera_id", "start_time", "duration");

        string camera_id = allArgs["camera_id"];
        bool edge        = allArgs["edge"];
        
        if (!edge) {
            // Forward to the camera owner node if it is not this node.
            auto owner = SearchEngine::findCurrentOwnerNode(camera_id);
            if (!owner.second.empty()) {
                HttpArgs fwd_body;
                fwd_body["name"]        = (string)allArgs["name"];
                fwd_body["description"] = (string)allArgs["description"];
                fwd_body["camera_id"]   = (string)allArgs["camera_id"];
                fwd_body["start_time"]  = (string)allArgs["start_time"];
                fwd_body["end_time"]    = (string)allArgs["end_time"];
                fwd_body["duration"]    = (string)allArgs["duration"];
                fwd_body["tags"]        = (string)allArgs["tags"];
                string jwt_token = allArgs["_jwt_token"];

                Broadcast::OnResInvoker on_response = [val, invoker, headerOut](const string &err, const int&, const Json::Value &res) mutable {
                    if (!err.empty()) {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_CREATE_FAILED, err);
                        return;
                    }
                    invoker(200, headerOut, res.toStyledString());
                };
                NOTICE_EMIT(BroadcastSyncBookmarkCreateOrUpdateArgs, Broadcast::kBroadcastSyncBookmarkCreateOrUpdate, owner.second, fwd_body, jwt_token, on_response, true);
                return;
            }
        }

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string name = allArgs["name"];
            string description = allArgs["description"];
            string camera_id = allArgs["camera_id"];
            int64_t start_time = allArgs["start_time"];
            int64_t end_time = allArgs["end_time"];
            int64_t duration = allArgs["duration"];
            string tags = allArgs["tags"];
            string user_id = allArgs["_user_id"];

            auto ret = findDeviceSource(camera_id);
            if (!ret) {
                val["data"]["flag"] = false;
                RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                return;
            }

            Bookmark bm;
            bm.name = name;
            bm.description = description;
            bm.camera_guid = camera_id;
            bm.start_time = start_time;
            bm.end_time = end_time;
            bm.duration = duration;
            bm.creator_guid = user_id;
            bm.created = time(nullptr);

            auto imp = std::make_shared<BookmarkImp>();
            imp->add(bm, tags);

            val["data"]["flag"] = true;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(camera_id, on_access);
    });

    api_regist("/media/esc/bookmark/update", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("id", "camera_id", "start_time", "duration");

        string bookmark_id = allArgs["id"];
        bool edge          = allArgs["edge"];

        if (!edge) {
            // Forward to the bookmark's owner node if it is not this node.
            auto owner = SearchEngine::findOwnerNodeForBookmark(bookmark_id);
            if (!owner.second.empty()) {
                HttpArgs fwd_body;
                fwd_body["id"]          = bookmark_id;
                fwd_body["name"]        = (string)allArgs["name"];
                fwd_body["description"] = (string)allArgs["description"];
                fwd_body["camera_id"]   = (string)allArgs["camera_id"];
                fwd_body["start_time"]  = (string)allArgs["start_time"];
                fwd_body["end_time"]    = (string)allArgs["end_time"];
                fwd_body["duration"]    = (string)allArgs["duration"];
                fwd_body["tags"]        = (string)allArgs["tags"];
                string jwt_token        = allArgs["_jwt_token"];

                Broadcast::OnResInvoker on_response = [val, invoker, headerOut](const string &err, const int&, const Json::Value &res) mutable {
                    if (!err.empty()) {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_UPDATE_FAILED, err);
                        return;
                    }
                    invoker(200, headerOut, res.toStyledString());
                };
                NOTICE_EMIT(BroadcastSyncBookmarkCreateOrUpdateArgs, Broadcast::kBroadcastSyncBookmarkCreateOrUpdate, owner.second, fwd_body, jwt_token, on_response, false);
                return;
            }
        }

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string id = allArgs["id"];
            string name = allArgs["name"];
            string description = allArgs["description"];
            string camera_id = allArgs["camera_id"];
            int64_t start_time = allArgs["start_time"];
            int64_t end_time = allArgs["end_time"];
            int64_t duration = allArgs["duration"];
            string tags = allArgs["tags"];
            string user_id = allArgs["_user_id"];

            auto imp = std::make_shared<BookmarkImp>();
            auto ret = imp->findById(id);
            if (!ret.size()) {
                WarnL << "Bookmark " << id << " not found";
                val["data"]["flag"] = false;
                RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
                return;
            }

            Bookmark bm = ret[0];
            bm.name = name;
            bm.description = description;
            bm.camera_guid = camera_id;
            bm.start_time = start_time;
            bm.end_time = end_time;
            bm.duration = duration;
            bm.creator_guid = user_id;
            bm.created = time(nullptr);

            imp->update(bm, tags);
            val["data"]["flag"] = true;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["camera_id"], on_access);
    });

    api_regist("/media/esc/bookmark/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string id = allArgs["id"];
        bool edge = allArgs["edge"];

        if (!edge) {
            // Forward to the bookmark's owner node if it is not this node.
            auto owner = SearchEngine::findOwnerNodeForBookmark(id);
            if (!owner.second.empty()) {
                string jwt_token = allArgs["_jwt_token"];
                Broadcast::OnResInvoker on_response = [val, invoker, headerOut](const string &err, const int&, const Json::Value &res) mutable {
                    if (!err.empty()) {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_DELETE_FAILED, err);
                        return;
                    }
                    invoker(200, headerOut, res.toStyledString());
                };
                NOTICE_EMIT(BroadcastSyncBookmarkDeleteArgs, Broadcast::kBroadcastSyncBookmarkDelete, owner.second, id, jwt_token, on_response);
                return;
            }
        }

        // Local: find in per-node DB to get camera_guid for auth check.
        auto imp = std::make_shared<BookmarkImp>();
        auto bm_ret = imp->findById(id);
        if (!bm_ret.size()) {
            WarnL << "Bookmark " << id << " not found";
            val["data"]["flag"] = false;
            RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
            return;
        }

        string camera_guid = bm_ret[0].camera_guid;
        auto on_access = [val, invoker, headerOut, imp, id]() mutable {
            imp->remove(id);
            val["data"]["flag"] = true;
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(camera_guid, on_access);
    });

    api_regist("/media/esc/bookmark/mostUsedTags", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("size");

        int size = allArgs["size"];
        if (size < 0) size = 1;
        
        auto imp = std::make_shared<BookmarkTagCountImp>();
        auto tags = imp->findTagsByCountDesc(size);

        val["data"] = arrayValue;
        for (const auto &tag : tags) {
            val["data"].append(tag);
        }
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/esc/bookmark/recent", [](API_ARGS_MAP_ASYNC) { 
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("size", "sort"); 

        string camera_id = allArgs["camera_id"];
        int size = MAX((int)allArgs["size"], 1);
        string sort = allArgs["sort"];
        string user_id = allArgs["_user_id"];
        string jwt_token = allArgs["_jwt_token"];
        bool edge = allArgs["edge"];
        
        SearchEngine::findRecentBookmarks(camera_id, user_id, size, sort, jwt_token, edge,
            [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
                if (ex) {
                    val["code"] = -1;
                    val["msg"]  = ex.what();
                    invoker(500, headerOut, val.toStyledString());
                    return;
                }
                val["data"]        = data["data"];
                val["currentPage"] = data["currentPage"];
                val["totalItems"]  = data["totalItems"];
                val["totalPages"]  = data["totalPages"];
                val["partial"]     = data["partial"];
                invoker(200, headerOut, val.toStyledString());
            });
    });

    api_regist("/media/esc/bookmark/recordThumbnail", [](API_ARGS_MAP_ASYNC) { 
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string bookmark_id = allArgs["id"];
        bool edge = allArgs["edge"];

        if (!edge) {
            // Forward to the bookmark's owner node if it is not this node.
            auto owner = SearchEngine::findOwnerNodeForBookmark(bookmark_id);
            if (!owner.second.empty()) {
                string jwt_token = allArgs["_jwt_token"];
                NOTICE_EMIT(BroadcastSyncBookmarkThumbnailArgs, Broadcast::kBroadcastSyncBookmarkThumbnail, owner.second, bookmark_id, jwt_token, invoker);
                return;
            }
        }

        // Local: load bookmark from per-node DB.
        auto imp = std::make_shared<BookmarkImp>();
        auto ret = imp->findById(bookmark_id);
        if (!ret.size()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
            return;
        }
        const auto &bm = ret[0];
        string device_id = bm.camera_guid;
        auto on_access = [allArgs, val, invoker, headerOut, bm]() mutable {
            string camera_id = bm.camera_guid;
            string stream_id = ""; // TODO: support stream id in bookmark
            uint64_t start_time = (uint64_t)bm.start_time;
            uint64_t diff_time = 0;

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
            TimeQuery::Ptr query;
            try {
                query = std::make_shared<TimeQuery>(tuple);
            } catch(...) {}

            string src_path;
            if (query) {
                auto block = query->getLastBlock(start_time);
                if (block) {
                    src_path = decodeBase64(block->file_path());
                    diff_time = start_time > block->start_time() ? start_time - block->start_time() : 0;
                }
            }

            if (src_path.empty()) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_TIMELINE_NOT_FOUND, "No data in period");
                return;
            }

            GET_CONFIG(string, snap_root, API::kSnapRoot);
            string snap_path = StrPrinter << File::absolutePath(camera_id + "/" + stream_id, snap_root) << "/" << start_time << ".jpeg";

            FFmpegSnap::makeSnap(false, src_path, snap_path, diff_time, 2, [invoker, val, headerOut, snap_path](bool success, const string &err_msg) mutable {
                if (!success) {
                    RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY, err_msg.data());
                    return;
                }
                invoker(200, StrCaseMap {}, snap_path);
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, on_access);
    });

    DebugL << "Bookmark APIs registered";
}

} // namespace managerkit