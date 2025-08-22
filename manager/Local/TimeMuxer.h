#ifndef LOCAL_TIMEMUXER_H_
#define LOCAL_TIMEMUXER_H_

#include "TimeFile.h"
#include "TimeMaker.h"

namespace mediakit {

class TimeMuxerInterface {
public:
    using Ptr = std::shared_ptr<TimeMuxerInterface>;

    virtual ~TimeMuxerInterface() = default;

    /**
     * Input block
     */
    virtual bool inputBlock(const TimeBlock &block); 

    /**
     * Reset all
     */
    virtual void reset();

    /**
     * Refresh all block cache output
     */
    virtual void flush();

    /**
     * Save time block list
     */
    virtual size_t save();

protected:
    virtual TimeFileIO::Writer createWriter() = 0;
   
protected:
    TimeBlockList _pending_list;

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
    
    bool inputBlock(const TimeBlock &block) override;

    size_t save() override; 

protected:
    TimeFileIO::Writer createWriter() override;

    void onInput(uint64_t block_time);

    void onFlush(size_t bytes);

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

} // namespace mediakit 

#endif // LOCAL_TIMEMUXER_H_
