#ifndef LOCAL_SEARCHENGINE_H
#define LOCAL_SEARCHENGINE_H

#include <functional>
#include "json/json.h"
#include "Network/Socket.h"
#include "Common/MediaSource.h"
#include "TimeQuery.h"
#include "MotionSearch.h"

namespace managerkit {

/**
 * High-level search facade that aggregates recording and motion queries
 * and produces JSON responses suitable for the HTTP API layer.
 */
class SearchEngine {
public:
    /**
     * Query recorded time periods (and optionally motion periods) for a stream.
     *
     * @param tuple          Stream identity (vhost / app / stream).
     * @param start_time     Query window start (seconds since epoch).
     * @param end_time       Query window end   (seconds since epoch).
     * @param period_type    0 = flat TimeBlock list  (all streams)
     *                       1 = per-stream TimeRange list
     *                       2 = calendar (date → hours bitmap)
     * @param detail         0 = summary  1 = per-stream detail (type 1 & 2 only)
     * @param include_motion When true, adds "motionPeriods" key to the result.
     * @param cb             Invoked with (SockException, Json::Value result).
     */
    static void findTimePeriod(
        const mediakit::MediaTuple &tuple,
        uint64_t start_time, uint64_t end_time,
        int period_type, int detail, bool include_motion,
        const std::function<void(const toolkit::SockException &, const Json::Value &)> &cb);
};

} // namespace managerkit

#endif // LOCAL_SEARCHENGINE_H
