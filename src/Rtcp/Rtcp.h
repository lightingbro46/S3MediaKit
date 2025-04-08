#ifndef S3MEDIAKIT_RTCP_H
#define S3MEDIAKIT_RTCP_H

#include "Common/macros.h"
#include "Network/Buffer.h"
#include "Util/util.h"
#include <stdint.h>
#include <vector>

namespace mediakit {

#pragma pack(push, 1)

// http://www.networksorcery.com/enp/protocol/rtcp.htm
#define RTCP_PT_MAP(XX)                                                                                                \
    XX(RTCP_FIR, 192)                                                                                                  \
    XX(RTCP_NACK, 193)                                                                                                 \
    XX(RTCP_SMPTETC, 194)                                                                                              \
    XX(RTCP_IJ, 195)                                                                                                   \
    XX(RTCP_SR, 200)                                                                                                   \
    XX(RTCP_RR, 201)                                                                                                   \
    XX(RTCP_SDES, 202)                                                                                                 \
    XX(RTCP_BYE, 203)                                                                                                  \
    XX(RTCP_APP, 204)                                                                                                  \
    XX(RTCP_RTPFB, 205)                                                                                                \
    XX(RTCP_PSFB, 206)                                                                                                 \
    XX(RTCP_XR, 207)                                                                                                   \
    XX(RTCP_AVB, 208)                                                                                                  \
    XX(RTCP_RSI, 209)                                                                                                  \
    XX(RTCP_TOKEN, 210)

// https://tools.ietf.org/html/rfc3550#section-6.5
#define SDES_TYPE_MAP(XX)                                                                                              \
    XX(RTCP_SDES_END, 0)                                                                                               \
    XX(RTCP_SDES_CNAME, 1)                                                                                             \
    XX(RTCP_SDES_NAME, 2)                                                                                              \
    XX(RTCP_SDES_EMAIL, 3)                                                                                             \
    XX(RTCP_SDES_PHONE, 4)                                                                                             \
    XX(RTCP_SDES_LOC, 5)                                                                                               \
    XX(RTCP_SDES_TOOL, 6)                                                                                              \
    XX(RTCP_SDES_NOTE, 7)                                                                                              \
    XX(RTCP_SDES_PRIVATE, 8)

// https://datatracker.ietf.org/doc/rfc4585/?include_text=1
// 6.3.  Payload-Specific Feedback Messages
//
//    Payload-Specific FB messages are identified by the value PT=PSFB as
//    RTCP message type.
//
//    Three payload-specific FB messages are defined so far plus an
//    application layer FB message.  They are identified by means of the
//    FMT parameter as follows:
//
//       0:     unassigned
//       1:     Picture Loss Indication (PLI)
//       2:     Slice Loss Indication (SLI)
//       3:     Reference Picture Selection Indication (RPSI)
//       4:     FIR https://tools.ietf.org/html/rfc5104#section-4.3.1.1
//       5:     TSTR https://tools.ietf.org/html/rfc5104#section-4.3.2.1
//       6:     TSTN https://tools.ietf.org/html/rfc5104#section-4.3.2.1
//       7:     VBCM https://tools.ietf.org/html/rfc5104#section-4.3.4.1
//       8-14:  unassigned
//       15:    REMB / Application layer FB (AFB) message, https://tools.ietf.org/html/draft-alvestrand-rmcat-remb-03
//       16-30: unassigned
//       31:    reserved for future expansion of the sequence number space
#define PSFB_TYPE_MAP(XX)                                                                                              \
    XX(RTCP_PSFB_PLI, 1)                                                                                               \
    XX(RTCP_PSFB_SLI, 2)                                                                                               \
    XX(RTCP_PSFB_RPSI, 3)                                                                                              \
    XX(RTCP_PSFB_FIR, 4)                                                                                               \
    XX(RTCP_PSFB_TSTR, 5)                                                                                              \
    XX(RTCP_PSFB_TSTN, 6)                                                                                              \
    XX(RTCP_PSFB_VBCM, 7)                                                                                              \
    XX(RTCP_PSFB_REMB, 15)

// https://tools.ietf.org/html/rfc4585#section-6.2
// 6.2.   Transport Layer Feedback Messages
//
//    Transport layer FB messages are identified by the value RTPFB as RTCP
//    message type.
//
//    A single general purpose transport layer FB message is defined in
//    this document: Generic NACK.  It is identified by means of the FMT
//    parameter as follows:
//
//    0:     unassigned
//    1:     Generic NACK
//    2:     reserved https://tools.ietf.org/html/rfc5104#section-4.2
//    3:     TMMBR https://tools.ietf.org/html/rfc5104#section-4.2.1.1
//    4:     TMMBN https://tools.ietf.org/html/rfc5104#section-4.2.2.1
//    5-14:  unassigned
//    15     transport-cc https://tools.ietf.org/html/draft-holmer-rmcat-transport-wide-cc-extensions-01
//    16-30: unassigned
//    31:    reserved for future expansion of the identifier number space
#define RTPFB_TYPE_MAP(XX)                                                                                             \
    XX(RTCP_RTPFB_NACK, 1)                                                                                             \
    XX(RTCP_RTPFB_TMMBR, 3)                                                                                            \
    XX(RTCP_RTPFB_TMMBN, 4)                                                                                            \
    XX(RTCP_RTPFB_TWCC, 15)

// rtcp type enum
enum class RtcpType : uint8_t {
#define XX(key, value) key = value,
    RTCP_PT_MAP(XX)
#undef XX
};

// sdes type enum
enum class SdesType : uint8_t {
#define XX(key, value) key = value,
    SDES_TYPE_MAP(XX)
#undef XX
};

// psfb type enum
enum class PSFBType : uint8_t {
#define XX(key, value) key = value,
    PSFB_TYPE_MAP(XX)
#undef XX
};

// rtpfb type enum
enum class RTPFBType : uint8_t {
#define XX(key, value) key = value,
    RTPFB_TYPE_MAP(XX)
#undef XX
};

/**
 * RtcpType to description string
 */
const char *rtcpTypeToStr(RtcpType type);

/**
 * SdesType enumeration to describe string
 */
const char *sdesTypeToStr(SdesType type);

/**
 * psfb enumeration to describe string
 */
const char *psfbTypeToStr(PSFBType type);

/**
 * rtpfb enumeration to describe string
 */
const char *rtpfbTypeToStr(RTPFBType type);

class RtcpHeader {
public:
#if __BYTE_ORDER == __BIG_ENDIAN
    // Version number, fixed to 2
    uint32_t version : 2;
    // padding, fixed to 0
    uint32_t padding : 1;
    // reception report count
    uint32_t report_count : 5;
#else
    // reception report count
    uint32_t report_count : 5;
    // padding, is there any additional padding at the end
    uint32_t padding : 1;
    // Version number, fixed to 2
    uint32_t version : 2;
#endif
    // rtcp type, RtcpType
    uint32_t pt : 8;

private:
    // length
    uint32_t length : 16;

public:
    /**
     * Parses rtcp and converts network byte order to host byte order, and returns the RtcpHeader derived class list
     * @param data Data pointer
     * @param size Total data length
     * @return rtcp object list, no need for free
     */
    static std::vector<RtcpHeader *> loadFromBytes(char *data, size_t size);

