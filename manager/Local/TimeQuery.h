#ifndef S3MEDIAKIT_TIMEQUERY_H_
#define S3MEDIAKIT_TIMEQUERY_H_

#include <vector>
#include <unordered_map>
#include <set>
#include <string>
#include "TimeDemuxer.h"
#include "Record/Recorder.h"

namespace managerkit {

struct TimeRange {
    TimeRange() = default;
    TimeRange(uint64_t start, uint32_t dur,
              const std::string &storage_tier = "HOT",
              bool restore_required = false)
        : startTime(start)
        , duration(dur)
        , tier(storage_tier.empty() ? "HOT" : storage_tier)
        , restoreRequired(restore_required) {}

    uint64_t startTime = 0;
    uint32_t duration = 0;
    std::string tier = "HOT";
    bool restoreRequired = false;
    bool isReplay = false;
};

class TimeQuery final {
public:
    using Ptr = std::shared_ptr<TimeQuery>;
    using TimeBlockListPtr = std::shared_ptr<TimeQuery>;
    using TimeBlockImp = std::function<void(const TimeBlock &block)>;

    /**
     * @param start_time,end_time When end_time > 0, only the day files that can hold blocks of
     *        this window are opened; queries outside it then miss data.
     */
    TimeQuery(const mediakit::MediaTuple &tuple, const std::string &file_path = "",
              uint64_t start_time = 0, uint64_t end_time = 0);

    ~TimeQuery();

    mediakit::MediaTuple& getMediaTuple() { return _tuple; }

    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::vector<TimeRange> &data)> &cb);
    
    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time, 
        const std::function<void(std::unordered_map<std::string /*stream*/, std::vector<TimeRange>> &data)> &cb);
    
    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::unordered_map<std::string /*date*/, std::set<int/*hour*/>> &data)> &cb);
    
    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time, 
        const std::function<void(std::unordered_map<std::string /*stream_id*/, std::unordered_map<std::string /*date*/, std::unordered_map<int, std::vector<TimeRange>>>> &data)> &cb);
    
    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time,
        const std::function<void(std::vector<TimeBlock> &data)> &cb);

    // Unlike getRecordedTimePeriod(), reports timefile parse/read exceptions
    // instead of returning a silently truncated block list. `out` is empty on failure.
    bool queryBlocks(uint64_t start_time, uint64_t end_time,
                     std::vector<TimeBlock> &out, std::string &error);

    int64_t getOffsetOfDate(uint64_t pos_time);

    std::shared_ptr<TimeBlock> getLastBlock(uint64_t last_archived_time = 0, uint32_t interval_sec = 600);

    std::shared_ptr<TimeBlock> getFirstBlock(uint64_t first_archived_time = 0, uint32_t interval_sec = 600);

private:
    /**
     * Seek to centain timestamp, return the nearest lower value
     */
    bool seekTo(uint64_t stamp);

    /**
     * Read block list in range time
     */
    bool readBlockList(uint64_t &start_stamp, uint64_t &end_stamp, const TimeBlockImp &cb);
    
    /**
     * Find blocks with start_time and end_time
     */
    void query(uint64_t &start_time, uint64_t &end_time, const TimeBlockImp &cb);

    // The TimeBlock fields the timeline overloads use.
    struct RangeBlock {
        uint64_t start_time;
        uint32_t time_len;
        uint32_t stream; // index into _streams
        bool is_replay;
    };

    /**
     * Blocks of [start_time, end_time] sorted by start time. The last window is kept, so the
     * motion filter and the timeline of one request read the timefiles once; a TimeQuery is
     * short-lived and does not see blocks written after that read.
     */
    const std::vector<RangeBlock> &queryRanges(uint64_t start_time, uint64_t end_time);

    /**
     * @brief Sets the current timestamp.
     * Updates the internal _last_time variable with the provided timestamp value.
     * @param stamp The new timestamp value to set.
     */
    void setCurrentStamp(uint64_t stamp) { _last_time = stamp; }

    /**
     * @brief Retrieves the most recent timestamp value.
     * @return uint64_t The last recorded time stamp.
     */
    uint64_t getCurrentStamp() { return _last_time; }

private: 
    mediakit::MediaTuple _tuple;
    uint64_t _last_time = 0;
    std::string _file_path;
    std::recursive_mutex _mtx;
    MultiTimeDemuxer::Ptr _demuxer;
    std::vector<std::string> _streams;
    std::vector<RangeBlock> _ranges;
    uint64_t _ranges_start = 0;
    uint64_t _ranges_end = 0;
    bool _ranges_valid = false;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMEQUERY_H_
