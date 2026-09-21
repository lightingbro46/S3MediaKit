#include "TierEncryptionUtils.h"
#include <ctime>
#include <functional>
#include <unistd.h>

using namespace toolkit;
using namespace std;

namespace managerkit {

/////////////////////////////////////////////////////RecordEncryptionUtils/////////////////////////////////////////////////////////

RecordEncryptionMode RecordEncryptionUtils::encryptionModeFromString(const std::string &s) {
    if (s == "light" || s == "Light" || s == "LIGHT") return RecordEncryptionMode::kLight;
    if (s == "strong" || s == "Strong" || s == "STRONG") return RecordEncryptionMode::kStrong;
    if (s == "none" || s == "None" || s == "NONE") return RecordEncryptionMode::kNone;
    return RecordEncryptionMode::kMax;
}

DerivedKeys RecordEncryptionUtils::deriveKeys(const std::string &password_hash,
                            const std::vector<uint8_t> &salt_enc,
                            const std::vector<uint8_t> &salt_hmac,
                            int iterations) {
    auto md5_hash = toolkit::MD5(password_hash).hexdigest();

    DerivedKeys k;
    k.enc_key  = pbkdf2(md5_hash, salt_enc, iterations, 32);
    k.hmac_key = pbkdf2(md5_hash, salt_hmac, iterations, 32);
    return k;
}

/////////////////////////////////////////////////////DerivedKeyCache/////////////////////////////////////////////////////////

std::mutex DerivedKeyCache::_mtx;
std::map<std::string, DerivedKeys> DerivedKeyCache::_cache;

DerivedKeys DerivedKeyCache::get(const std::string &password_hash,
                                 const std::vector<uint8_t> &salt_enc,
                                 const std::vector<uint8_t> &salt_hmac,
                                 int iterations) {
    std::string cache_key = password_hash;
    cache_key += '|';
    cache_key += RecordEncryptionUtils::bytesToHexStr(salt_enc);
    cache_key += '|';
    cache_key += RecordEncryptionUtils::bytesToHexStr(salt_hmac);
    cache_key += '|';
    cache_key += std::to_string(iterations);

    {
        std::lock_guard<std::mutex> lock(_mtx);
        auto it = _cache.find(cache_key);
        if (it != _cache.end()) {
            return it->second;
        }
    }

    // Derive outside the lock: two PBKDF2 runs take ~245 ms and must not block
    // other threads that already have their keys cached.
    DerivedKeys keys = RecordEncryptionUtils::deriveKeys(password_hash, salt_enc, salt_hmac, iterations);

    {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_cache.size() >= kMaxEntries) {
            // Only grows when configuration changes repeatedly. Dropping the
            // whole map is cheaper than tracking usage and costs one re-derive.
            _cache.clear();
        }
        _cache[cache_key] = keys;
    }
    return keys;
}

void DerivedKeyCache::clear() {
    std::lock_guard<std::mutex> lock(_mtx);
    _cache.clear();
}


std::string RecordEncryptionUtils::computeVerificationHash(const std::string &password,
                                            const std::string &verify_salt_hex) {
    return toolkit::MD5(password + ":" + verify_salt_hex).hexdigest();
}

bool RecordEncryptionUtils::verifyPassword(const std::string &password_input,
                            const std::string &verify_salt_hex,
                            const std::string &stored_hash) {
    auto computed = computeVerificationHash(password_input, verify_salt_hex);
    if (computed.size() != stored_hash.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < computed.size(); i++) diff |= (computed[i] ^ stored_hash[i]);
    return diff == 0;
}


