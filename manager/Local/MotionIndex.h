#ifndef LOCAL_MOTIONINDEX_H_
#define LOCAL_MOTIONINDEX_H_

#include "FileMaker.h"
#include "TimeFile.h"
#include "Poller/EventPoller.h"

namespace managerkit {

struct MotionIndexEntry {
    uint64_t motion_start;
    uint64_t motion_end;
    int motion_level;
    int motion_area;
} __attribute__((packed)); 

class MotionMaker : public FileMaker<MotionIndexEntry> {
public:
    bool inputData(uint64_t &motion_start, uint64_t &motion_end, int &motion_level, int &motion_area);

private:
    uint64_t getStampOfEntry(MotionIndexEntry &entry) override;
};

class MotionMakerImp : public MotionMaker {
public:
    using Ptr = std::shared_ptr<MotionMakerImp>;

    MotionMakerImp();
    ~MotionMakerImp() override;

    void openFile(const std::string &file, std::string mode);
    void closeFile();

    TimeWriter::Ptr createWriter();
    TimeReader::Ptr createReader();

private:
    void onWriteEntry(MotionIndexEntry &entry) override;

    void onReadEntry(MotionIndexEntry &entry, bool &eof) override;

    bool onSeekEntry(uint32_t offset) override;

private:
    std::string _file_path;
    TimeFileDisk::Ptr _file;
    TimeWriter::Ptr _writer;
    TimeReader::Ptr _reader;
};

} // namespace managerkit

#endif // LOCAL_MOTIONINDEX_H_