    /**
     * rtcp package to Buffer object
     * @param rtcp rtcp package object smart pointer
     * @return Buffer object
     */
    static toolkit::Buffer::Ptr toBuffer(std::shared_ptr<RtcpHeader> rtcp);

    /**
     * Print rtcp related fields details (call dumpString function of derived class)
     * What type of derived class is internally determined
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Get the total length of rtcp based on the length field
     */
    size_t getSize() const;

    /**
     * Add padding data length later
     */
    size_t getPaddingSize() const;

    /**
     * Set the rtcp length field
     * @param size rtcp total length, unit bytes
     */
    void setSize(size_t size);

protected:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpHeader() const;

private:
    /**
     * Call the net2Host function of the derived class
     * @param size rtcp character length
     */
    void net2Host(size_t size);

};

/////////////////////////////////////////////////////////////////////////////

// ReportBlock
class ReportItem {
public:
    friend class RtcpSR;
    friend class RtcpRR;

    uint32_t ssrc;
    // Fraction lost
    uint32_t fraction : 8;
    // Cumulative number of packets lost
    uint32_t cumulative : 24;
    // Sequence number cycles count
    uint16_t seq_cycles;
    // Highest sequence number received
    uint16_t seq_max;
    // Interarrival jitter
    uint32_t jitter;
    // Last SR timestamp, NTP timestamp,(ntpmsw & 0xFFFF) << 16  | (ntplsw >> 16) & 0xFFFF)
    uint32_t last_sr_stamp;
    // Delay since last SR timestamp,expressed in units of 1/65536 seconds
    uint32_t delay_since_last_sr;

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     */
    void net2Host();
};

/*
 * 6.4.1 SR: Sender Report RTCP Packet

        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    RC   |   PT=SR=200   |             length            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         SSRC of sender                        |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
sender |              NTP timestamp, most significant word             |
info   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |             NTP timestamp, least significant word             |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         RTP timestamp                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     sender's packet count                     |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      sender's octet count                     |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_1 (SSRC of first source)                 |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  1    | fraction lost |       cumulative number of packets lost       |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |           extended highest sequence number received           |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      interarrival jitter                      |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         last SR (LSR)                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                   delay since last SR (DLSR)                  |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_2 (SSRC of second source)                |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  2    :                               ...                             :
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
       |                  profile-specific extensions                  |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
// sender report
class RtcpSR : public RtcpHeader {
public:
    friend class RtcpHeader;
    uint32_t ssrc;
    // ntp timestamp MSW(in second)
    uint32_t ntpmsw;
    // ntp timestamp LSW(in picosecond)
    uint32_t ntplsw;
    // rtp timestamp
    uint32_t rtpts;
    // sender packet count
    uint32_t packet_count;
    // sender octet count
    uint32_t octet_count;
    // There may be many
    ReportItem items;

public:
    /**
     * Create an SR package, only the RtcpHeader part (network endianness) is assigned
     * @param item_count Number of ReportItem objects
     * @return SR package
     */
    static std::shared_ptr<RtcpSR> create(size_t item_count);

