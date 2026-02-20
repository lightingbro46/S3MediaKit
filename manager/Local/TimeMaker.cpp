#include <iomanip>
#include "TimeMaker.h"
#include "Common/config.h"
#include "Util/util.h"
#include "Common/StrUtil.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

////////////////////// TimeMaker ////////////////////////

bool TimeMaker::inputData(uint64_t &block_time, size_t &block_size) {
    auto last_minute = StampUtils::getStartOfMinute(block_time);
    if (last_minute > getLastStamp()) {
        BlockListIndexEntry entry;
        entry.start_time = last_minute;
        entry.offset = _last_offset;
        
        inputEntry(entry);
        setLastStamp(last_minute);
    }
    _last_offset += block_size;
    return true;
}

uint64_t TimeMaker::getStampOfEntry(BlockListIndexEntry &entry) {
    return entry.start_time;
}

////////////////////// TimeMakerImp /////////////////////////

TimeMakerImp::TimeMakerImp() : TimeMaker() {}

TimeMakerImp::~TimeMakerImp() {
    closeFile();
}

string TimeMakerImp::indexFile(const string &db_path) {
    string file_path(db_path);
    replace(file_path, ".s3db", ".idx");
    return file_path;
}

void TimeMakerImp::openFile(const std::string &file, string mode) {
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

void TimeMakerImp::closeFile() {
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

void TimeMakerImp::onWriteEntry(BlockListIndexEntry &entry) {
    if(_writer) {
        _writer->write(reinterpret_cast<const char *>(&entry), sizeof(entry));
        _writer->flush();
    }
}

void TimeMakerImp::onReadEntry(BlockListIndexEntry &entry, bool &eof) {
    eof = false;
    if (_reader && _reader->read(reinterpret_cast<char*>(&entry), sizeof(entry)) == 0) {
        return;
    }
    eof = true;
}

bool TimeMakerImp::onSeekEntry(uint32_t offset) {
    if(_reader) {
        return _reader->seek(offset) == 0;
    }
    return false;
}

} // namespace managerkit