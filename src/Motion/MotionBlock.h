#ifndef MOTION_MOTIONFILE_H
#define MOTION_MOTIONFILE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "MotionBitmap.h"
#include "Util/BlockInterface.h"
#include "Util/BlockStorageEngine.h"

namespace mediakit {

static constexpr uint32_t kMotionMagic   = 0x4D4F544E; // "MOTN"
static constexpr uint16_t kMotionVersion = 1;

enum class MotionBlockType : uint16_t {
    Summary = 0x01,
    Event   = 0x02,
    // future types can be added here
};

// ── On-disk packed extension headers ─────────────────────────────────────────
//
// Motion block on-disk layout:
//   [toolkit::BlockHeader][ExtHeader][bitmap...]
//
//   BlockHeader.magic        = kMotionMagic
//   BlockHeader.type         = MotionBlockType::Event or ::Summary
//   BlockHeader.header_size  = sizeof(BlockHeader) + sizeof(ExtHeader)
//   BlockHeader.payload_size = bitmap_bytes  (pure payload, ext header excluded)
//   BlockHeader.stamp        = block start timestamp (ms)
//   BlockHeader.crc          = CRC32 of (ExtHeader + bitmap)
//
// Total on-disk block size = header_size + payload_size.
// The reader receives ext_header and payload as separate buffers from readBlock():
//   ext_header = (header.header_size - sizeof(BlockHeader)) bytes
//   payload    = header.payload_size bytes (bitmap only)

#pragma pack(push, 1)

/**
 * Extension header for Event blocks.
 * Immediately follows toolkit::BlockHeader on disk.
 */
struct MotionEventExtHeader {
    uint16_t rows;
    uint16_t cols;
    uint16_t active_cells;
    uint16_t reserved;
};

/**
 * Extension header for Summary blocks.
 * BlockHeader.stamp = start_stamp; end_stamp is stored here.
 */
struct MotionSummaryExtHeader {
    uint64_t end_stamp;
    uint16_t rows;
    uint16_t cols;
    uint16_t active_cells;
    uint16_t reserved;
};

#pragma pack(pop)

static_assert(sizeof(MotionEventExtHeader)   ==  8, "MotionEventExtHeader layout changed");
static_assert(sizeof(MotionSummaryExtHeader) == 16, "MotionSummaryExtHeader layout changed");

// ── CRC32 helper ──────────────────────────────────────────────────────────────
// Standard CRC32/ISO-HDLC over concatenated (ext_header + bitmap) bytes.
// Used to populate BlockHeader.crc on write and verify integrity on read.
static inline uint32_t motionCrc32(const uint8_t *ext,  size_t ext_len,
                                    const uint8_t *data, size_t data_len) {
    auto step = [](uint32_t crc, const uint8_t *p, size_t n) -> uint32_t {
        for (size_t i = 0; i < n; ++i) {
            crc ^= p[i];
            for (int b = 0; b < 8; ++b)
                crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
        return crc;
    };
    uint32_t crc = 0xFFFFFFFFu;
    if (ext  && ext_len)  crc = step(crc, ext,  ext_len);
    if (data && data_len) crc = step(crc, data, data_len);
    return ~crc;
}

/**
 * Motion-specific index entry.
 * Extends the base (stamp + offset) with the block type so that readers
 * can distinguish Event and Summary blocks directly from the index without
 * seeking into the block file.
 * Layout: 8 + 8 + 2 + 6 = 24 bytes, naturally 8-byte aligned.
 */
struct MotionIndexEntry {
    uint64_t stamp;
    uint64_t offset;
    uint16_t type;        // MotionBlockType value
    uint16_t reserved[3]; // pad to 24 bytes
};
static_assert(sizeof(MotionIndexEntry) == 24, "MotionIndexEntry layout changed");

// ── In-memory block classes implementing toolkit::BlockInterface ──────────────

/**
 * In-memory Motion Event block.
 *
 * headerSize()  = sizeof(BlockHeader) + sizeof(MotionEventExtHeader)
 * payloadSize() = bitmap bytes only
 *
 * serialize() produces the full binary blob [BlockHeader][ExtHeader][bitmap]
 * for convenience (e.g. testing); MotionEventWriter uses the split API instead.
 */
class MotionEventBlock : public toolkit::BlockInterface {
public:
    using Ptr = std::shared_ptr<MotionEventBlock>;

    MotionEventBlock() = default;
    MotionEventBlock(uint64_t stamp, const MotionEventExtHeader &ext,
                     std::vector<uint8_t> bitmap, uint32_t crc = 0)
        : _stamp(stamp), _ext(ext), _bitmap(std::move(bitmap)), _crc(crc) {}

    uint32_t magic()      const override { return kMotionMagic; }
    uint16_t type()       const override { return static_cast<uint16_t>(MotionBlockType::Event); }
    uint64_t stamp()      const override { return _stamp; }
    uint32_t headerSize() const override {
        return static_cast<uint32_t>(sizeof(toolkit::BlockHeader) + sizeof(MotionEventExtHeader));
    }
    // bitmap bytes only — ext header is part of the header region
    uint32_t payloadSize() const override {
        return static_cast<uint32_t>(_bitmap.size());
    }
    uint32_t crc() const override {
        return motionCrc32(reinterpret_cast<const uint8_t *>(&_ext), sizeof(_ext),
                           _bitmap.data(), _bitmap.size());
    }

    void serialize(toolkit::BlockBuffer &buf) const override {
        auto hdr = buildBaseHeader();
        buf.append(&hdr,           sizeof(hdr));
        buf.append(&_ext,          sizeof(_ext));
        buf.append(_bitmap.data(), _bitmap.size());
    }

    const MotionEventExtHeader &extHeader() const { return _ext; }
    const std::vector<uint8_t> &bitmap()    const { return _bitmap; }

private:
    uint64_t             _stamp = 0;
    MotionEventExtHeader _ext{};
    std::vector<uint8_t> _bitmap;
    uint32_t             _crc   = 0;
};

/**
 * In-memory Motion Summary block.
 *
 * headerSize()  = sizeof(BlockHeader) + sizeof(MotionSummaryExtHeader)
 * payloadSize() = bitmap bytes only
 */
class MotionSummaryBlock : public toolkit::BlockInterface {
public:
    using Ptr = std::shared_ptr<MotionSummaryBlock>;

    MotionSummaryBlock() = default;
    MotionSummaryBlock(uint64_t start_stamp, const MotionSummaryExtHeader &ext,
                       std::vector<uint8_t> bitmap, uint32_t crc = 0)
        : _stamp(start_stamp), _ext(ext), _bitmap(std::move(bitmap)), _crc(crc) {}

    uint32_t magic()      const override { return kMotionMagic; }
    uint16_t type()       const override { return static_cast<uint16_t>(MotionBlockType::Summary); }
    uint64_t stamp()      const override { return _stamp; }
    uint32_t headerSize() const override {
        return static_cast<uint32_t>(sizeof(toolkit::BlockHeader) + sizeof(MotionSummaryExtHeader));
    }
    // bitmap bytes only — ext header is part of the header region
    uint32_t payloadSize() const override {
        return static_cast<uint32_t>(_bitmap.size());
    }
    uint32_t crc() const override {
        return motionCrc32(reinterpret_cast<const uint8_t *>(&_ext), sizeof(_ext),
                           _bitmap.data(), _bitmap.size());
    }

    void serialize(toolkit::BlockBuffer &buf) const override {
        auto hdr = buildBaseHeader();
        buf.append(&hdr,           sizeof(hdr));
        buf.append(&_ext,          sizeof(_ext));
        buf.append(_bitmap.data(), _bitmap.size());
    }

    const MotionSummaryExtHeader &extHeader() const { return _ext; }
    const std::vector<uint8_t>   &bitmap()    const { return _bitmap; }

private:
    uint64_t               _stamp = 0;
    MotionSummaryExtHeader _ext{};
    std::vector<uint8_t>   _bitmap;
    uint32_t               _crc   = 0;
};

/**
 * Raw in-memory block produced by BlockReaderInterface::readBlock().
 * ext_header and payload are already separated by the reader.
 */
struct MotionBlock {
    toolkit::BlockHeader header;
    std::vector<uint8_t> ext_header; // (header.header_size - sizeof(BlockHeader)) bytes
    std::vector<uint8_t> payload;    // bitmap bytes only
};

} // namespace mediakit

#endif // MOTION_MOTIONFILE_H
