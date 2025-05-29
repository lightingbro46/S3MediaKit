#ifndef S3MEDIAKIT_MKV_H
#define S3MEDIAKIT_MKV_H

#if defined(ENABLE_MKV)

#include <memory>
#include <string>
#include "mkv-writer.h"
#include "mkv-reader.h"
#include "mpeg4-hevc.h"
#include "mpeg4-avc.h"
#include "mpeg4-aac.h"
#include "mkv-buffer.h"
#include "mkv-format.h"

namespace mediakit {

// Abstract interface class for mkv file IO
class MKVFileIO : public std::enable_shared_from_this<MKVFileIO> {
public:
    using Ptr = std::shared_ptr<MKVFileIO>;
    using Writer = std::shared_ptr<mkv_writer_t>;
    using Reader = std::shared_ptr<mkv_reader_t>;

    virtual ~MKVFileIO() = default;

    /**
     * Create an mkv muxer
     * @param options Supports 0, MKV_OPTION_WEBM, MKV_OPTION_LIVE
     * @return mkv muxer
     */
    virtual Writer createWriter(int options);

    /**
     * Create an mkv demuxer
     * @return mkv demuxer
     */
    virtual Reader createReader();

    /**
     * Get the file read/write position
     */
    virtual uint64_t onTell() = 0;

    /**
     * Seek to a certain location in the file
     * @param offset File offset
     * @return Whether it is successful (0 successful)
     */
    virtual int onSeek(uint64_t offset) = 0;

    /**
     * Read a certain amount of data from the file
     * @param data Data storage pointer
     * @param bytes Pointer length
     * @return Whether it is successful (0 successful)
     */
    virtual int onRead(void *data, size_t bytes) = 0;

    /**
     * Write a certain amount of data to the file
     * @param data Data pointer
     * @param bytes Data length
     * @return Whether it is successful (0 successful)
     */
    virtual int onWrite(const void *data, size_t bytes) = 0;
};

// Disk MKV file class
class MKVFileDisk : public MKVFileIO {
public:
    using Ptr = std::shared_ptr<MKVFileDisk>;

    /**
     * Open the disk file
     * @param file File path
     * @param mode fopen mode
     */
    void openFile(const char *file, const char *mode);

    /**
     * Close the disk file
     */
    void closeFile();

protected:
    uint64_t onTell() override;
    int onSeek(uint64_t offset) override;
    int onRead(void *data, size_t bytes) override;
    int onWrite(const void *data, size_t bytes) override;

private:
    std::shared_ptr<FILE> _file;
};

class MKVFileMemory : public MKVFileIO {
public:
    using Ptr = std::shared_ptr<MKVFileMemory>;   
    
    /**
     * Get the file size
     */
    size_t fileSize() const;

    /**
     * Get and clear the file cache
     */
    std::string getAndClearMemory();

protected:
    uint64_t onTell() override;
    int onSeek(uint64_t offset) override;
    int onRead(void *data, size_t bytes) override;
    int onWrite(const void *data, size_t bytes) override;

private:
    uint64_t _offset = 0;
    std::string _memory;
};

} // namespace mediakit

#endif // defined(ENABLE_MKV)
#endif // S3MEDIAKIT_MKV_H