bool RecordEncryptionUtils::findMoovBox(const std::string &path, int64_t &out_offset, int64_t &out_size) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int64_t offset = 0;
    while (offset + 8 <= file_size) {
        unsigned char header[8];
        if (fseek(fp, offset, SEEK_SET) != 0) break;
        if (fread(header, 1, 8, fp) != 8) break;

        uint32_t box_size = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
        std::string box_type((char *)header + 4, 4);

        int64_t actual_size = box_size;
        if (box_size == 1) {
            unsigned char large[8];
            if (fread(large, 1, 8, fp) != 8) break;
            actual_size = 0;
            for (int i = 0; i < 8; i++) actual_size = (actual_size << 8) | large[i];
        } else if (box_size == 0) {
            actual_size = file_size - offset;
        }

        if (actual_size < 8 || offset + actual_size > file_size) break;

        if (box_type == "moov") {
            fclose(fp);
            out_offset = offset;
            out_size = actual_size;
            return true;
        }
        offset += actual_size;
    }

    fclose(fp);
    return false;
}

bool RecordEncryptionUtils::hasEncSuffix(const std::string &path) {
    const auto &suf = encryptedFileSuffix();
    return path.size() > suf.size() && path.compare(path.size() - suf.size(), suf.size(), suf) == 0;
}

std::string RecordEncryptionUtils::stripEncSuffix(const std::string &path) {
    if (hasEncSuffix(path)) {
        return path.substr(0, path.size() - encryptedFileSuffix().size());
    }
    return path;
}

std::string RecordEncryptionUtils::toggleEncSuffix(const std::string &path) {
    return hasEncSuffix(path) ? stripEncSuffix(path) : path + encryptedFileSuffix();
}


std::string RecordEncryptionUtils::computeTargetPath(const std::string &current_path, RecordEncryptionMode new_mode) {
    std::string base = stripEncSuffix(current_path);
    if (new_mode == RecordEncryptionMode::kNone) {
        return base;
    }
    return base + encryptedFileSuffix();
}

std::vector<uint8_t> RecordEncryptionUtils::pbkdf2(const std::string &input,
                                const std::vector<uint8_t> &salt,
                                int iterations, int keyLen) {
    std::vector<uint8_t> out(keyLen);
    if (1 != PKCS5_PBKDF2_HMAC(input.c_str(), (int)input.size(),
                                salt.data(), (int)salt.size(),
                                iterations, EVP_sha256(), keyLen, out.data())) {
        throw std::runtime_error("PBKDF2 failed");
    }
    return out;
}

std::string RecordEncryptionUtils::bytesToHexStr(const std::vector<uint8_t> &b) {
    static const char *hex = "0123456789abcdef";
    std::string s; s.reserve(b.size() * 2);
    for (auto byte : b) { s.push_back(hex[byte >> 4]); s.push_back(hex[byte & 0xF]); }
    return s;
}

std::vector<uint8_t> RecordEncryptionUtils::hexStrToBytes(const std::string &hex) {
    // Must not throw: this runs inside the playback path on values read from
    // the database, where a truncated or corrupted column is possible.
    if (hex.size() % 2 != 0) {
        return {};
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return out;
}

std::string RecordEncryptionUtils::makeRandomHex(size_t byte_len) {
    return bytesToHexStr(randomBytesVec(byte_len));
}

std::vector<uint8_t> RecordEncryptionUtils::randomBytesVec(size_t byte_len) {
    std::vector<uint8_t> buf(byte_len);
    // Silently accepting a failure here would leave the buffer all zeros. For a
    // nonce that means the same keystream is reused across every file written
    // with the same key, which removes the protection entirely, so this must be
    // a hard failure.
    if (1 != RAND_bytes(buf.data(), (int)byte_len)) {
        throw std::runtime_error("RAND_bytes failed: no secure randomness available");
    }
    return buf;
}

int64_t RecordEncryptionUtils::parseRecordingTimestamp(const std::string &path) {
    auto slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int n = std::sscanf(base.c_str(), "%d-%d-%d-%d-%d-%d-", &y, &mo, &d, &h, &mi, &s);
    if (n != 6) return -1;

    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return -1;

    struct tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;

    time_t ts = std::mktime(&t);
    if (ts == (time_t)-1) return -1;
    return (int64_t)ts;
}

const std::string& RecordEncryptionUtils::encryptedFileSuffix() {
    static const std::string s = ".enc";
    return s;
}

bool RecordEncryptionUtils::hasEncryptionTrailer(const std::string &path) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    int64_t file_size = ftell(fp);
    if (file_size < (int64_t)EncryptedFileTrailer::kSize) { fclose(fp); return false; }

    // magic + version sit right after nonce(16) + moov_start(8) + moov_end(8)
    int64_t magic_off = file_size - (int64_t)EncryptedFileTrailer::kSize + 32;
    uint8_t buf[8];
    bool ok = (fseek(fp, magic_off, SEEK_SET) == 0) && (fread(buf, 1, 8, fp) == 8);
    fclose(fp);
    if (!ok) return false;

    uint32_t magic = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] << 8) | buf[3];
    uint32_t version = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                       ((uint32_t)buf[6] << 8) | buf[7];
    return magic == EncryptedFileTrailer::kMagic && version == EncryptedFileTrailer::kVersion;
}

