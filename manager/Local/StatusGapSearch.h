#ifndef LOCAL_STATUSGAPSEARCH_H
#define LOCAL_STATUSGAPSEARCH_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>
#include "Common/MediaSource.h"

namespace managerkit {

struct StatusGap {
    std::string stream_id;
    uint64_t startTime = 0;
    uint32_t duration = 0;
    int status = 1;
    std::string reason;
};

class StatusGapSearch {
public:
    using Ptr = std::shared_ptr<StatusGapSearch>;
    explicit StatusGapSearch(const mediakit::MediaTuple &tuple) : _tuple(tuple) {}

    void getRecordedStatusGaps(uint64_t start_time, uint64_t end_time, 
        const std::function<void(std::vector<StatusGap> &)> &cb);

    void getRecordedStatusGaps(uint64_t start_time, uint64_t end_time, 
        const std::function<void(std::unordered_map<std::string/*stream_id*/, std::vector<StatusGap>> &)> &cb);

    void getRecordedStatusGaps(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::unordered_map<std::string/*stream_id*/, std::unordered_map<std::string /*date*/, std::unordered_map<int /*hour*/, std::vector<StatusGap>>>> &)> &cb);

private:
    static void mergeInto(std::vector<StatusGap> &list, const std::string &stream_id,
                          uint64_t start_time, uint64_t end_time,
                          int status, const std::string &reason);

    mediakit::MediaTuple _tuple;
};

} // namespace managerkit

#endif // LOCAL_STATUSGAPSEARCH_H
