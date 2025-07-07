#include "TimeDemuxer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

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

int64_t TimeDemuxer::seekTo(int64_t stamp_sec) {
    if (_maker) {
        // find with index file
        BlockListIndexEntry entry;
        if (!_maker->findLowerBound(entry, stamp_sec)) {
            return -1;
        }
        if (_reader->seek(entry.offset) < 0) {
            return -1;
        }
        return entry.start_time;
    }

    auto pos_time = 0;
    {
        // find without index file, by scanning db file 
        _reader->seek(0);
        auto last_offset = 0;
        bool eof = false;
        while (!eof && pos_time < stamp_sec) {
            last_offset = _reader->tell();
            uint32_t size;
            auto ret = _reader->read(reinterpret_cast<char *>(&size), sizeof(uint32_t));
            if (ret < 0) {
                eof = true;
                break;
            }
            string buffer(size, '\0');
            ret = _reader->read(&buffer[0], size);
            if (ret < 0) {
                eof = true;
                break;
            }
            TimeBlockList list;
            list.ParseFromString(buffer);
            pos_time = list.created_at();
        }
        _reader->seek(last_offset);
    }
    
    return pos_time;
}

void TimeDemuxer::readBlockList(TimeBlockList &list, bool &eof) {
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
    if (!list.ParseFromString(buffer)) {
        throw std::runtime_error("Parse from buffer failed");
    }
}

uint64_t TimeDemuxer::findFirstStamp() {
    if (_maker) {
        return _maker->getFirstStamp();
    }

    auto first_stamp = 0;
    auto eof = false;
    _reader->seek(0);
    TimeBlockList list;
    readBlockList(list, eof);
    if (!eof) {
        first_stamp = list.created_at();
    }
    _reader->seek(0);
    return first_stamp;
}

/////////////////////////////////////////////////////////////////////////////////

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

int64_t MultiTimeDemuxer::seekTo(int64_t stamp_sec) {
    auto it = _demuxers.upper_bound(stamp_sec);
    // find last element less than or equal to stamp_sec, or return the first element
    _it = it == _demuxers.begin() ? it :  std::prev(it);
    return _it->second->seekTo(stamp_sec);
}

void MultiTimeDemuxer::readBlockList(TimeBlockList &list, bool &eof) {
    for (;;) {
        _it->second->readBlockList(list, eof);
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

} // namespace mediakit