/////////////////////////////////////////////////////OffsetKeyedCtrCipher/////////////////////////////////////////////////////////

OffsetKeyedCtrCipher::OffsetKeyedCtrCipher(std::vector<uint8_t> key, std::vector<uint8_t> baseIv)
    : _key(std::move(key)), _baseIv(std::move(baseIv)) {
    if (_key.size() != 32) throw std::invalid_argument("Key must be 32 bytes");
    if (_baseIv.size() != 16) throw std::invalid_argument("iv must be 16 bytes");
}

std::vector<uint8_t> OffsetKeyedCtrCipher::xcryptAtOffset(const uint8_t *data, size_t len, int64_t offset) const {
    if (offset < 0) throw std::invalid_argument("Invalid offset");

    uint64_t blockOffset = (uint64_t)offset / 16;
    uint64_t byteInBlock = (uint64_t)offset % 16;

    std::vector<uint8_t> iv = _baseIv;
    uint64_t counter = 0;
    for (int i = 8; i < 16; ++i) counter = (counter << 8) | iv[i];
    counter += blockOffset;
    for (int i = 15; i >= 8; --i) { iv[i] = (uint8_t)(counter & 0xFF); counter >>= 8; }

    std::vector<uint8_t> padded(byteInBlock + len);
    std::memcpy(padded.data() + byteInBlock, data, len);

    std::vector<uint8_t> out(padded.size());
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    int outl = 0, total = 0;
    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, _key.data(), iv.data())) {
        EVP_CIPHER_CTX_free(ctx); throw std::runtime_error("CTR init failed");
    }
    if (1 != EVP_EncryptUpdate(ctx, out.data(), &outl, padded.data(), (int)padded.size())) {
        EVP_CIPHER_CTX_free(ctx); throw std::runtime_error("CTR update failed");
    }
    total = outl;
    if (1 != EVP_EncryptFinal_ex(ctx, out.data() + outl, &outl)) {
        EVP_CIPHER_CTX_free(ctx); throw std::runtime_error("CTR final failed");
    }
    total += outl;
    EVP_CIPHER_CTX_free(ctx);
    if (total != (int)padded.size()) {
        throw std::runtime_error("CTR produced unexpected length");
    }
    out.resize(total);

    return std::vector<uint8_t>(out.begin() + byteInBlock, out.end());
}

/////////////////////////////////////////////////////MP4 box helpers/////////////////////////////////////////////////////////

