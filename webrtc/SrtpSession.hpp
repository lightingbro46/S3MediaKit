#ifndef MS_RTC_SRTP_SESSION_HPP
#define MS_RTC_SRTP_SESSION_HPP

#include "Util/Byte.hpp"

#include <memory>

typedef struct srtp_ctx_t_ *srtp_t;

namespace RTC {

class DepLibSRTP;

class SrtpSession {
public:
    using Ptr = std::shared_ptr<SrtpSession>;
    enum class CryptoSuite {
        NONE = 0,
        AES_CM_128_HMAC_SHA1_80 = 1,
        AES_CM_128_HMAC_SHA1_32,
        AEAD_AES_256_GCM,
        AEAD_AES_128_GCM
    };

public:
    enum class Type { INBOUND = 1, OUTBOUND };

public:
    SrtpSession(Type type, CryptoSuite cryptoSuite, uint8_t *key, size_t keyLen);
    ~SrtpSession();

public:
    bool EncryptRtp(uint8_t *data, int *len);
    bool DecryptSrtp(uint8_t *data, int *len);
    bool EncryptRtcp(uint8_t *data, int *len);
    bool DecryptSrtcp(uint8_t *data, int *len);
    void RemoveStream(uint32_t ssrc);

private:
    // Allocated by this.
    srtp_t session { nullptr };
    std::shared_ptr<DepLibSRTP> _env;
};

} // namespace RTC

#endif
