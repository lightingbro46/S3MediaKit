#ifndef S3MEDIAKIT_FILEREADER_H
#define S3MEDIAKIT_FILEREADER_H

#include <stdlib.h>
#include <memory>
#include "Network/Buffer.h"
#include "Util/ResourcePool.h"
#include "Util/logger.h"
#include "Thread/WorkThreadPool.h"

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b) )
#endif //MIN

namespace mediakit {

/**
 * Base class definition for http content part
 */
class HttpBody : public std::enable_shared_from_this<HttpBody>{
public:
    using Ptr = std::shared_ptr<HttpBody>;
    virtual ~HttpBody() = default;

    /**
     * Remaining data size, if -1 is returned, then content-length is not set
     */
    virtual int64_t remainSize() { return 0;};

    /**
     * Read a certain number of bytes, the returned size may be less than size
     * @param size Request size
     * @return Byte object, if it is read, please return nullptr
     */
    virtual toolkit::Buffer::Ptr readData(size_t size) { return nullptr;};

    /**
     * Asynchronously request to read a certain number of bytes, the returned size may be less than size
     * @param size Request size
     * @param cb Callback function
     */
    virtual void readDataAsync(size_t size,const std::function<void(const toolkit::Buffer::Ptr &buf)> &cb){
        // Since unix and linux read files through mmap, putting file reading operations in the background thread does not improve performance
        // On the contrary, frequent thread switching will lead to performance degradation and increased latency, so we get the file content synchronously by default
        // (Actually, there is no reading, the file data is copied in the kernel state when copying)
        cb(readData(size));
    }

    /**
     * Use sendfile to optimize file sending
     * @param fd socket fd
     * @return 0 success, other error codes
     */
    virtual int sendFile(int fd) {
        return -1;
    }
};

/**
 * std::string type content
 */
class HttpStringBody : public HttpBody{
public:
    using Ptr = std::shared_ptr<HttpStringBody>;
    HttpStringBody(std::string str);

    int64_t remainSize() override;
    toolkit::Buffer::Ptr readData(size_t size) override ;

private:
    size_t _offset = 0;
    mutable std::string _str;
};

/**
 * Buffer type content
 */
class HttpBufferBody : public HttpBody{
public:
    using Ptr = std::shared_ptr<HttpBufferBody>;
    HttpBufferBody(toolkit::Buffer::Ptr buffer);

    int64_t remainSize() override;
    toolkit::Buffer::Ptr readData(size_t size) override;

private:
    toolkit::Buffer::Ptr _buffer;
};

/**
 * File type content
 */
class HttpFileBody : public HttpBody {
public:
    using Ptr = std::shared_ptr<HttpFileBody>;

    /**
     * Constructor
     * @param file_path File path
     * @param use_mmap Whether to use mmap to access the file
     */
    HttpFileBody(const std::string &file_path, bool use_mmap = true);

    /**
     * Set the reading range
     * @param offset Offset relative to the file header
     * @param max_size Maximum number of bytes to read
     */
    void setRange(uint64_t offset, uint64_t max_size);

    int64_t remainSize() override;
    toolkit::Buffer::Ptr readData(size_t size) override;
    int sendFile(int fd) override;

private:
    int64_t _read_to = 0;
    uint64_t _file_offset = 0;
    std::shared_ptr<FILE> _fp;
    std::shared_ptr<char> _map_addr;
    toolkit::ResourcePool<toolkit::BufferRaw> _pool;
};

class HttpArgs;

/**
 * http MultiForm way to submit http content
 */
class HttpMultiFormBody : public HttpBody {
public:
    using Ptr = std::shared_ptr<HttpMultiFormBody>;

    /**
     * Constructor
     * @param args http submission parameter list
     * @param filePath File path
     * @param boundary Boundary string
     */
    HttpMultiFormBody(const HttpArgs &args,const std::string &filePath,const std::string &boundary = "0xKhTmLbOuNdArY");
    int64_t remainSize() override ;
    toolkit::Buffer::Ptr readData(size_t size) override;

public:
    static std::string multiFormBodyPrefix(const HttpArgs &args,const std::string &boundary,const std::string &fileName);
    static std::string multiFormBodySuffix(const std::string &boundary);
    static std::string multiFormContentType(const std::string &boundary);

private:
    uint64_t _offset = 0;
    int64_t _totalSize;
    std::string _bodyPrefix;
    std::string _bodySuffix;
    HttpFileBody::Ptr _fileBody;
};

}//namespace mediakit

#endif //S3MEDIAKIT_FILEREADER_H
