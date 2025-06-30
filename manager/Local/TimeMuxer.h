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
    void save();

    /**
     * input index if use maker
     */
    virtual void onInput(uint64_t block_time) = 0;

    /**
     * write index if use maker
     */
    virtual void onFlush(size_t bytes) = 0;

protected:
    virtual TimeFileIO::Writer createWriter() = 0;

private:
    uint64_t _last_minute = 0;
    TimeFileIO::Writer _writer;
    TimeBlockList _pending_list;
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
    
private:
    TimeFileIO::Writer createWriter() override;

    void onInput(uint64_t block_time) override;

    void onFlush(size_t bytes) override;

private:
    std::string _file_name;
    TimeFileDisk::Ptr _file;
    bool _use_maker;
    TimeMakerImp::Ptr _maker;
};

} // namespace mediakit 

#endif // LOCAL_TIMEMUXER_H_
