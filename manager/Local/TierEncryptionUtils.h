#ifndef S3MEDIAKIT_TIERENCRYPTIONUTILS_H
#define S3MEDIAKIT_TIERENCRYPTIONUTILS_H

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <mutex>
#include <cstdio>
#include "Util/MD5.h"

namespace managerkit {

enum class RecordEncryptionMode {
    kNone = 0,   // No encryption.
    kLight = 1,  // Encrypt the whole moov payload plus every complete keyframe
                 // sample. Non-keyframe samples stay in plaintext.
    kStrong = 2, // Encrypt all bytes written to disk.
    kMax = 3
};

struct EncKeyConfig {
    RecordEncryptionMode mode = RecordEncryptionMode::kNone;
    std::vector<uint8_t> salt_enc;
    std::vector<uint8_t> salt_hmac;
};

// Key derivation: MD5(password_hash) -> PBKDF2 -> AES key + HMAC key
struct DerivedKeys {
    std::vector<uint8_t> enc_key;   // 32 byte, AES-256
    std::vector<uint8_t> hmac_key;  // 32 byte, HMAC-SHA256
};

class RecordEncryptionUtils {
public:
    static RecordEncryptionMode encryptionModeFromString(const std::string &s);

    static DerivedKeys deriveKeys(const std::string &password_hash,
                                const std::vector<uint8_t> &salt_enc,
                                const std::vector<uint8_t> &salt_hmac,
                                int iterations = 210000);

    static std::string computeVerificationHash(const std::string &password,
                                                const std::string &verify_salt_hex);

    static bool verifyPassword(const std::string &password_input,
                                const std::string &verify_salt_hex,
                                const std::string &stored_hash);

    static bool findMoovBox(const std::string &path, int64_t &out_offset, int64_t &out_size);

    static std::string stripEncSuffix(const std::string &path);

    static bool hasEncSuffix(const std::string &path);

    static std::string toggleEncSuffix(const std::string &path);

    static std::string computeTargetPath(const std::string &current_path, RecordEncryptionMode new_mode);

    static const std::string &encryptedFileSuffix();

    static bool hasEncryptionTrailer(const std::string &path);

    static std::string bytesToHexStr(const std::vector<uint8_t> &b);

    static std::vector<uint8_t> hexStrToBytes(const std::string &hex);

    static std::string makeRandomHex(size_t byte_len);

    static std::vector<uint8_t> randomBytesVec(size_t byte_len);

    static int64_t parseRecordingTimestamp(const std::string &path);

    static std::vector<uint8_t> pbkdf2(const std::string &input,
                                    const std::vector<uint8_t> &salt,
                                    int iterations, int keyLen);
};

class DerivedKeyCache {
public:
    static DerivedKeys get(const std::string &password_hash,
                           const std::vector<uint8_t> &salt_enc,
                           const std::vector<uint8_t> &salt_hmac,
                           int iterations = 210000);

    // Called when a tier password/salt changes so stale entries cannot pile up.
    static void clear();

private:
    static std::mutex _mtx;
    static std::map<std::string, DerivedKeys> _cache;
    static constexpr size_t kMaxEntries = 64;
};

class OffsetKeyedCtrCipher {
public:
    OffsetKeyedCtrCipher(std::vector<uint8_t> key, std::vector<uint8_t> baseIv);
    std::vector<uint8_t> xcryptAtOffset(const uint8_t *data, size_t len, int64_t offset) const;

private:
    std::vector<uint8_t> _key;
    std::vector<uint8_t> _baseIv;
};

struct ByteRange {
    int64_t offset = 0;
    int64_t length = 0;

