#include "HttpRequestSplitter.h"
#include "Util/logger.h"
#include "Util/util.h"
using namespace toolkit;
using namespace std;

// Protocol parsing maximum cache 4MB data
static constexpr size_t kMaxCacheSize = 4 * 1024 * 1024;

namespace mediakit {

void HttpRequestSplitter::input(const char *data,size_t len) {
    {
        auto size = remainDataSize();
        if (size > _max_cache_size) {
            // If too much data is cached and cannot be processed, throw an exception
            reset();
            throw std::out_of_range("remain data size is too huge, now cleared:" + to_string(size));
        }
    }
    const char *ptr = data;
    if(!_remain_data.empty()){
        _remain_data.append(data,len);
        data = ptr = _remain_data.data();
        len = _remain_data.size();
    }

    splitPacket:

    /*
     * Ensure the last byte of ptr is 0 to prevent strstr from going out of bounds
     * Since S3ToolKit ensures that the last byte of memory is a reserved unused byte and set to 0,
     * so there is no need to set it to 0 again here
     * But the upper layer data may come from other channels, so it is better to set it to 0 for safety
     */

    char &tail_ref = ((char *) ptr)[len];
    char tail_tmp = tail_ref;
    tail_ref = 0;

    // Data is processed according to the request header
    const char *index = nullptr;
    _remain_data_size = len;
    while (_content_len == 0 && _remain_data_size > 0 && (index = onSearchPacketTail(ptr,_remain_data_size)) != nullptr) {
        if (index == ptr) {
            break;
        }
        if (index < ptr || index > ptr + _remain_data_size) {
            throw std::out_of_range("The logic exception of the upper layer subcontracting");
        }
        // _content_len == 0, this is the request header
        const char *header_ptr = ptr;
        ssize_t header_size = index - ptr;
        ptr = index;
        _remain_data_size = len - (ptr - data);
        _content_len = onRecvHeader(header_ptr, header_size);
    }

    /*
     * Restore the last byte
     * Move it here to prevent HttpRequestSplitter::reset() from causing memory failure
     */
    tail_ref = tail_tmp;

    if(_remain_data_size <= 0){
        // No remaining data, clear the cache
        _remain_data.clear();
        return;
    }

    if(_content_len == 0){
        // HTTP header not found yet, cache is located at the remaining data part
        _remain_data.assign(ptr,_remain_data_size);
        return;
    }

    // HTTP header has been found
    if(_content_len > 0){
        // Data is processed according to fixed length content
        if(_remain_data_size < (size_t)_content_len){
            // Insufficient data, cache is located at the remaining data part
            _remain_data.assign(ptr, _remain_data_size);
            return;
        }
        // Content data received and content reception completed
        onRecvContent(ptr,_content_len);

        _remain_data_size -= _content_len;
        ptr += _content_len;
        // Content processing completed, subsequent data is treated as request header
        _content_len = 0;

        if(_remain_data_size > 0){
            // There is still data that has not been processed
            _remain_data.assign(ptr,_remain_data_size);
            data = ptr = (char *)_remain_data.data();
            len = _remain_data.size();
            goto splitPacket;
        }
        _remain_data.clear();
        return;
    }


    // _content_len < 0; Data is processed according to variable length content
    onRecvContent(ptr,_remain_data_size); // Consumption of all remaining data
    _remain_data.clear();
}

void HttpRequestSplitter::setContentLen(ssize_t content_len) {
    _content_len = content_len;
}

void HttpRequestSplitter::reset() {
    _content_len = 0;
    _remain_data_size = 0;
    _remain_data.clear();
}

const char *HttpRequestSplitter::onSearchPacketTail(const char *data,size_t len) {
    auto pos = strstr(data,"\r\n\r\n");
    if(pos == nullptr){
        return nullptr;
    }
    return  pos + 4;
}

size_t HttpRequestSplitter::remainDataSize() {
    return _remain_data_size;
}

const char *HttpRequestSplitter::remainData() const {
    return _remain_data.data();
}

void HttpRequestSplitter::setMaxCacheSize(size_t max_cache_size) {
    if (!max_cache_size) {
        max_cache_size = kMaxCacheSize;
    }
    _max_cache_size = max_cache_size;
}

HttpRequestSplitter::HttpRequestSplitter() {
    setMaxCacheSize(0);
}

} /* namespace mediakit */