namespace {

static uint32_t rdU32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rdU64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static bool findChildBox(const std::vector<uint8_t> &buf, size_t start, size_t end,
                          const char type[4], size_t &out_off, size_t &out_size) {
    size_t off = start;
    while (off + 8 <= end) {
        uint64_t sz = rdU32(buf.data() + off);
        size_t hdr = 8;
        if (sz == 1) {
            if (off + 16 > end) return false;
            sz = rdU64(buf.data() + off + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = end - off;
        }
        if (sz < hdr || off + sz > end) return false;
        if (std::memcmp(buf.data() + off + 4, type, 4) == 0) {
            out_off = off;
            out_size = (size_t)sz;
            return true;
        }
        off += (size_t)sz;
    }
    return false;
}

static void forEachChildBox(const std::vector<uint8_t> &buf, size_t start, size_t end,
                             const char type[4],
                             const std::function<void(size_t off, size_t size)> &cb) {
    size_t off = start;
    while (off + 8 <= end) {
        uint64_t sz = rdU32(buf.data() + off);
        size_t hdr = 8;
        if (sz == 1) {
            if (off + 16 > end) return;
            sz = rdU64(buf.data() + off + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = end - off;
        }
        if (sz < hdr || off + sz > end) return;
        if (std::memcmp(buf.data() + off + 4, type, 4) == 0) {
            cb(off, (size_t)sz);
        }
        off += (size_t)sz;
    }
}

static bool descendBoxPath(const std::vector<uint8_t> &buf, size_t start, size_t end,
                            const std::vector<std::string> &path,
                            size_t &out_content_start, size_t &out_content_end) {
    size_t cur_start = start, cur_end = end;
    for (const auto &name : path) {
        size_t off = 0, sz = 0;
        if (!findChildBox(buf, cur_start, cur_end, name.c_str(), off, sz))
            return false;
        uint64_t raw = rdU32(buf.data() + off);
        size_t hdr = (raw == 1) ? 16 : 8;
        cur_start = off + hdr;
        cur_end = off + sz;
    }
    out_content_start = cur_start;
    out_content_end = cur_end;
    return true;
}

struct TrackSampleTable {
    std::vector<uint32_t> sync_samples;     // stss: 1-based sample indices of keyframes
    std::vector<uint64_t> chunk_offsets;    // stco/co64 chunk offsets
    std::vector<uint32_t> sample_sizes;     // stsz: per-sample sizes (empty if uniform_size is used)
    uint32_t uniform_size = 0;              // stsz: fixed sample size when sample_size != 0
    uint32_t sample_count = 0;              // Total number of samples
    std::vector<std::pair<uint32_t, uint32_t>> stsc; // stsc: first_chunk and samples_per_chunk entries
};

static bool parseStbl(const std::vector<uint8_t> &buf, size_t stbl_start, size_t stbl_end,
                       TrackSampleTable &out) {
    size_t off = 0, sz = 0;

    if (findChildBox(buf, stbl_start, stbl_end, "stss", off, sz) && sz >= 16) {
        uint32_t cnt = rdU32(buf.data() + off + 12);
        size_t need = off + 16 + (size_t)cnt * 4;
        if (need > stbl_end) return false;
        out.sync_samples.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++)
            out.sync_samples.push_back(rdU32(buf.data() + off + 16 + i * 4));
    }

    // stsz / stz2
    if (findChildBox(buf, stbl_start, stbl_end, "stsz", off, sz) && sz >= 20) {
        out.uniform_size = rdU32(buf.data() + off + 12);
        out.sample_count = rdU32(buf.data() + off + 16);
        if (out.uniform_size == 0) {
            size_t need = off + 20 + (size_t)out.sample_count * 4;
            if (need > stbl_end) return false;
            out.sample_sizes.reserve(out.sample_count);
            for (uint32_t i = 0; i < out.sample_count; i++)
                out.sample_sizes.push_back(rdU32(buf.data() + off + 20 + i * 4));
        }
    } else {
        return false;
    }

    if (findChildBox(buf, stbl_start, stbl_end, "stco", off, sz) && sz >= 16) {
        uint32_t cnt = rdU32(buf.data() + off + 12);
        size_t need = off + 16 + (size_t)cnt * 4;
        if (need > stbl_end) return false;
        out.chunk_offsets.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++)
            out.chunk_offsets.push_back(rdU32(buf.data() + off + 16 + i * 4));
    } else if (findChildBox(buf, stbl_start, stbl_end, "co64", off, sz) && sz >= 16) {
        uint32_t cnt = rdU32(buf.data() + off + 12);
        size_t need = off + 16 + (size_t)cnt * 8;
        if (need > stbl_end) return false;
        out.chunk_offsets.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++)
            out.chunk_offsets.push_back(rdU64(buf.data() + off + 16 + i * 8));
    } else {
        return false;
    }