    /**
     * Set ntpmsw and ntplsw fields to network byte order
     * @param tv time
     */
    void setNtpStamp(struct timeval tv);
    void setNtpStamp(uint64_t unix_stamp_ms);

    /**
     * Returns the string of ntp time
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string getNtpStamp() const;
    uint64_t getNtpUnixStampMS() const;

    /**
     * Get the ReportItem object pointer list
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::vector<ReportItem *> getItemList();

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);
};

/////////////////////////////////////////////////////////////////////////////

/*
 * 6.4.2 RR: Receiver Report RTCP Packet

        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    RC   |   PT=RR=201   |             length            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     SSRC of packet sender                     |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_1 (SSRC of first source)                 |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  1    | fraction lost |       cumulative number of packets lost       |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |           extended highest sequence number received           |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      interarrival jitter                      |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         last SR (LSR)                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                   delay since last SR (DLSR)                  |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_2 (SSRC of second source)                |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  2    :                               ...                             :
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
       |                  profile-specific extensions                  |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

// Receiver Report
class RtcpRR : public RtcpHeader {
public:
    friend class RtcpHeader;

    uint32_t ssrc;
    // There may be many
    ReportItem items;

public:
    /**
     * Create an RR package, only the RtcpHeader part is assigned
     * @param item_count Number of ReportItem objects
     * @return RR package
     */
    static std::shared_ptr<RtcpRR> create(size_t item_count);

    /**
     * Get the ReportItem object pointer list
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::vector<ReportItem *> getItemList();

private:
    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);

    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

};

/////////////////////////////////////////////////////////////////////////////

/*
 *      6.5 SDES: Source Description RTCP Packet
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    SC   |  PT=SDES=202  |             length            |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
chunk  |                          SSRC/CSRC_1                          |
  1    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                           SDES items                          |
       |                              ...                              |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
chunk  |                          SSRC/CSRC_2                          |
  2    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                           SDES items                          |
       |                              ...                              |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
 */

/*

SDES items definition
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   SdesType  |     length    | user and domain name        ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

// Source description Chunk
class SdesChunk {
public:
    friend class RtcpSdes;

    uint32_t ssrc;
    // SdesType
    uint8_t type;
    // Text length strands, can be 0
    uint8_t txt_len;
    // Undecidedly long
    char text[1];
    // Finally ends with RTCP_SDES_END
    // Only fields are placeholder fields, not representing the real location
    uint8_t end;

public:
    /**
     * Return to change the object byte length
     */
    size_t totalBytes() const;

