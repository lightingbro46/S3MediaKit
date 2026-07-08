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
};

class TimeQuery final {
public:
    using Ptr = std::shared_ptr<TimeQuery>;
    using TimeBlockListPtr = std::shared_ptr<TimeQuery>;
    using TimeBlockImp = std::function<void(const TimeBlock &block)>;

    TimeQuery(const mediakit::MediaTuple &tuple, const std::string &file_path = "", bool use_statistic = true);

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
    bool _use_statistic = false;
    std::recursive_mutex _mtx;
    MultiTimeDemuxer::Ptr _demuxer;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMEQUERY_H_