    // stsc
    if (findChildBox(buf, stbl_start, stbl_end, "stsc", off, sz) && sz >= 16) {
        uint32_t cnt = rdU32(buf.data() + off + 12);
        size_t need = off + 16 + (size_t)cnt * 12;
        if (need > stbl_end) return false;
        out.stsc.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t first_chunk = rdU32(buf.data() + off + 16 + i * 12);
            uint32_t spc = rdU32(buf.data() + off + 16 + i * 12 + 4);
            out.stsc.emplace_back(first_chunk, spc);
        }
    } else {
        return false;
    }

    return true;
}

static std::vector<int64_t> computeSampleOffsets(const TrackSampleTable &t) {
    std::vector<int64_t> offsets;
    if (t.sample_count == 0 || t.chunk_offsets.empty() || t.stsc.empty())
        return offsets;
    offsets.reserve(t.sample_count);

    auto sizeOf = [&](uint32_t idx) -> uint32_t {
        return t.uniform_size != 0 ? t.uniform_size
                                   : (idx < t.sample_sizes.size() ? t.sample_sizes[idx] : 0);
    };

    uint32_t sample_idx = 0;
    for (size_t e = 0; e < t.stsc.size() && sample_idx < t.sample_count; e++) {
        uint32_t first_chunk = t.stsc[e].first;
        uint32_t spc = t.stsc[e].second;
        uint32_t last_chunk = (e + 1 < t.stsc.size()) ? t.stsc[e + 1].first
                                                       : (uint32_t)t.chunk_offsets.size() + 1;
        if (first_chunk == 0 || spc == 0) return {};

        for (uint32_t c = first_chunk; c < last_chunk && sample_idx < t.sample_count; c++) {
            if (c - 1 >= t.chunk_offsets.size()) return {};
            int64_t pos = (int64_t)t.chunk_offsets[c - 1];
            for (uint32_t s = 0; s < spc && sample_idx < t.sample_count; s++) {
                offsets.push_back(pos);
                pos += (int64_t)sizeOf(sample_idx);
                sample_idx++;
            }
        }
    }
    return offsets;
}

} // namespace

/////////////////////////////////////////////////////RecordFileEncryptor/////////////////////////////////////////////////////////

size_t RecordFileEncryptor::boxHeaderSize(const uint8_t *first4) {
    return rdU32(first4) == 1 ? 16 : 8;
}

std::vector<ByteRange> RecordFileEncryptor::buildLightModeRegions(const std::vector<uint8_t> &plain_moov,
                                                                   int64_t moov_start,
                                                                   int64_t moov_end) {
    std::vector<ByteRange> regions;
    if (plain_moov.size() < 8 || moov_end <= moov_start) {
        return regions;
    }

    const int64_t hdr = (int64_t)boxHeaderSize(plain_moov.data());
    const int64_t content_off = moov_start + hdr;
    const int64_t content_len = moov_end - content_off;
    if (content_len <= 0) {
        return regions;
    }

    regions = collectKeyframeRegionsFromMoov(plain_moov);
    regions.emplace_back(content_off, content_len);
    normalizeRegions(regions);
    return regions;
}

void RecordFileEncryptor::normalizeRegions(std::vector<ByteRange> &regions) {
    regions.erase(std::remove_if(regions.begin(), regions.end(),
                                  [](const ByteRange &r) { return r.length <= 0; }),
                  regions.end());
    if (regions.empty()) return;

    std::sort(regions.begin(), regions.end(),
              [](const ByteRange &a, const ByteRange &b) { return a.offset < b.offset; });

    std::vector<ByteRange> merged;
    merged.push_back(regions.front());
    for (size_t i = 1; i < regions.size(); i++) {
        ByteRange &last = merged.back();
        const ByteRange &cur = regions[i];
        if (cur.offset <= last.end()) {
            int64_t new_end = std::max(last.end(), cur.end());
            last.length = new_end - last.offset;
        } else {
            merged.push_back(cur);
        }
    }
    regions.swap(merged);
}

