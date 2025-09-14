#include "TimeDemuxer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {
/////////////////////////TimerDemuxerInterface////////////////////////

int64_t TimerDemuxerInterface::seekTo(uint64_t stamp_sec) {
    uint64_t pos_time = 0;
    {
        // find without index file, by scanning db file 
        _reader->seek(0);
        auto last_offset = 0;
        bool eof = false;
        while (!eof && pos_time < stamp_sec) {
            last_offset = _reader->tell();
            TimeBlock block;
            readBlock(block, eof);
            if (eof) {
                break;
            }
            pos_time = block.start_time();
        }
        _reader->seek(last_offset);
    }
    
    return pos_time;
}

void TimerDemuxerInterface::readBlock(TimeBlock &block, bool &eof) {
    eof = false;
    uint32_t size;
    auto ret = _reader->read(reinterpret_cast<char*>(&size), sizeof(uint32_t));
    if (ret < 0) {
        eof = true;
        return;
    }
    string buffer(size, '\0');
    ret = _reader->read(reinterpret_cast<char*>(&buffer[0]), size);
    if (ret < 0) {
        eof = true;
        return;
    }
    if (!block.ParseFromString(buffer)) {
        throw std::runtime_error("Parse from buffer failed");
    }
}

uint64_t TimerDemuxerInterface::findFirstStamp() {
    auto first_stamp = 0;
    auto eof = false;
    _reader->seek(0);
    TimeBlock block;
    readBlock(block, eof);
    if (!eof) {
        first_stamp = block.start_time();
    }
    _reader->seek(0);
    return first_stamp;
}

/////////////////////////TimeDemuxer////////////////////////
TimeDemuxer::~TimeDemuxer() {
    closeFile();
}

void TimeDemuxer::openFile(const string &file) {
    closeFile();
    
    _file_name = file;
    _file = std::make_shared<TimeFileDisk>();
    _file->openFile(_file_name.data(), "rb");
    _reader = _file->createReader();

    auto index_path = TimeMakerImp::indexFile(_file_name);
    if (File::fileExist(index_path)) {
        _maker = std::make_shared<TimeMakerImp>();
        _maker->openFile(index_path, "rb");
    }

    _first_stamp = findFirstStamp();
}

void TimeDemuxer::closeFile() {
    _maker.reset();
    _reader.reset();
    _file.reset();
}

int64_t TimeDemuxer::seekTo(uint64_t stamp_sec) {
    if (_maker) {
        // find with index file
        BlockListIndexEntry entry;
        auto block_minute = getStartOfMinute(stamp_sec);
        if (!_maker->findLowerBound(entry, block_minute)) {
            return 0;
        }
        if (_reader->seek(entry.offset) < 0) {
            return -1;
        }
        return entry.start_time;
    }

    return TimerDemuxerInterface::seekTo(stamp_sec);
}

uint64_t TimeDemuxer::findFirstStamp() {
    if (_maker) {
        // find with index file
        return _maker->getFirstStamp();
    }

    // find without index file, scan db
    return TimerDemuxerInterface::findFirstStamp();
}

////////////////////////////////////MultiTimeDemuxer/////////////////////////////////////////////

void MultiTimeDemuxer::openFile(const string &files_string) {
    std::vector<std::string> files;
    if (File::is_dir(files_string)) {
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        File::scanDir(files_string, [&](const string &path, bool is_dir) {
            if (!is_dir && path.find("/" + mediaServerId) != string::npos && end_with(path, ".s3db")) {
                files.emplace_back(path);
            }
            return true;
        });
        std::sort(files.begin(), files.end());
    } else {
        files = split(files_string, ";");
    }

    for (auto &file : files) {
        auto demuxer = std::make_shared<TimeDemuxer>();
        demuxer->openFile(file);
        auto start_stamp = demuxer->getFirstStamp();
        _demuxers.emplace(start_stamp, demuxer);
    }
    CHECK(!_demuxers.empty());
    _it = _demuxers.begin();
}

void MultiTimeDemuxer::closeFile() {
    _demuxers.clear();
    _it = _demuxers.end();
}

int64_t MultiTimeDemuxer::seekTo(uint64_t stamp_sec) {
    auto it = _demuxers.upper_bound(stamp_sec);
    // find last element less than or equal to stamp_sec, or return the first element
    _it = it == _demuxers.begin() ? it : std::prev(it);
    return _it->second->seekTo(stamp_sec);
}

void MultiTimeDemuxer::readBlock(TimeBlock &block, bool &eof) {
    for (;;) {
        _it->second->readBlock(block, eof);
        if (eof && _it != _demuxers.end()) {
            // Switch to the next file
            if (++_it == _demuxers.end()) {
                // It's the last file
                return;
            }
            // The next file starts from scratch
            _it->second->seekTo(0);
            continue;
        }
        return;
    }
}

////////////////////////////TimeMemoryDemuxer//////////////////////////////////////

TimeMemoryDemuxer::TimeMemoryDemuxer(const string& buf) {
    _file = std::make_shared<TimeFileMemory>(buf);
    _reader = _file->createReader();
    _first_stamp = findFirstStamp();
}

} // namespace mediakit