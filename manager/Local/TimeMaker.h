#ifndef LOCAL_TIMEMAKER_H
#define LOCAL_TIMEMAKER_H

#include <string>
#include <deque>
#include "TimeFile.h"
#include "Poller/EventPoller.h"

namespace mediakit {

uint64_t getStartOfDay(uint64_t seconds);

uint64_t getStartOfHour(uint64_t seconds);

uint64_t getStartOfMinute(uint64_t seconds);

struct BlockListIndexEntry {
    uint64_t start_time;
    uint64_t offset;
} __attribute__((packed));

class TimeMaker {
public:
    virtual ~TimeMaker() = default;

    bool inputData(uint64_t &block_time, size_t &block_size);

    bool findLowerBound(BlockListIndexEntry &entry, uint64_t &stamp);

    uint64_t getFirstStamp() { return _first_minute; }

    void setLastOffset(uint64_t offset) { _last_offset = offset; }

protected:
    /**
     * Return first time block in minute or return 0
     */
    uint64_t findFirstStamp();

    /**
     * Return last time block in minute or return 0
     */
    uint64_t findLastStamp();

    void setFirstStamp(uint64_t stamp) { _first_minute = stamp; }

    void setLastStamp(uint64_t stamp) { _last_minute = stamp; }

private:
    void writeIndex(uint64_t stamp, uint64_t offset);

    virtual void onWriteIndex(BlockListIndexEntry &entry) = 0;

    virtual void onReadIndex(BlockListIndexEntry &entry, bool &eof) = 0;

    virtual bool onSeekIndex(uint32_t offset) = 0;

private:
    uint64_t _first_minute = 0;
    uint64_t _last_minute = 0;
    uint32_t _last_offset = 0;
};

class TimeMakerImp : public TimeMaker {
public:
    using Ptr = std::shared_ptr<TimeMakerImp>;

    static std::string indexFile(const std::string &db_path);

    TimeMakerImp();
    ~TimeMakerImp() override;

    void openFile(const std::string &file, std::string mode);
    void closeFile();

    TimeWriter::Ptr createWriter();
    TimeReader::Ptr createReader();

private:
    void onWriteIndex(BlockListIndexEntry &entry) override;

    void onReadIndex(BlockListIndexEntry &entry, bool &eof) override;

    bool onSeekIndex(uint32_t offset) override;

private:
    std::string _file_path;
    TimeFileDisk::Ptr _file;
    TimeWriter::Ptr _writer;
    TimeReader::Ptr _reader;
};

} // namespace mediakit
#endif // LOCAL_TIMEMAKER_H