#ifndef LOCAL_TIMEDEMUXER_H
#define LOCAL_TIMEDEMUXER_H

#include "TimeFile.h"
#include "TimeMaker.h"

namespace managerkit {

class TimerDemuxerInterface {
public:
    using Ptr = std::shared_ptr<TimerDemuxerInterface>;

    virtual ~TimerDemuxerInterface() = default;

    /**
     * Move timeline to a specific location
     */
    virtual int64_t seekTo(uint64_t stamp_ms);

    /**
     * Read a block
     * @param eof Whether the file has been read completely
     * @return Blocklist data, may be empty
     */
    void readBlock(TimeBlock &block, bool &eof);

    /**
     * Get timestamp of the first block in file
     */
    uint64_t getFirstStamp() { return _first_stamp; }

protected:
    virtual uint64_t findFirstStamp();

protected:
    uint64_t _first_stamp;
    TimeFileIO::Reader _reader;
};

class TimeDemuxer final : public TimerDemuxerInterface {
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

    int64_t seekTo(uint64_t stamp_ms) override;

private:
    uint64_t findFirstStamp() override;

private:
    std::string _file_name;
    TimeFileDisk::Ptr _file;
    TimeMakerImp::Ptr _maker;
};

class MultiTimeDemuxer final {
public:
    using Ptr = std::shared_ptr<MultiTimeDemuxer>;

    ~MultiTimeDemuxer() = default;

    void openFile(const std::string &file);

    void closeFile();

    int64_t seekTo(uint64_t stamp_ms);

    void readBlock(TimeBlock &block, bool &eof);

    uint64_t getFirstStamp() { return _demuxers.begin()->first; }

private:
    std::map<uint64_t, TimeDemuxer::Ptr>::iterator _it;
    std::map<uint64_t, TimeDemuxer::Ptr> _demuxers;
};

class TimeMemoryDemuxer final : public TimerDemuxerInterface {
public:
    using Ptr = std::shared_ptr<TimeMemoryDemuxer>;

    TimeMemoryDemuxer(const std::string &buf);
    ~TimeMemoryDemuxer() = default;

private:
    TimeFileMemory::Ptr _file;
};

} // namespace managerkit 

#endif // LOCAL_TIMEDEMUXER_H