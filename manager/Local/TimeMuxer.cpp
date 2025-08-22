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
        _maker->openFile(index_path, "ab+");
    }
}

TimeFileIO::Writer TimeMuxer::createWriter() {
    return _file->createWriter();
}

void TimeMuxer::closeFile() {
    TimeMuxerInterface::flush();
    _file = nullptr;
    _maker = nullptr;
}

void TimeMuxer::onInput(uint64_t block_time) {
    if (_maker) {
        _maker->inputData(block_time); 
    }
}

void TimeMuxer::onFlush(size_t bytes) {
    if (_maker) {
        _maker->writeIndex(bytes);
    }
}

bool TimeMuxer::inputBlock(const TimeBlock &block) {
    TimeMuxerInterface::inputBlock(block);
    onInput(_pending_list.created_at());
    return true;
}

size_t TimeMuxer::save() {
    size_t writen_size = TimeMuxerInterface::save();
    onFlush(writen_size);
    return writen_size;
}

/////////////////////////////////////////// TimeMuxerInterface /////////////////////////////////////////////

size_t TimeMuxerInterface::save() {
    string data = _pending_list.SerializeAsString();
    uint32_t size = data.size();
    if (!_writer) {
        _writer = createWriter();
    }
    _writer->write(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
    _writer->write(data.c_str(), size);
    _writer->flush();

    return sizeof(uint32_t) + size;
}

void TimeMuxerInterface::reset() {
    _pending_list.Clear();
}

void TimeMuxerInterface::flush() {
    if (_pending_list.blocks_size() == 0) {
        return;
    }
    save();
    reset();
}

bool TimeMuxerInterface::inputBlock(const TimeBlock &block) {
    auto block_minute = getStartOfMinute(block.start_time());
    if (block_minute != _pending_list.created_at()) {
        flush();
        _pending_list.set_created_at(block_minute);
    }

    *_pending_list.add_blocks() = block;
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