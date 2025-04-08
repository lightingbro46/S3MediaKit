#ifndef S3MEDIAKIT_MP4_H
#define S3MEDIAKIT_MP4_H

#if defined(ENABLE_MP4)

#include <memory>
#include <string>
#include "mp4-writer.h"
#include "mov-writer.h"
#include "mov-reader.h"
#include "mpeg4-hevc.h"
#include "mpeg4-avc.h"
#include "mpeg4-aac.h"
#include "mov-buffer.h"
#include "mov-format.h"

namespace mediakit {

// Abstract interface class for mp4 file IO
class MP4FileIO : public std::enable_shared_from_this<MP4FileIO> {
public:
    using Ptr = std::shared_ptr<MP4FileIO>;
    using Writer = std::shared_ptr<mp4_writer_t>;
    using Reader = std::shared_ptr<mov_reader_t>;

    virtual ~MP4FileIO() = default;

    /**
     * Create an mp4 muxer
     * @param flags Supports 0, MOV_FLAG_FASTSTART, MOV_FLAG_SEGMENT
     * @param is_fmp4 Whether it is fmp4 or ordinary mp4
     * @return mp4 muxer
     */
    virtual Writer createWriter(int flags, bool is_fmp4 = false);

    /**
     * Create an mp4 demuxer
     * @return mp4 demuxer
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

// Disk MP4 file class
class MP4FileDisk : public MP4FileIO {
public:
    using Ptr = std::shared_ptr<MP4FileDisk>;

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

class MP4FileMemory : public MP4FileIO{
public:
    using Ptr = std::shared_ptr<MP4FileMemory>;

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

}//namespace mediakit
#endif //defined(ENABLE_MP4)
#endif //S3MEDIAKIT_MP4_H
