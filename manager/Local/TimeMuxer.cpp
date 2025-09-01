#include "TimeMuxer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

//////////////////////////// TimeMuxer //////////////////////////

TimeMuxer::~TimeMuxer() {
    closeFile();
}

void TimeMuxer::openFile(const string &file) {
    closeFile();
    _file_name = file;
    _file = std::make_shared<TimeFileDisk>();
    _file->openFile(_file_name.data(), "ab+");

    if (_use_maker) {
        auto index_path = TimeMakerImp::indexFile(_file_name);
        _maker = std::make_shared<TimeMakerImp>();
        // open file to write time index in append mode
        _maker->openFile(index_path, "ab+");
        // set current offset to write correct time index after restart server
        _maker->setLastOffset(File::fileSize(_file_name));
    }
}

TimeFileIO::Writer TimeMuxer::createWriter() {
    return _file->createWriter();
}

void TimeMuxer::closeFile() {
    _file = nullptr;
    _maker = nullptr;
}

void TimeMuxer::onInput(uint64_t block_time, size_t bytes) {
    if (_maker) {
        _maker->inputData(block_time, bytes); 
    }
}

size_t TimeMuxer::save(const TimeBlock &block) {
    size_t writen_size = TimeMuxerInterface::save(block);
    onInput(block.start_time(), writen_size);
    return writen_size;
}

/////////////////////////////////////////// TimeMuxerInterface /////////////////////////////////////////////

size_t TimeMuxerInterface::save(const TimeBlock &block) {
    string data = block.SerializeAsString();
    uint32_t size = data.size();
    if (!_writer) {
        _writer = createWriter();
    }
    _writer->write(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
    _writer->write(data.c_str(), size);
    _writer->flush();

    return sizeof(uint32_t) + size;
}

bool TimeMuxerInterface::inputBlock(const TimeBlock &block) {
    save(block);
    return true;
}

/////////////////////////////////////////// TimeMuxerMemory /////////////////////////////////////////////

TimeMuxerMemory::TimeMuxerMemory() {
    _memory_file = std::make_shared<TimeFileMemory>();
}

TimeFileIO::Writer TimeMuxerMemory::createWriter() {
    return _memory_file->createWriter();
}

string TimeMuxerMemory::getMemoryBlock() {
    return _memory_file->getAndClearMemory();
}

} // namespace mediakit