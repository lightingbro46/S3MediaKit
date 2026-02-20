#ifndef LOCAL_FILEMAKER_H_
#define LOCAL_FILEMAKER_H_

#include <string>
#include <deque>
#include "BaseFileIO.h"

namespace managerkit {

template<typename Entry>
class BaseMakerInterface {
public:
    virtual ~BaseMakerInterface() = default;

    bool findLowerBound(Entry &entry, uint64_t &stamp) {
        bool found = false;
        if (onSeekEntry(0)) {
            bool eof = false;
            uint64_t last_time = 0;
            while (!eof && last_time < stamp) {
                Entry sample;
                onReadEntry(sample, eof);
                if (!eof) {
                    if (getStampOfEntry(sample) < stamp) {
                        entry = sample;
                        found = true;
                    }
                    last_time = getStampOfEntry(sample);
                }
            }
        }
        return found;
    }

    bool inputEntry(Entry &entry) {
        onWriteEntry(entry);
        return true;
    }

    uint64_t findFirstStamp() {
        uint64_t first_stamp = 0;
        if (onSeekEntry(0)) {
            bool eof = false;
            Entry entry;
            onReadEntry(entry, eof);
            if (!eof) {
                first_stamp = getStampOfEntry(entry);
            }
        }
        return first_stamp;
    }

    uint64_t findLastStamp() {
        uint64_t last_stamp = 0;
        if (onSeekEntry(0)) {
            bool eof = false;
            Entry entry;
            while(!eof) {
                onReadEntry(entry, eof);
                if (!eof) {
                    last_stamp = getStampOfEntry(entry);
                }
            }
        }
        return last_stamp;
    }

    uint64_t getFirstStamp() { return _first_stamp; }
    void setFirstStamp(uint64_t stamp) { _first_stamp = stamp; }

    uint64_t getLastStamp() { return _last_stamp; }
    void setLastStamp(uint64_t stamp) { _last_stamp = stamp; }

protected:
    virtual void onWriteEntry(Entry &entry) = 0;

    virtual void onReadEntry(Entry &entry, bool &eof) = 0;

    virtual bool onSeekEntry(uint32_t offset) = 0;

    virtual uint64_t getStampOfEntry(Entry &entry) = 0;

private:
    uint64_t _first_stamp = 0;
    uint64_t _last_stamp = 0;
};

template<typename Entry>
class FileMaker : public BaseMakerInterface<Entry> {
public:
    ~FileMaker() override {
        closeFile();
    };

    static std::string toIndexFilePath(const std::string &db_path) {
        std::string file_path(db_path);
        toolkit::replace(file_path, ".s3db", ".idx");
        return file_path;
    }

    void openFile(const std::string &file, std::string mode) {
        closeFile();

        if (mode != "rb" && mode != "ab+") {
            throw std::runtime_error("File mode \"" + mode + "\" do not support in reading or writing index");
        }

        _file_path = file;
        _file = std::make_shared<FileDisk>();
        _file->openFile(_file_path.data(), mode.data());

        if (mode == "ab+") {
            _writer = _file->createWriter();
        } 
        if (mode == "rb" || mode == "ab+") {
            _reader = _file->createReader();
        }

        if (_reader) {
            auto first_stamp = this->findFirstStamp();
            this->setFirstStamp(first_stamp);
        }

        if (_writer && _reader) {
            auto last_stamp = this->findLastStamp();
            this->setLastStamp(last_stamp);
        }
    }

    void closeFile() {
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

private:
    void onWriteEntry(Entry &entry) override {
        if(_writer) {
            _writer->write(reinterpret_cast<const char *>(&entry), sizeof(entry));
            _writer->flush();
        }
    }

    void onReadEntry(Entry &entry, bool &eof) override {
        eof = false;
        if (_reader && _reader->read(reinterpret_cast<char*>(&entry), sizeof(entry)) == 0) {
            return;
        }
        eof = true;
    }

    bool onSeekEntry(uint32_t offset) override {
        if (_reader) {
            return _reader->seek(offset) == 0;
        }
        return false;
    }

private:
    std::string _file_path;
    FileDisk::Ptr _file;
    FileWriter::Ptr _writer;
    FileReader::Ptr _reader;
};

} // namespace managerkit

#endif // LOCAL_FILEMAKER_H_