std::vector<ByteRange> RecordFileEncryptor::collectKeyframeRegionsFromMoov(const std::vector<uint8_t> &moov_buf) {
    std::vector<ByteRange> regions;
    if (moov_buf.size() < 8) return regions;

    uint64_t raw = rdU32(moov_buf.data());
    size_t hdr = (raw == 1) ? 16 : 8;
    size_t moov_content_start = hdr;
    size_t moov_content_end = moov_buf.size();

    forEachChildBox(moov_buf, moov_content_start, moov_content_end, "trak",
                     [&](size_t trak_off, size_t trak_size) {
        uint64_t traw = rdU32(moov_buf.data() + trak_off);
        size_t thdr = (traw == 1) ? 16 : 8;
        size_t stbl_start = 0, stbl_end = 0;
        if (!descendBoxPath(moov_buf, trak_off + thdr, trak_off + trak_size,
                             {"mdia", "minf", "stbl"}, stbl_start, stbl_end)) {
            return;
        }

        TrackSampleTable table;
        if (!parseStbl(moov_buf, stbl_start, stbl_end, table))
            return;

        if (table.sync_samples.empty())
            return;

        auto offsets = computeSampleOffsets(table);
        if (offsets.empty()) return;

        for (uint32_t ss : table.sync_samples) {
            if (ss == 0 || ss > offsets.size()) continue;
            uint32_t idx = ss - 1;
            uint32_t sz = table.uniform_size != 0
                              ? table.uniform_size
                              : (idx < table.sample_sizes.size() ? table.sample_sizes[idx] : 0);
            if (sz == 0) continue;
            regions.emplace_back(offsets[idx], (int64_t)sz);
        }
    });

    normalizeRegions(regions);
    return regions;
}

std::vector<ByteRange> RecordFileEncryptor::collectStrongModeRegions(const std::string &plain_path,
                                               EncryptedFileTrailer &out_trailer) {
    std::vector<ByteRange> regions;
    int64_t moov_off = 0, moov_size = 0;
    if (!RecordEncryptionUtils::findMoovBox(plain_path, moov_off, moov_size))
        return regions;

    FILE *fp = fopen(plain_path.c_str(), "rb");
    if (!fp) return regions;
    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    fclose(fp);
    if (file_size <= 0) return regions;

    out_trailer.moov_start = moov_off;
    out_trailer.moov_end = moov_off + moov_size;
    out_trailer.mode = RecordEncryptionMode::kStrong;

    regions.emplace_back(0, file_size);
    return regions;
}

std::vector<ByteRange> RecordFileEncryptor::collectLightModeRegions(const std::string &plain_path,
                                              EncryptedFileTrailer &out_trailer) {
    std::vector<ByteRange> regions;
    int64_t moov_off = 0, moov_size = 0;
    if (!RecordEncryptionUtils::findMoovBox(plain_path, moov_off, moov_size))
        return regions;

    out_trailer.moov_start = moov_off;
    out_trailer.moov_end = moov_off + moov_size;
    out_trailer.mode = RecordEncryptionMode::kLight;

    FILE *fp = fopen(plain_path.c_str(), "rb");
    if (!fp) return regions;
    std::vector<uint8_t> moov_buf((size_t)moov_size);
    if (fseek(fp, moov_off, SEEK_SET) != 0 ||
        fread(moov_buf.data(), 1, (size_t)moov_size, fp) != (size_t)moov_size) {
        fclose(fp);
        return regions;
    }
    fclose(fp);

    // Header size comes from the box itself so a 64-bit largesize moov is not
    // corrupted by encrypting over its size field. The reader recomputes the
    // same value from the plaintext header, so both sides stay in sync.
    return buildLightModeRegions(moov_buf, moov_off, moov_off + moov_size);
}

