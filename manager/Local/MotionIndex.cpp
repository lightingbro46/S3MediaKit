#include "MotionIndex.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

///////////////////////////////MotionMaker////////////////////////////////

bool MotionMaker::inputData(uint64_t &motion_start, uint64_t &motion_end, int &motion_level, int &motion_area) {
    MotionIndexEntry entry;
    entry.motion_start = motion_start;
    entry.motion_end = motion_end;
    entry.motion_level = motion_level;
    entry.motion_area = motion_area;

    return inputEntry(entry);
};

uint64_t MotionMaker::getStampOfEntry(MotionIndexEntry &entry) {
    return entry.motion_start;
}

////////////////////// MotionMakerImp /////////////////////////

MotionMakerImp::MotionMakerImp() : MotionMaker() {}

MotionMakerImp::~MotionMakerImp() {
    closeFile();
}

void MotionMakerImp::openFile(const std::string &file, string mode) {
    closeFile();

    if (mode != "rb" && mode != "ab+") {
        throw std::runtime_error("File mode \"" + mode + "\" do not support in reading or writing index");
    }

    _file_path = file;
    _file = std::make_shared<TimeFileDisk>();
    _file->openFile(_file_path.data(), mode.data());

    if (mode == "ab+") {
        _writer = _file->createWriter();
    } 
    if (mode == "rb" || mode == "ab+") {
        _reader = _file->createReader();
    }

    if (_reader) {
        auto first_stamp = findFirstStamp();
        setFirstStamp(first_stamp);
    }

    if (_writer && _reader) {
        auto last_stamp = findLastStamp();
        setLastStamp(last_stamp);
    }
}

void MotionMakerImp::closeFile() {
    if (_writer) {
        _writer->flush();
        _writer.reset();
    }
    if (_reader) {
        _reader.reset();
    }
    if (_file) {
        _file->closeFile();
        _file.reset();
    }
}

void MotionMakerImp::onWriteEntry(MotionIndexEntry &entry) {
    if (_writer) {
        _writer->write(reinterpret_cast<char*>(&entry), sizeof(entry));
    }
}

void MotionMakerImp::onReadEntry(MotionIndexEntry &entry, bool &eof) {
    eof = false;
    if (_reader && _reader->read(reinterpret_cast<char*>(&entry), sizeof(entry)) == 0) {
        return;
    }
    eof = true;
}

bool MotionMakerImp::onSeekEntry(uint32_t offset) {
    if(_reader) {
        return _reader->seek(offset) == 0;
    }
    return false;
}

} // namespace managerkit

