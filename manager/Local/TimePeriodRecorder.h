#ifndef S3MANAGER_TIMEPERIOD_H
#define S3MANAGER_TIMEPERIOD_H

#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "json/json.h"
#include "Network/Socket.h"
#include "proto/timeblock.pb.h"

namespace managerkit {

struct BlockListIndexEntry {
    uint32_t file_index;
    uint64_t start_time;
    uint64_t offset;
    uint32_t count;
} __attribute__((packed));

class TimeBlockWriter : public std::enable_shared_from_this<TimeBlockWriter> {
public:
    using Ptr = std::shared_ptr<TimeBlockWriter>;
    ~TimeBlockWriter();

    static TimeBlockWriter& Instance();

    void addBlock(TimeBlock& block);
    void flush();

private:
    TimeBlockWriter(size_t max_batch = 1000, size_t flush_threshold = 1000000);

    void rolateFile();
    void writeList(const TimeBlockList& list);
    std::string indexToFilename(uint32_t index);
    void getCurrentIndex();

private:
    std::recursive_mutex _mtx_time;
    std::ofstream _data_stream;
    std::ofstream _meta_stream;

    std::string _output_dir;
    uint64_t _current_offset = 0;
    uint32_t _current_file_index = 0;

    size_t _max_batch;
    size_t _flush_threshold;
    TimeBlockList _pending_list;

    int64_t _current_minute = -1;
};

class TimeBlockReader {
public:
    static TimeBlockReader& Instance();

    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time, const std::string &camera_id, int period_type, int detail,
        const std::function<void(const toolkit::SockException &ex, const Json::Value &data)> &cb);
    
public:
    std::vector<TimeBlock> query(uint64_t start_time, uint64_t end_time, const std::string &camera_id);
    void query(uint64_t start_time, uint64_t end_time, const std::string &camera_id, const std::function<void(const TimeBlock &block)> &cb);

private:
    TimeBlockReader();

    void loadIndex();
    void reloadIndex();
    std::string indexToFilename(uint32_t index);
    TimeBlockList readList(uint32_t file_index, uint64_t offset);

private:
    std::string _output_dir;
    std::recursive_mutex _mtx_time;
    std::vector<BlockListIndexEntry> _index;
    int64_t _last_block_minute = -1;
};

} // namespace managerkit

#endif // S3MANAGER_TIMEPERIOD_H