bool RecordFileEncryptor::xcryptRegionsInPlace(const std::string &path,
                                                const std::vector<ByteRange> &regions_in,
                                                const std::vector<uint8_t> &enc_key,
                                                const std::vector<uint8_t> &base_iv) {
    if (regions_in.empty()) return true;

    std::vector<ByteRange> regions = regions_in;
    normalizeRegions(regions);

    OffsetKeyedCtrCipher cipher(enc_key, base_iv);

    FILE *fp = fopen(path.c_str(), "r+b");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);

    const int64_t kChunk = 1 << 20;
    std::vector<uint8_t> buf((size_t)kChunk);
    for (const auto &r : regions) {
        if (r.offset < 0 || r.end() > file_size) { fclose(fp); return false; }
        int64_t remaining = r.length;
        int64_t pos = r.offset;
        while (remaining > 0) {
            size_t n = (size_t)std::min<int64_t>(kChunk, remaining);
            if (fseek(fp, pos, SEEK_SET) != 0) { fclose(fp); return false; }
            if (fread(buf.data(), 1, n, fp) != n) { fclose(fp); return false; }
            auto out = cipher.xcryptAtOffset(buf.data(), n, pos);
            if (fseek(fp, pos, SEEK_SET) != 0) { fclose(fp); return false; }
            if (fwrite(out.data(), 1, n, fp) != n) { fclose(fp); return false; }
            pos += (int64_t)n;
            remaining -= (int64_t)n;
        }
    }
    fclose(fp);
    return true;
}

bool RecordFileEncryptor::encryptFileInPlace(const std::string &path,
                                              const std::vector<ByteRange> &regions_in,
                                              EncryptedFileTrailer &trailer,
                                              const std::vector<uint8_t> &enc_key,
                                              const std::vector<uint8_t> &hmac_key) {
    if (regions_in.empty()) return false;

    std::vector<ByteRange> regions = regions_in;
    normalizeRegions(regions);

    trailer.nonce = RecordEncryptionUtils::randomBytesVec(16);
    // Reserve the low 64 bits for the block counter so that advancing it while
    // seeking can never carry into the random half. 64 bits of nonce entropy is
    // far more than enough for one key scoped to a single storage pool.
    std::fill(trailer.nonce.begin() + 8, trailer.nonce.end(), (uint8_t)0);

    if (!xcryptRegionsInPlace(path, regions, enc_key, trailer.nonce)) return false;

    FILE *fp = fopen(path.c_str(), "r+b");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    const int64_t kChunk = 1 << 20;
    std::vector<uint8_t> buf((size_t)kChunk);

    uint8_t meta[EncryptedFileTrailer::kSize - 32];
    auto wrU64 = [](uint8_t *p, uint64_t v) {
        for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
    };
    auto wrU32 = [](uint8_t *p, uint32_t v) {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
    };
    std::memset(meta, 0, sizeof(meta));
    std::memcpy(meta, trailer.nonce.data(), 16);
    wrU64(meta + 16, (uint64_t)trailer.moov_start);
    wrU64(meta + 24, (uint64_t)trailer.moov_end);
    wrU32(meta + 32, EncryptedFileTrailer::kMagic);
    wrU32(meta + 36, EncryptedFileTrailer::kVersion);
    wrU32(meta + 40, (uint32_t)trailer.mode);

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    if (fwrite(meta, 1, sizeof(meta), fp) != sizeof(meta)) { fclose(fp); return false; }
    fflush(fp);

    int64_t hmac_len = file_size + (int64_t)sizeof(meta);
    HMAC_CTX *ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, hmac_key.data(), (int)hmac_key.size(), EVP_sha256(), nullptr);
    if (fseek(fp, 0, SEEK_SET) != 0) { HMAC_CTX_free(ctx); fclose(fp); return false; }
    int64_t remaining = hmac_len;
    while (remaining > 0) {
        size_t n = (size_t)std::min<int64_t>(kChunk, remaining);
        if (fread(buf.data(), 1, n, fp) != n) { HMAC_CTX_free(ctx); fclose(fp); return false; }
        HMAC_Update(ctx, buf.data(), n);
        remaining -= (int64_t)n;
    }
    unsigned char mac[32];
    unsigned int mac_len = 0;
    HMAC_Final(ctx, mac, &mac_len);
    HMAC_CTX_free(ctx);
    if (mac_len != 32) { fclose(fp); return false; }

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    bool ok = fwrite(mac, 1, 32, fp) == 32;
    if (ok) {
        // Without this the rename in reencryptOneFile() can land on disk before
        // the trailer does, leaving a file that fails its own HMAC check after
        // an unclean shutdown.
        ok = (fflush(fp) == 0) && (fsync(fileno(fp)) == 0);
    }
    fclose(fp);
    return ok;
}

