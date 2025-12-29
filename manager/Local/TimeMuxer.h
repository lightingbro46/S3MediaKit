#ifndef LOCAL_TIMEMUXER_H_
#define LOCAL_TIMEMUXER_H_

#include "TimeFile.h"
#include "TimeMaker.h"

namespace managerkit {

class TimeMuxerInterface {
public:
    using Ptr = std::shared_ptr<TimeMuxerInterface>;

    virtual ~TimeMuxerInterface() = default;

    /**
     * Input block
     */
    bool inputBlock(const TimeBlock &block); 

protected:
    virtual TimeFileIO::Writer createWriter() = 0;

    /**
     * Save time block list
     */
    virtual size_t save(const TimeBlock &block);
   
private:
    uint64_t _last_minute = 0;
    TimeFileIO::Writer _writer;
};

class TimeMuxer : public TimeMuxerInterface {
public:
    using Ptr = std::shared_ptr<TimeMuxer>;

    explicit TimeMuxer(bool use_maker = true) : _use_maker(use_maker) {}
    ~TimeMuxer() override;

    /**
     * Open db file
     * @param file Full file path
     */
    void openFile(const std::string &file);

    /**
     * Manually close the file (it will be closed automatically when the object is destructed)
     */
    void closeFile();

protected:
    TimeFileIO::Writer createWriter() override;

    size_t save(const TimeBlock &block) override; 

    void onInput(uint64_t block_time, size_t bytes);

private:
    std::string _file_name;
    TimeFileDisk::Ptr _file;
    bool _use_maker;
    TimeMakerImp::Ptr _maker;
};

class TimeMuxerMemory : public TimeMuxerInterface {
public:
    TimeMuxerMemory();

    std::string getMemoryBlock();

protected:
    TimeFileIO::Writer createWriter() override;


private:
    TimeFileMemory::Ptr _memory_file;
};

} // namespace managerkit 

#endif // LOCAL_TIMEMUXER_H_