    ByteRange() = default;
    ByteRange(int64_t off, int64_t len) : offset(off), length(len) {}
    int64_t end() const { return offset + length; }
};

/**
 * Layout of the 80-byte trailer appended after the MP4 content:
 *   [ 0..15] nonce (base IV for AES-CTR; low 64 bits are always zero)
 *   [16..23] moov_start (big endian, absolute offset of the moov box header)
 *   [24..31] moov_end   (big endian, moov_start + moov box size)
 *   [32..35] magic
 *   [36..39] version
 *   [40..43] mode        (RecordEncryptionMode the file was written with)
 *   [44..47] reserved    (zero)
 *   [48..79] HMAC-SHA256 over bytes [0, content_len + 48)
 *
 * The moov box header itself is left in plaintext, so a reader can recover the
 * header size (8 or 16 bytes for largesize) without holding the key.
 *
 * `mode` was added in version 3 and is what makes the trailer self-describing.
 * Version 2 stored only the key material, so nothing on disk recorded whether a
 * file was Light or Strong. When a pool switched mode without changing its
 * password, the HMAC still verified under the unchanged key and the file was
 * mistaken for one that had already been converted, which left Strong files
 * being decrypted as if they were Light.
 */
struct EncryptedFileTrailer {
    static constexpr size_t kSize = 80;
    static constexpr uint32_t kMagic = 0x53454E43;
    static constexpr uint32_t kVersion = 3;

    std::vector<uint8_t> nonce;   // 16 byte, base_iv for AES-CTR
    int64_t moov_start = 0;
    int64_t moov_end = 0;
    RecordEncryptionMode mode = RecordEncryptionMode::kNone;
};

class RecordFileEncryptor {
public:
    /**
    * Collect encryption regions for Light mode.
    * Encrypts the moov payload and all keyframe samples.
    */
    static std::vector<ByteRange> collectLightModeRegions(const std::string &plain_path, EncryptedFileTrailer &out_trailer);

    /**
    * Collect the encryption region for Strong mode: the entire file.
    */
    static std::vector<ByteRange> collectStrongModeRegions(const std::string &plain_path, EncryptedFileTrailer &out_trailer);

    /**
    * Encrypt the specified regions in place and append the HMAC trailer.
    * Generates and stores a random nonce in the trailer.
    */
    static bool encryptFileInPlace(const std::string &path, const std::vector<ByteRange> &regions,
        EncryptedFileTrailer &trailer, const std::vector<uint8_t> &enc_key, const std::vector<uint8_t> &hmac_key);

    /**
    * Encrypt or decrypt the specified regions in place using CTR mode.
    */
    static bool xcryptRegionsInPlace(const std::string &path, const std::vector<ByteRange> &regions,
                                    const std::vector<uint8_t> &enc_key, const std::vector<uint8_t> &base_iv);

    /**
    * Read and verify the HMAC trailer.
    * Returns the content length excluding the trailer.
    */
    static bool readAndVerifyTrailer(const std::string &path, const std::vector<uint8_t> &hmac_key,
                                    EncryptedFileTrailer &out_trailer, int64_t &out_content_len);

    /**
    * Parse moov sample tables and collect all keyframe regions.
    */
    static std::vector<ByteRange> collectKeyframeRegionsFromMoov(const std::vector<uint8_t> &moov_buf);

    /**
    * Size of a box header given its first 4 bytes: 16 when the box uses the
    * 64-bit largesize form, otherwise 8. The moov header is never encrypted,
    * so both writer and reader can call this and agree on where the encrypted
    * payload starts.
    */
    static size_t boxHeaderSize(const uint8_t *first4);

    /**
    * Rebuild the Light-mode region list from an already decrypted moov buffer.
    * Shared by the playback path and by decryptFileToPlain so the two cannot
    * drift apart.
    */
    static std::vector<ByteRange> buildLightModeRegions(const std::vector<uint8_t> &plain_moov,
                                                        int64_t moov_start,
                                                        int64_t moov_end);
    /**
    * Sort and merge overlapping regions.
    */
    static void normalizeRegions(std::vector<ByteRange> &regions);

private:
    static bool parseTrailer(FILE *fp, EncryptedFileTrailer &out_trailer, int64_t &out_content_len);
};

} // namespace managerkit

#endif //S3MEDIAKIT_TIERENCRYPTIONUTILS_H
