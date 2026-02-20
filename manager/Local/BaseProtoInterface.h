#ifndef LOCAL_BASEPROTOINTERFACE_H_
#define LOCAL_BASEPROTOINTERFACE_H_

#include "BaseFileIO.h"

namespace managerkit {

template<typename Entry>
class BaseProtoMuxerInterface {
public:
    virtual ~BaseProtoMuxerInterface() = default;

    /**
     * Input block
     * @param block Block to be written, must be serializable to string
     * @return Whether the block is successfully written
     */
    bool inputBlock(const Entry &block) {
        save(block);
        return true;
    }

protected:
    /**
     * Create a writer to write data to file, which will be called when writing the first block. 
     * The writer will be cached and reused for subsequent blocks. 
     * If you want to create a new writer for each block, you can override the save function and call createWriter in it.
     */
    virtual BaseFileIO::Writer createWriter() = 0;

    /**
     * Save time block list
     * @param block Block to be written, must be serializable to string
     * @return Written data size in bytes
     */
    virtual size_t save(const Entry &block) {
        if (!_writer) {
            _writer = createWriter();
        }
        std::string data = block.SerializeAsString();
        uint32_t size = data.size();
        if (size == 0) {
            throw std::runtime_error("Serialize block to string failed");
        }
        if (!_writer) {
            throw std::runtime_error("Writer is not initialized");
        }
        _writer->write(reinterpret_cast<const char *>(&size), sizeof(uint32_t));
        _writer->write(data.c_str(), size);
        _writer->flush();

        return sizeof(uint32_t) + size;
    }

private:
    uint64_t _last_minute = 0;
    BaseFileIO::Writer _writer;
};

template <typename Entry>
class BaseProtoDemuxerInterface {
public:
    virtual ~BaseProtoDemuxerInterface() = default;

    /**
     * Read a block
     * @param block Block read from file, must be deserializable from string
     * @param eof Whether the file has been read completely
     * @return Blocklist data, may be empty
     */
    virtual void readBlock(Entry &block, bool &eof) {
        if (!_reader) {
            throw std::runtime_error("Reader is not initialized");
        }
        uint32_t size;
        auto ret = _reader->read(reinterpret_cast<char*>(&size), sizeof(uint32_t));
        if (ret < 0) {
            eof = true;
            return;
        }
        std::string buffer(size, '\0');
        ret = _reader->read(reinterpret_cast<char*>(&buffer[0]), size);
        if (ret < 0) {
            eof = true;
            return;
        }
        if (!block.ParseFromString(buffer)) {
            throw std::runtime_error("Parse from buffer failed");
        }
    }

    /**
     * Seek to a certain timestamp in the file, and return the timestamp of the block at that position. The default implementation is to seek to the beginning of the file.
     * @param stamp_sec Timestamp to be seeked, in seconds
     * @return Timestamp of the block at the seeked position, in seconds
     */
    virtual int64_t seekTo(uint64_t stamp_sec) {
        if (!_reader) {
            throw std::runtime_error("Reader is not initialized");
        }
        uint64_t pos_time = 0;
        {
            // find without index file, by scanning db file 
            _reader->seek(0);
            auto last_offset = 0;
            bool eof = false;
            while (!eof && pos_time < stamp_sec) {
                last_offset = _reader->tell();
                Entry block;
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

    /**
     * Get timestamp of the first block in file
     */
    uint64_t getFirstStamp() { return _first_stamp; }

protected:
    virtual uint64_t findFirstStamp() {
        if (!_reader) {
            throw std::runtime_error("Reader is not initialized");
        }
        auto first_stamp = 0;
        auto eof = false;
        _reader->seek(0);
        Entry block;
        readBlock(block, eof);
        if (!eof) {
            first_stamp = block.start_time();
        }
        _reader->seek(0);
        return first_stamp;
    }

private:
    uint64_t _first_stamp = 0;
    BaseFileIO::Reader _reader;
};

} // namespace managerkit

#endif // LOCAL_BASEPROTOINTERFACE_H_