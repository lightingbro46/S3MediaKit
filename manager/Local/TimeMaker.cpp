#include <iomanip>
#include "TimeMaker.h"
#include "Common/config.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

uint64_t getStartOfDay(uint64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&t);

    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;

    return static_cast<uint64_t>(std::mktime(tm));
}

uint64_t getStartOfHour(uint64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&t);

    tm->tm_min = 0;
    tm->tm_sec = 0;

    return static_cast<uint64_t>(std::mktime(tm));
}

uint64_t getStartOfMinute(uint64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&t);

    tm->tm_sec = 0;

    return static_cast<uint64_t>(std::mktime(tm));
}

////////////////////// TimeMaker/////////////////////////

bool TimeMaker::inputData(uint64_t &block_time, size_t &block_size) {
    auto last_minute = getStartOfMinute(block_time);
    if (last_minute > _last_minute) {
        writeIndex(last_minute, _last_offset);
        _last_minute = last_minute;
    }
    _last_offset += block_size;
    return true;
}

void TimeMaker::writeIndex(uint64_t stamp, uint64_t offset) {
    BlockListIndexEntry entry;
    entry.offset = offset;
    entry.start_time = stamp;

    onWriteIndex(entry);
}

bool TimeMaker::findLowerBound(BlockListIndexEntry &entry, uint64_t &stamp) {
    bool found = false;
    if (onSeekIndex(0)) {
        bool eof = false;
        uint64_t last_time = 0;
        while (!eof && last_time < stamp) {
            BlockListIndexEntry sample;
            onReadIndex(sample, eof);
            if (!eof) {
                if (sample.start_time < stamp) {
                    entry = sample;
                    found = true;
                }
                last_time = sample.start_time;
            }
        }
    }
    return found;
}

uint64_t TimeMaker::findFirstStamp() {
    uint64_t first_stamp = 0;
    if (onSeekIndex(0)) {
        bool eof = false;
        BlockListIndexEntry entry;
        onReadIndex(entry, eof);
        if (!eof) {
            first_stamp = entry.start_time;
        }
    }
    return first_stamp;
}

uint64_t TimeMaker::findLastStamp() {
    uint64_t last_stamp = 0;
    if (onSeekIndex(0)) {
        bool eof = false;
        BlockListIndexEntry entry;
        while(!eof) {
            onReadIndex(entry, eof);
            if (!eof) {
                last_stamp = entry.start_time;
            }
        }
    }
    return last_stamp;
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
    _file = nullptr;
}

void TimeMakerImp::onWriteIndex(BlockListIndexEntry &entry) {
    if(_writer) {
        _writer->write(reinterpret_cast<const char *>(&entry), sizeof(entry));
        _writer->flush();
    }
}

void TimeMakerImp::onReadIndex(BlockListIndexEntry &entry, bool &eof) {
    eof = false;
    if (_reader && _reader->read(reinterpret_cast<char*>(&entry), sizeof(entry)) == 0) {
        return;
    }
    eof = true;
}

bool TimeMakerImp::onSeekIndex(uint32_t offset) {
    if(_reader) {
        return _reader->seek(offset) == 0;
    }
    return false;
}

} // namespace managerkit