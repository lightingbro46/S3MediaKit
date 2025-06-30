#include <iomanip>
#include "TimeMaker.h"
#include "Common/config.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

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

bool TimeMaker::inputData(uint64_t &block_time) {
    _last_count++;
    _last_minute = block_time;
    return true;
}

void TimeMaker::reset() {
    _last_count = 0;
}

void TimeMaker::writeIndex(size_t &block_size) {
    BlockListIndexEntry entry;
    entry.count = _last_count;
    entry.offset = _last_offset;
    entry.start_time = _last_minute;

    onWriteIndex(entry);
    reset();

    _last_offset += block_size;
}

bool TimeMaker::findLowerBound(BlockListIndexEntry &entry, int64_t &stamp) {
    if (onSeekIndex(0)) {
        bool eof = false;
        int64_t last_time = -1;
        vector<BlockListIndexEntry> samples;
        while (!eof && last_time < stamp) {
            BlockListIndexEntry sample;
            onReadIndex(sample, eof);
            if (!eof) {
                samples.push_back(sample);
                last_time = sample.start_time;
            }
        } 
        if (samples.size() > 0) {
            entry = samples.size() > 1 ? samples[samples.size() - 2] : samples[0];
            return true;
        }
    }
    return false;
}

uint64_t TimeMaker::findFirstStamp() {
    if (onSeekIndex(0)) {
        bool eof = false;
        BlockListIndexEntry entry;
        onReadIndex(entry, eof);
        if (!eof) {
            return entry.start_time;
        }
    }
    return 0;
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

    _file_path = file;
    _file = std::make_shared<TimeFileDisk>();
    _file->openFile(_file_path.data(), mode.data());

    if (mode == "ab+") {
        _writer = _file->createWriter();
    } else if (mode == "rb") {
        _reader = _file->createReader();
    } else {
        throw std::runtime_error("File mode \"" + mode + "\" do not support in reading or writing index");
    }

    if (_reader) {
        auto first_stamp = findFirstStamp();
        setFirstStamp(first_stamp);
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
        return _reader->seek(offset) == static_cast<int>(offset);
    }
    return false;
}

} // namespace mediakit