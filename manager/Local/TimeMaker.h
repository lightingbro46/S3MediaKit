#ifndef LOCAL_TIMEMAKER_H
#define LOCAL_TIMEMAKER_H

#include "FileMaker.h"
#include "TimeFile.h"
#include "Poller/EventPoller.h"

namespace managerkit {

struct BlockListIndexEntry {
    uint64_t start_time;
    uint64_t offset;
} __attribute__((packed));

class TimeMaker : public FileMaker<BlockListIndexEntry> {
public:
    bool inputData(uint64_t &block_time, size_t &block_size);

    void setLastOffset(uint32_t offset) { _last_offset = offset; }

private:
    uint64_t getStampOfEntry(BlockListIndexEntry &entry) override;

private:
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
    void onWriteEntry(BlockListIndexEntry &entry) override;

    void onReadEntry(BlockListIndexEntry &entry, bool &eof) override;

    bool onSeekEntry(uint32_t offset) override;

private:
    std::string _file_path;
    TimeFileDisk::Ptr _file;
    TimeWriter::Ptr _writer;
    TimeReader::Ptr _reader;
};

} // namespace managerkit

#endif // LOCAL_TIMEMAKER_H