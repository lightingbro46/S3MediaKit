#ifndef LOCAL_TIMEMUXER_H_
#define LOCAL_TIMEMUXER_H_

#include "TimeMaker.h"
#include "BaseProtoInterface.h"
#include "proto/timeblock.pb.h"

namespace managerkit {

class TimeMuxer : public BaseProtoMuxerInterface<TimeBlock> {
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
    BaseFileIO::Writer createWriter() override;

    size_t save(const TimeBlock &block) override; 

    void onInput(uint64_t block_time, size_t bytes);

private:
    std::string _file_name;
    FileDisk::Ptr _file;
    bool _use_maker;
    TimeMaker::Ptr _maker;
};

class TimeMuxerMemory : public BaseProtoMuxerInterface<TimeBlock> {
public:
    using Ptr = std::shared_ptr<TimeMuxerMemory>;
    TimeMuxerMemory();

    std::string getMemoryBlock();

protected:
    BaseFileIO::Writer createWriter() override;

private:
    FileMemory::Ptr _memory_file;
};

} // namespace managerkit 

#endif // LOCAL_TIMEMUXER_H_
