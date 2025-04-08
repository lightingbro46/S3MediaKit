#ifndef S3MEDIAKIT_PROCESSINTERFACE_H
#define S3MEDIAKIT_PROCESSINTERFACE_H

#include <stdint.h>
#include <memory>

namespace mediakit {

class ProcessInterface {
public:
    using Ptr = std::shared_ptr<ProcessInterface>;
    virtual ~ProcessInterface() = default;

    /**
     * Input rtp
     * @param is_udp Whether it is udp mode
     * @param data rtp data pointer
     * @param data_len rtp data length
     * @return Whether the parsing is successful
      */
    virtual bool inputRtp(bool is_udp, const char *data, size_t data_len) = 0;

    /**
     * Refresh and output all caches
     */
    virtual void flush() {}
};

}//namespace mediakit
#endif //S3MEDIAKIT_PROCESSINTERFACE_H