    /**
     * The minimum length of this object
     */
    static size_t minSize();

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     */
    void net2Host();
};

// Source description
class RtcpSdes : public RtcpHeader {
public:
    friend class RtcpHeader;

    // There may be many
    SdesChunk chunks;

public:
    /**
     * Create SDES packages, assign only the length and text parts of the RtcpHeader and SdesChunk object.
     * @param item_text SdesChunk list, only assign length and text parts
     * @return SDES package
     */
    static std::shared_ptr<RtcpSdes> create(const std::vector<std::string> &item_text);

    /**
     * Get the list of SdesChunk object pointers
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::vector<SdesChunk *> getChunkList();

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);
};

// https://tools.ietf.org/html/rfc4585#section-6.1
// 6.1.   Common Packet Format for Feedback Messages
//
//   All FB messages MUST use a common packet format that is depicted in
//   Figure 3:
//
//    0                   1                   2                   3
//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |V=2|P|   FMT   |       PT      |          length               |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |                  SSRC of packet sender                        |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |                  SSRC of media source                         |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   :            Feedback Control Information (FCI)                 :
//   :                                                               :
// The data structures of rtcpfb and psfb are consistent
class RtcpFB : public RtcpHeader {
public:
    friend class RtcpHeader;
    uint32_t ssrc;
    uint32_t ssrc_media;

public:
    /**
     * Create a feedback package of psfb type
     */
    static std::shared_ptr<RtcpFB> create(PSFBType fmt, const void *fci = nullptr, size_t fci_len = 0);

    /**
     * Create a feedback package of rtpfb type
     */
    static std::shared_ptr<RtcpFB> create(RTPFBType fmt, const void *fci = nullptr, size_t fci_len = 0);

    /**
     * Convert fci into a pointer for an object
     * @tparam Type object type
     * @return Object pointer
     */
    template <typename Type>
    const Type &getFci() const {
        auto fci_data = getFciPtr();
        auto fci_len = getFciSize();
        Type *fci = (Type *)fci_data;
        fci->check(fci_len);
        return *fci;
    }

    /**
     * Get fci pointer
     */
    const void *getFciPtr() const;

    /**
     * Get the FCI data length
     */
    size_t getFciSize() const;

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);

private:
    static std::shared_ptr<RtcpFB> create_l(RtcpType type, int fmt, const void *fci, size_t fci_len);
};

// BYE
/*
       0                   1                   2                   3
       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |V=2|P|    SC   |   PT=BYE=203  |             length            |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      |                           SSRC/CSRC                           |
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      :                              ...                              :
      +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
(opt) |     length    |               reason for leaving            ...
      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

class RtcpBye : public RtcpHeader {
public:
    friend class RtcpHeader;
    /* To lengthen, determine how many ssrcs are based on the count */
    uint32_t ssrc[1];

    /** There may be several in the middle ssrc **/

    /* Optional */
    uint8_t reason_len;
    char reason[1];

public:
    /**
     * Create a bye package
     * @param ssrc Ssrc List
     * @param reason reason
     * @return rtcp bye package
     */
    static std::shared_ptr<RtcpBye> create(const std::vector<uint32_t> &ssrc, const std::string &reason);

    /**
     * Get the ssrc list
     */
    std::vector<uint32_t *> getSSRC();

    /**
     * Get the reason
     */
    std::string getReason() const;

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);
};

/*
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|reserved |   PT=XR=207   |             length            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                              SSRC                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
:                         report blocks                         :
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

*/
/*

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |     BT=4      |   reserved    |       block length = 2        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |              NTP timestamp, most significant word             |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |             NTP timestamp, least significant word             |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

*/
class RtcpXRRRTR : public RtcpHeader {
public:
    friend class RtcpHeader;
    uint32_t ssrc;
    // 4
    uint8_t bt;
    uint8_t reserved;
    // 2
    uint16_t block_length;
    // ntp timestamp MSW(in second)
    uint32_t ntpmsw;
    // ntp timestamp LSW(in picosecond)
    uint32_t ntplsw;

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);

};