bool RecordFileEncryptor::parseTrailer(FILE *fp,
                                        EncryptedFileTrailer &out_trailer,
                                        int64_t &out_content_len) {
    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    if (file_size < (int64_t)EncryptedFileTrailer::kSize) return false;

    int64_t meta_off = file_size - (int64_t)EncryptedFileTrailer::kSize;

    uint8_t meta[EncryptedFileTrailer::kSize - 32];
    if (fseek(fp, meta_off, SEEK_SET) != 0 ||
        fread(meta, 1, sizeof(meta), fp) != sizeof(meta)) return false;

    if (rdU32(meta + 32) != EncryptedFileTrailer::kMagic) return false;
    if (rdU32(meta + 36) != EncryptedFileTrailer::kVersion) return false;

    out_trailer.nonce.assign(meta, meta + 16);
    out_trailer.moov_start = (int64_t)rdU64(meta + 16);
    out_trailer.moov_end = (int64_t)rdU64(meta + 24);
    uint32_t raw_mode = rdU32(meta + 40);
    if (raw_mode == 0 || raw_mode >= (uint32_t)RecordEncryptionMode::kMax) {
        // A trailer exists, so the file is encrypted; a mode of none or an
        // unknown value means the trailer is not trustworthy.
        return false;
    }
    out_trailer.mode = (RecordEncryptionMode)raw_mode;
    out_content_len = meta_off;

    return out_trailer.moov_start >= 0 && out_trailer.moov_end <= out_content_len &&
           out_trailer.moov_start < out_trailer.moov_end;
}

bool RecordFileEncryptor::readAndVerifyTrailer(const std::string &path,
                                                const std::vector<uint8_t> &hmac_key,
                                                EncryptedFileTrailer &out_trailer,
                                                int64_t &out_content_len) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    if (!parseTrailer(fp, out_trailer, out_content_len)) { fclose(fp); return false; }

    fseek(fp, 0, SEEK_END);
    int64_t file_size = ftell(fp);
    const int64_t meta_size = (int64_t)EncryptedFileTrailer::kSize - 32;
    int64_t meta_off = file_size - (int64_t)EncryptedFileTrailer::kSize;
    int64_t hmac_off = file_size - 32;

    uint8_t meta[EncryptedFileTrailer::kSize - 32];
    if (fseek(fp, meta_off, SEEK_SET) != 0 ||
        fread(meta, 1, sizeof(meta), fp) != sizeof(meta)) { fclose(fp); return false; }

    unsigned char stored[32];
    if (fseek(fp, hmac_off, SEEK_SET) != 0 ||
        fread(stored, 1, 32, fp) != 32) { fclose(fp); return false; }

    HMAC_CTX *ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, hmac_key.data(), (int)hmac_key.size(), EVP_sha256(), nullptr);
    if (fseek(fp, 0, SEEK_SET) != 0) { HMAC_CTX_free(ctx); fclose(fp); return false; }
    std::vector<uint8_t> buf(1 << 16);
    int64_t remaining = meta_off + meta_size;
    while (remaining > 0) {
        size_t n = (size_t)std::min<int64_t>((int64_t)buf.size(), remaining);
        if (fread(buf.data(), 1, n, fp) != n) { HMAC_CTX_free(ctx); fclose(fp); return false; }
        HMAC_Update(ctx, buf.data(), n);
        remaining -= (int64_t)n;
    }
    unsigned char mac[32];
    unsigned int mac_len = 0;
    HMAC_Final(ctx, mac, &mac_len);
    HMAC_CTX_free(ctx);
    fclose(fp);

    if (mac_len != 32) return false;
    unsigned char diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac[i] ^ stored[i];
    return diff == 0;
}

}//namespace managerkit
