#ifndef LOCAL_TIMEFILE_H
#define LOCAL_TIMEFILE_H

#include <memory>
#include <string>
#include "Util/File.h"
#include "proto/timeblock.pb.h"

namespace mediakit {

class TimeFileIO;

class TimeWriter {
public:
    using Ptr = std::shared_ptr<TimeWriter>;

    TimeWriter(std::shared_ptr<TimeFileIO> io) : _io(std::move(io)) {}
    ~TimeWriter() = default;

public:
    int write(const void *data, size_t bytes);
    int seek(int64_t offset);
    int64_t tell();
    int flush();

private:
    std::shared_ptr<TimeFileIO> _io;
};

class TimeReader {
public:
    using Ptr = std::shared_ptr<TimeReader>;

    TimeReader(std::shared_ptr<TimeFileIO> &io) : _io(std::move(io)) {}
    ~TimeReader() = default;

public:
    int read(void *data, size_t bytes);
    int seek(int64_t offset);
    int64_t tell();

private:
    std::shared_ptr<TimeFileIO> _io;
};


// Abstract interface class for time block file IO
class TimeFileIO : public std::enable_shared_from_this<TimeFileIO> {
public:
    using Ptr = std::shared_ptr<TimeFileIO>;
    using Writer = std::shared_ptr<TimeWriter>;
    using Reader = std::shared_ptr<TimeReader>;

    virtual ~TimeFileIO() = default;

    /**
     * Create an time writer
     * @return time writer
     */
    virtual Writer createWriter();

    /**
     * Create an time reader
     * @return time reader
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

    /**
     * Flush a certain amount of data to the file
     * @param data Data pointer
     * @param bytes Data length
     * @return Whether it is successful (0 successful)
     */
    virtual int onFlush() { return 0; }
};

// Disk Time file class
class TimeFileDisk : public TimeFileIO { 
public:
    using Ptr = std::shared_ptr<TimeFileDisk>;

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
    int onFlush() override;

private:
    std::shared_ptr<FILE> _file;
};

class TimeFileMemory : public TimeFileIO {
public:
    using Ptr = std::shared_ptr<TimeFileMemory>;
    TimeFileMemory(const std::string &buf = "");

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

#endif // LOCAL_TIMEFILE_H