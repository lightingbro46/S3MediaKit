#ifndef S3MEDIAKIT_HTTPREQUESTSPLITTER_H
#define S3MEDIAKIT_HTTPREQUESTSPLITTER_H

#include <string>
#include "Network/Buffer.h"

namespace mediakit {

class HttpRequestSplitter {
public:
    HttpRequestSplitter();
    virtual ~HttpRequestSplitter() = default;

    /**
     * Add data
     * @param data Data to be added
     * @param len Data length
     * @warning Actual memory must be no less than len + 1. strstr is used internally for searching. To prevent out-of-bounds search, a '\0' terminator is set at the @p len + 1 position.
     */
    virtual void input(const char *data, size_t len);

    /**
     * Restore initial settings
     */
    void reset();

    /**
     * Remaining data size
     */
    size_t remainDataSize();

    /**
     * Get remaining data pointer
     */
    const char *remainData() const;

    /**
     * Set maximum cache size
     */
    void setMaxCacheSize(size_t max_cache_size);

protected:
    /**
     * Receive request header
     * @param data Request header data
     * @param len Request header length
     *
     * @return Content length after request header,
     *  <0 : Represents that all subsequent data is content, in which case the subsequent content will be called back in segments through the onRecvContent function
     *  0 : Represents that the subsequent data is still the request header,
     *  >0 : Represents that the subsequent data is fixed-length content, in which case the content will be cached and called back through the onRecvContent function once all content is received
     */
    virtual ssize_t onRecvHeader(const char *data,size_t len) = 0;

    /**
     * Receive content fragments or all data
     * onRecvHeader function returns >0, then it is all data
     * @param data Content fragments or all data
     * @param len Data length
     */
    virtual void onRecvContent(const char *data,size_t len) {};

    /**
     * Determine if there is a packet tail in the data
     * @param data Data pointer
     * @param len Data length
     * @return nullptr represents that the packet position is not found, otherwise returns the packet tail pointer
     */
    virtual const char *onSearchPacketTail(const char *data, size_t len);

    /**
     * Set content len
     */
    void setContentLen(ssize_t content_len);

private:
    ssize_t _content_len = 0;
    size_t _max_cache_size = 0;
    size_t _remain_data_size = 0;
    toolkit::BufferLikeString _remain_data;
};

} /* namespace mediakit */

#endif //S3MEDIAKIT_HTTPREQUESTSPLITTER_H
