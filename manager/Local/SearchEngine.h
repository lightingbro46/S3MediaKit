#ifndef LOCAL_SEARCHENGINE_H
#define LOCAL_SEARCHENGINE_H

#include <functional>
#include "json/json.h"
#include "Network/Socket.h"
#include "Common/MediaSource.h"
#include "TimeQuery.h"
#ifdef ENABLE_MOTION
#include "MotionSearch.h"
#endif // ENABLE_MOTION

namespace managerkit {

/**
 * High-level search facade that aggregates recording and motion queries
 * and produces JSON responses suitable for the HTTP API layer.
 */
class SearchEngine {
public:
    /**
     * Query recorded time periods (and optionally motion periods) for a stream,
     * merging results from remote media servers that held the camera during the
     * requested time range according to VmsResourceAssignment.
     *
     * @param tuple          Stream identity (vhost / app / stream).
     * @param start_time     Query window start (seconds since epoch).
     * @param end_time       Query window end   (seconds since epoch).
     * @param period_type    0 = flat TimeBlock list  (all streams)
     *                       1 = per-stream TimeRange list
     *                       2 = calendar (date → hours bitmap)
     * @param detail         0 = summary  1 = per-stream detail (type 1 & 2 only)
     * @param include_motion When true, adds "motionPeriods" key to the result.
     * @param jwt_token      JWT bearer token forwarded to remote peers for auth.
     * @param cb             Invoked once with (SockException, merged Json::Value).
     */
    static void findTimePeriod(
        const mediakit::MediaTuple &tuple,
        uint64_t start_time, uint64_t end_time,
        int period_type, int detail, bool include_motion,
        const std::string &jwt_token,
        const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb);

    /**
     * Search bookmarks across the entire cluster using the ESC-synced bookmark_index.
     *
     * The index is queried locally (it is replicated to every node via the sync
     * protocol), then full bookmark detail is fetched from the owning node for
     * each page of results — one HTTP call per distinct remote owner, zero extra
     * calls for locally-owned bookmarks.
     *
     * @param camera_id   Filter by camera GUID (empty = all cameras).
     * @param start_time  Query window start (seconds since epoch).
     * @param end_time    Query window end   (seconds since epoch).
     * @param search      Free-text filter applied locally against bookmark names
     *                    (applied after fetching detail; for full FTS use the
     *                     single-node endpoint instead).
     * @param user_id     Filter by creator GUID (empty = all users).
     * @param page        1-based page number.
     * @param size        Items per page (> 0).
     * @param sort        "ASC" or "DESC" ordered by start_time.
     * @param jwt_token   JWT bearer token forwarded to remote peers for auth.
     * @param cb          Invoked once with the merged JSON:
     *                    {
     *                      "data":        [ { bookmark detail }, ... ],
     *                      "currentPage": N,
     *                      "totalItems":  N,
     *                      "totalPages":  N,
     *                      "partial":     false   // true when ≥1 remote peer failed
     *                    }
     */
    static void findBookmarks(
        const std::string &camera_id,
        int64_t start_time, int64_t end_time,
        const std::string &search,
        const std::string &user_id,
        int page, int size, const std::string &sort,
        const std::string &jwt_token,
        const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb);

    /**
     * Fetch full bookmark detail for a list of GUIDs stored on this node.
     * Returns a JSON object { "data": [ { bookmark detail }, ... ] }.
     *
     * @param guids  List of bookmark GUIDs to fetch.
     * @param cb     Invoked once with (SockException, Json::Value).
     */
    static void getBookmarkDetail(
        const std::vector<std::string> &guids,
        const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb);

    /**
     * Search for recent bookmarks created after since_time for a given camera.
     * Returns a JSON object { "data": [ { bookmark detail }, ... ] }.
     */
    static void findRecentBookmarks(
        const std::string &camera_id,
        const std::string &user_id,
        int size,
        const std::string &sort,
        const std::string &jwt_token,
        const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb);

    /**
     * Find the owner node for a bookmark by GUID (looks up bookmark_index in ESC DB).
     * Returns {peer_id, peer_url}; peer_url is empty when owner is this node.
     */
    static std::pair<std::string, std::string> findOwnerNodeForBookmark(
        const std::string &bookmark_guid);

    /**
     * Find the node that owns the recording of camera_id at pos_time,
     * based on VmsResourceAssignment. Uses the most recent assignment active
     * at pos_time (assigned_at <= pos_time AND released_at == 0 OR >= pos_time).
     *
     * @return {peer_id, peer_url}: peer_url is empty when the owner is this node
     *         or when no assignment record exists (treat as local).
     */
    static std::pair<std::string, std::string> findOwnerNodeAtTime(
        const std::string &camera_id, int64_t pos_time);

    /**
     * Find the node that currently owns camera_id (i.e. the latest assignment
     * regardless of whether it has been released).
     *
     * @return {peer_id, peer_url}: peer_url is empty when owner is this node.
     */
    static std::pair<std::string, std::string> findCurrentOwnerNode(
        const std::string &camera_id);

    /**
     * Search for motion periods filtered by a ROI mask within [start_time, end_time].
     *
     * @param roi_mask  String of length rows*cols (e.g. 32*44 = 1408 chars).
     *                  Each char '0' or '1': '0' = exclude cell, '1' = include cell.
     *                  Passing an empty string returns all motion periods.
     * @param cb        Invoked with the JSON response:
     *                  { "cameraId": "...", "motionPeriods": [{"startTime":..., "duration":...}] }
     */
    static void findMotionPeriodByRoi(
        const mediakit::MediaTuple &tuple,
        uint64_t start_time, uint64_t end_time,
        const std::string &roi_mask,
        const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb);
};

} // namespace managerkit

#endif // LOCAL_SEARCHENGINE_H
