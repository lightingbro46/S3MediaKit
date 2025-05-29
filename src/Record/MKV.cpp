#if defined(ENABLE_MKV)

#include "MKV.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static struct mkv_buffer_t s_io = { 
    [](void *ctx, void *data, uint64_t bytes) {
        MKVFileIO *thiz = (MKVFileIO *)ctx;
        return thiz->onRead(data, bytes);
    },
    [](void *ctx, const void *data, uint64_t bytes) {
        MKVFileIO *thiz = (MKVFileIO *)ctx;
        return thiz->onWrite(data, bytes);
    },
    [](void *ctx, int64_t offset) {
        MKVFileIO *thiz = (MKVFileIO *)ctx;
        return thiz->onSeek(offset);
    },
    [](void *ctx) {
        MKVFileIO *thiz = (MKVFileIO *)ctx;
        return (int64_t)thiz->onTell();
    } 
};

MKVFileIO::Writer MKVFileIO::createWriter(int options) {
    Writer writer;
    Ptr self = shared_from_this();
    // Save a strong reference to itself to prevent premature release
    writer.reset(mkv_writer_create(&s_io,this, options),[self](mkv_writer_t *ptr){
        if(ptr){
            mkv_writer_destroy(ptr);
        }
    });
    if(!writer){
        throw std::runtime_error("Failed to write to mkv file!");
    }
    return writer;
}

MKVFileIO::Reader MKVFileIO::createReader() {
    Reader reader;
    Ptr self = shared_from_this();
    // Save a strong reference to itself to prevent premature release
    reader.reset(mkv_reader_create(&s_io,this),[self](mkv_reader_t *ptr){
        if(ptr){
            mkv_reader_destroy(ptr);
        }
    });
    if(!reader){
        throw std::runtime_error("Failed to read mkv file!");
    }
    return reader;
}

/////////////////////////////////////////////////////MKVFileDisk/////////////////////////////////////////////////////////

#if defined(_WIN32) || defined(_WIN64)
    #define fseek64 _fseeki64
    #define ftell64 _ftelli64
#else
    #define fseek64 fseek
    #define ftell64 ftell
#endif

void MKVFileDisk::openFile(const char *file, const char *mode) {
    // Create a file
    auto fp = File::create_file(file, mode);
    if(!fp){
        throw std::runtime_error(string("Failed to open the file:") + file);
    }

    GET_CONFIG(uint32_t,mkvBufSize,Record::kFileBufSize);

    // Create a new file io cache
    std::shared_ptr<char> file_buf(new char[mkvBufSize],[](char *ptr){
        if(ptr){
            delete [] ptr;
        }
    });

    if(file_buf){
        // Set the file io cache
        setvbuf(fp, file_buf.get(), _IOFBF, mkvBufSize);
    }

    // Create a smart pointer
    _file.reset(fp,[file_buf](FILE *fp) {
        fflush(fp);
        fclose(fp);
    });
}

void MKVFileDisk::closeFile() {
    _file = nullptr;
}

int MKVFileDisk::onRead(void *data, size_t bytes) {
    if (bytes == fread(data, 1, bytes, _file.get())){
        return 0;
    }
    return 0 != ferror(_file.get()) ? ferror(_file.get()) : -1 /*EOF*/;
}

int MKVFileDisk::onWrite(const void *data, size_t bytes) {
    return bytes == fwrite(data, 1, bytes, _file.get()) ? 0 : ferror(_file.get());
}

int MKVFileDisk::onSeek(uint64_t offset) {
    return fseek64(_file.get(), offset, SEEK_SET);
}

uint64_t MKVFileDisk::onTell() {
    return ftell64(_file.get());
}

/////////////////////////////////////////////////////MKVFileMemory/////////////////////////////////////////////////////////

string MKVFileMemory::getAndClearMemory(){
    string ret;
    ret.swap(_memory);
    _offset = 0;
    return ret;
}

size_t MKVFileMemory::fileSize() const{
    return _memory.size();
}

uint64_t MKVFileMemory::onTell(){
    return _offset;
}

int MKVFileMemory::onSeek(uint64_t offset){
    if (offset > _memory.size()) {
        return -1;
    }
    _offset = offset;
    return 0;
}

int MKVFileMemory::onRead(void *data, size_t bytes){
    if (_offset >= _memory.size()) {
        //EOF
        return -1;
    }
    bytes = MIN(bytes, _memory.size() - _offset);
    memcpy(data, _memory.data(), bytes);
    _offset += bytes;
    return 0;
}

int MKVFileMemory::onWrite(const void *data, size_t bytes){
    if (_offset + bytes > _memory.size()) {
        // Need to expand
        _memory.resize(_offset + bytes);
    }
    memcpy((uint8_t *) _memory.data() + _offset, data, bytes);
    _offset += bytes;
    return 0;
}

} // namespace mediakit
#endif // defined(ENABLE_MKV)