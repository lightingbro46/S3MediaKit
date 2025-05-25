#ifndef S3MEDIAKIT_TIMEPERIOD_H
#define S3MEDIAKIT_TIMEPERIOD_H

#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "json/json.h"
#include "Network/Socket.h"
#include "proto/timeblock.pb.h"

namespace mediakit {

struct BlockListIndexEntry {
    uint32_t file_index;
    uint64_t start_time;
    uint64_t offset;
    uint32_t count;
} __attribute__((packed));

class TimeRecorder : public std::enable_shared_from_this<TimeRecorder> {
public:
    using Ptr = std::shared_ptr<TimeRecorder>;
    
    static TimeRecorder& Instance();

    ~TimeRecorder();

    void addBlock(const TimeBlock &block);

    void getRecordedTimePeriod(uint64_t start_time, uint64_t end_time, const std::string &camera_id, int period_type, int detail,
        const std::function<void(const toolkit::SockException &ex, const Json::Value &data)> &cb);

    int64_t getOffsetOfDate(uint64_t pos_time, const std::string &camera_id, const std::string &stream_id);


private:
    TimeRecorder(size_t max_batch = 1000, size_t flush_threshold = 1000000);

    std::set<uint32_t> getListDataIndex(const std::string &dir_path);

    uint32_t getBlockListSize(const std::string &file_path);

    std::string indexToDbFilePath(uint32_t index);

    std::string indexToMetaFilePath(uint32_t index);

    void flush(bool open_new_file = true);

    void rolateFile();

    void writeList(const TimeBlockList& list);

    std::vector<BlockListIndexEntry> readMetaList(uint32_t file_index);

    TimeBlockList readBlockList(uint32_t file_index, uint64_t offset);

    void query(uint64_t start_time, uint64_t end_time, const std::string &camera_id, const std::function<void(const TimeBlock &block)> &cb);

private:
    std::recursive_mutex _mutex;
    size_t _max_batch;
    size_t _flush_threshold;
    std::string _output_dir;
    std::ofstream _data_stream;
    std::ofstream _meta_stream;
    std::set<uint32_t> _file_index_map;

    TimeBlockList _pending_list;
    uint32_t _current_file_index = 0;
    uint32_t _current_block_size = 0;
    uint32_t _current_offset = 0;
    int64_t _current_minute = -1;
};
} // namespace mediakit

#endif // S3MEDIAKIT_TIMEPERIOD_H