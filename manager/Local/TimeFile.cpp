#include "TimeFile.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

//////////////////////////TimeWriter////////////////////////////////

int TimeWriter::write(const void *data, size_t bytes) { 
    return _io->onWrite(data, bytes); 
}

int TimeWriter::seek(int64_t offset) { 
    return _io->onSeek(offset); 
}

int64_t TimeWriter::tell() { 
    return _io->onTell(); 
}

int TimeWriter::flush() { 
    return _io->onFlush(); 
}

//////////////////////////TimeWriter////////////////////////////////

int TimeReader::read(void *data, size_t bytes)  { 
    return _io->onRead(data, bytes); 
}

int TimeReader::seek(int64_t offset) { 
    return _io->onSeek(offset); 
}

int64_t TimeReader::tell() { 
    return _io->onTell(); 
}

//////////////////////////TimeFileIO////////////////////////////////

TimeFileIO::Writer TimeFileIO::createWriter() {
    Ptr self = shared_from_this();
    // Save a strong reference to itself to prevent premature release
    Writer writer = std::make_shared<TimeWriter>(self);
    if (!writer) {
        throw std::runtime_error("Failed to write to time file!");
    }
    return writer;
}

TimeFileIO::Reader TimeFileIO::createReader() {
    Ptr self = shared_from_this();
    // Save a strong reference to itself to prevent premature release
    Reader reader = std::make_shared<TimeReader>(self);
    if (!reader) {
        throw std::runtime_error("Failed to read to time file!");
    }
    return reader;
}

/////////////////////////////////////////////////////TimeFileDisk/////////////////////////////////////////////////////////

#if defined(_WIN32) || defined(_WIN64)
    #define fseek64 _fseeki64
    #define ftell64 _ftelli64
#else
    #define fseek64 fseek
    #define ftell64 ftell
#endif

void TimeFileDisk::openFile(const char *file, const char* mode) {
    // Create a file
    auto fp = File::create_file(file, mode);
    if(!fp){
        throw std::runtime_error(string("Failed to open the file:") + file);
    }

    GET_CONFIG(uint32_t, timeBufSize, Record::kFileBufSize);
    // Create a new file io cache
    std::shared_ptr<char> file_buf(new char[timeBufSize],[](char *ptr){
        if(ptr){
            delete [] ptr;
        }
    });

    if (file_buf) {
        // Set the file io cache
        setvbuf(fp, file_buf.get(), _IOFBF, timeBufSize);
    }
    // Create a smart pointer
    _file.reset(fp,[file_buf](FILE *fp) {
        fflush(fp);
        fclose(fp);
    });
}

void TimeFileDisk::closeFile() {
    _file = nullptr;
}

int TimeFileDisk::onRead(void *data, size_t bytes) {
    if (bytes == fread(data, 1, bytes, _file.get())) {
        return 0;
    }
    return 0 != ferror(_file.get()) ? ferror(_file.get()) : -1 /*EOF*/;
}

int TimeFileDisk::onWrite(const void *data, size_t bytes) {
    return bytes == fwrite(data, 1, bytes, _file.get()) ? 0 : ferror(_file.get());
}

int TimeFileDisk::onSeek(uint64_t offset) {
    return fseek64(_file.get(), offset, SEEK_SET);
}

uint64_t TimeFileDisk::onTell() {
    return ftell64(_file.get());
}

int TimeFileDisk::onFlush() {
    return fflush(_file.get());
}

/////////////////////////////////////////////////////TimeFileMemory/////////////////////////////////////////////////////////

TimeFileMemory::TimeFileMemory(const string &buf) : _memory(buf) {
    _offset = _memory.size();
}

string TimeFileMemory::getAndClearMemory() {
    string ret;
    ret.swap(_memory);
    _offset = 0;
    return ret;
}

size_t TimeFileMemory::fileSize() const {
    return _memory.size();
}

uint64_t TimeFileMemory::onTell() {
    return _offset;
}

int TimeFileMemory::onSeek(uint64_t offset) {
    if (offset > _memory.size()) {
        return -1;
    }
    _offset = offset;
    return 0;
}

int TimeFileMemory::onRead(void *data, size_t bytes){
    if (_offset >= _memory.size()) {
        //EOF
        return -1;
    }
    bytes = MIN(bytes, _memory.size() - _offset);
    memcpy(data, _memory.data(), bytes);
    _offset += bytes;
    return 0;
}

int TimeFileMemory::onWrite(const void *data, size_t bytes){
    if (_offset + bytes > _memory.size()) {
        // Need to expand
        _memory.resize(_offset + bytes);
    }
    memcpy((uint8_t *) _memory.data() + _offset, data, bytes);
    _offset += bytes;
    return 0;
}

} // namespace managerkit