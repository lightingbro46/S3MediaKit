#ifndef LOCAL_TIMEDEMUXER_H
#define LOCAL_TIMEDEMUXER_H

#include "TimeFile.h"
#include "TimeMaker.h"

namespace mediakit {

class TimeDemuxer final {
public:
    using Ptr = std::shared_ptr<TimeDemuxer>;

    ~TimeDemuxer();

    /**
     * Open db file
     * @param file Full file path
     */
    void openFile(const std::string &file);

    /**
     * Manually close the file (it will be closed automatically when the object is destructed)
     */
    void closeFile();

    /**
     * Move timeline to a specific location
     */
    int64_t seekTo(int64_t stamp_ms);

    /**
     * Read a blocklist
     * @param eof Whether the file has been read completely
     * @return Blocklist data, may be empty
     */
    void readBlockList(TimeBlockList &list, bool &eof);

    uint64_t getFirstStamp() { return _first_stamp; }

private:
    uint64_t findFirstStamp();

private:
    std::string _file_name;
    uint64_t _first_stamp;
    TimeFileDisk::Ptr _file;
    TimeFileDisk::Reader _reader;
    TimeMakerImp::Ptr _maker;
};

class MultiTimeDemuxer final {
public:
    using Ptr = std::shared_ptr<MultiTimeDemuxer>;

    ~MultiTimeDemuxer() = default;

    void openFile(const std::string &file);

    void closeFile();

    int64_t seekTo(int64_t stamp_ms);

    void readBlockList(TimeBlockList &list, bool &eof);

private:
    std::map<uint64_t, TimeDemuxer::Ptr>::iterator _it;
    std::map<uint64_t, TimeDemuxer::Ptr> _demuxers;
};

} // namespace mediakit 

#endif // LOCAL_TIMEDEMUXER_H