/*

  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |     BT=5      |   reserved    |         block length          |
 +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
 |                 SSRC_1 (SSRC of first receiver)               | sub-
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ block
 |                         last RR (LRR)                         |   1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                   delay since last RR (DLRR)                  |
 +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
 |                 SSRC_2 (SSRC of second receiver)              | sub-
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ block
 :                               ...                             :   2
 +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
*/
class RtcpXRDLRRReportItem {
public:
    friend class RtcpXRDLRR;
    uint32_t ssrc;
    uint32_t lrr;
    uint32_t dlrr;

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     */
    void net2Host();
};

class RtcpXRDLRR : public RtcpHeader {
public:
    friend class RtcpHeader;
    uint32_t ssrc;
    uint8_t bt;
    uint8_t reserved;
    uint16_t block_length;
    RtcpXRDLRRReportItem items;

    /**
     * Create the RtcpXRDLRR package, and only assign the RtcpHeader part(Network byte order)
     * @param item_count RtcpXRDLRRReportItem object number
     * @return RtcpXRDLRR package
     */
    static std::shared_ptr<RtcpXRDLRR> create(size_t item_count);

    /**
     * Get the list of RtcpXRDLRRReportItem object pointers
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::vector<RtcpXRDLRRReportItem *> getItemList();

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);

};

//  RFC 4585: Feedback format.
//
//  Common packet format:
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |     BT=42     |   reserved    |         block length          |
//  +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//
//  Target bitrate item (repeat as many times as necessary).
//
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |   S   |   T   |                Target Bitrate                 |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  :  ...                                                          :
//
//  Spatial Layer (S): 4 bits
//    Indicates which temporal layer this bitrate concerns.
//
//  Temporal Layer (T): 4 bits
//    Indicates which temporal layer this bitrate concerns.
//
//  Target Bitrate: 24 bits
//    The encoder target bitrate for this layer, in kbps.
//
//  As an example of how S and T are intended to be used, VP8 simulcast will
//  use a separate TargetBitrate message per stream, since they are transmitted
//  on separate SSRCs, with temporal layers grouped by stream.
//  If VP9 SVC is used, there will be only one SSRC, so each spatial and
//  temporal layer combo used shall be specified in the TargetBitrate packet.
class RtcpXRTargetBitrateItem {
public:
    friend class RtcpXRTargetBitrate;
#if __BYTE_ORDER == __BIG_ENDIAN
    // Indicates which temporal layer this bitrate concerns.
    uint32_t spatial_layer : 4;
    // Indicates which temporal layer this bitrate concerns.
    uint32_t temporal_layer : 4;
#else
    // Indicates which temporal layer this bitrate concerns.
    uint32_t temporal_layer : 4;
    // Indicates which temporal layer this bitrate concerns.
    uint32_t spatial_layer : 4;
#endif
    //The encoder target bitrate for this layer, in kbps.
    uint32_t target_bitrate : 24;

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     */
    void net2Host();
};


class RtcpXRTargetBitrate : public RtcpHeader {
public:
    friend class RtcpHeader;
    uint32_t ssrc;
    uint8_t bt;
    uint8_t reserved;
    uint16_t block_length;
    RtcpXRTargetBitrateItem items;

    /**
     * Create the RtcpXRTargetBitrate package, and only assign the RtcpHeader part(Network byte order)
     * @param item_count RtcpXRTargetBitrateItem object number
     * @return RtcpXRTargetBitrate package
     */
    static std::shared_ptr<RtcpXRTargetBitrate> create(size_t item_count);

    /**
     * Get the list of pointers for RtcpXRTargetBitrateItem object
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::vector<RtcpXRTargetBitrateItem *> getItemList();

private:
    /**
     * Print field details
     * This function can only be used after converting it to host endianness using net2Host
     */
    std::string dumpString() const;

    /**
     * Convert network byte order to host byte order
     * @param size Byte length to prevent memory from crossing boundaries
     */
    void net2Host(size_t size);

};

#pragma pack(pop)

} // namespace mediakit
#endif // S3MEDIAKIT_RTCP_H
