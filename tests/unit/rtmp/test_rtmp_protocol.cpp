#include <gtest/gtest.h>

#include "Rtmp/RtmpProtocol.h"

using namespace mediakit;

namespace {

class CapturingRtmpProtocol : public RtmpProtocol {
public:
    std::vector<toolkit::Buffer::Ptr> sent;
    std::vector<RtmpPacket::Ptr> received;
    std::vector<uint32_t> stream_begin;
    std::vector<uint32_t> stream_eof;
    std::vector<uint32_t> stream_dry;

    void sendAck(uint32_t value) { sendAcknowledgement(value); }
    void sendWindow(uint32_t value) { sendAcknowledgementSize(value); }
    void sendBandwidth(uint32_t value) { sendPeerBandwidth(value); }
    void setChunkSize(uint32_t value) { sendChunkSize(value); }
    void pingRequest(uint32_t value) { sendPingRequest(value); }
    void pingResponse(uint32_t value) { sendPingResponse(value); }
    void setBuffer(uint32_t stream, uint32_t length) { sendSetBufferLength(stream, length); }
    void sendMessage(uint8_t type, uint32_t stream, const std::string &body,
                     uint32_t stamp, int chunk) {
        sendRtmp(type, stream, body, stamp, chunk);
    }
    void resetProtocol() { reset(); }
    toolkit::BufferRaw::Ptr buffer(const void *data, size_t size) {
        return obtainBuffer(data, size);
    }

protected:
    void onSendRawData(toolkit::Buffer::Ptr value) override {
        sent.emplace_back(std::move(value));
    }
    void onRtmpChunk(RtmpPacket::Ptr packet) override {
        received.emplace_back(std::move(packet));
    }
    void onStreamBegin(uint32_t stream) override {
        RtmpProtocol::onStreamBegin(stream);
        stream_begin.emplace_back(stream);
    }
    void onStreamEof(uint32_t stream) override { stream_eof.emplace_back(stream); }
    void onStreamDry(uint32_t stream) override { stream_dry.emplace_back(stream); }
};

std::string join(const std::vector<toolkit::Buffer::Ptr> &buffers) {
    std::string out;
    for (const auto &buffer : buffers) {
        out.append(buffer->data(), buffer->size());
    }
    return out;
}

void feed(RtmpProtocol &protocol, const std::string &bytes) {
    std::vector<char> mutable_bytes(bytes.begin(), bytes.end());
    mutable_bytes.push_back('\0');
    protocol.onParseRtmp(mutable_bytes.data(), bytes.size());
}

std::string chunk(uint8_t type, const std::string &body, uint32_t timestamp = 0,
                  uint32_t stream = 0, uint8_t chunk_id = CHUNK_NETWORK) {
    std::string out;
    out.push_back(static_cast<char>(chunk_id));
    out.push_back(static_cast<char>((timestamp >> 16) & 0xFF));
    out.push_back(static_cast<char>((timestamp >> 8) & 0xFF));
    out.push_back(static_cast<char>(timestamp & 0xFF));
    out.push_back(static_cast<char>((body.size() >> 16) & 0xFF));
    out.push_back(static_cast<char>((body.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(body.size() & 0xFF));
    out.push_back(static_cast<char>(type));
    out.push_back(static_cast<char>(stream & 0xFF));
    out.push_back(static_cast<char>((stream >> 8) & 0xFF));
    out.push_back(static_cast<char>((stream >> 16) & 0xFF));
    out.push_back(static_cast<char>((stream >> 24) & 0xFF));
    out += body;
    return out;
}

std::string be16(uint16_t value) {
    std::string out;
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
    return out;
}

std::string be32(uint32_t value) {
    std::string out;
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
    return out;
}

void completeServerHandshake(CapturingRtmpProtocol &protocol) {
    std::string c0c1(1 + sizeof(RtmpHandshake), '\0');
    c0c1[0] = HANDSHAKE_PLAINTEXT;
    feed(protocol, c0c1);
    feed(protocol, std::string(sizeof(RtmpHandshake), '\0'));
    protocol.sent.clear();
}

} // namespace

TEST(RtmpProtocolTest, SerializesControlMessages) {
    CapturingRtmpProtocol protocol;
    protocol.sendAck(0x01020304);
    protocol.sendWindow(5000000);
    protocol.sendBandwidth(2500000);
    protocol.pingRequest(123);
    protocol.pingResponse(456);
    protocol.setBuffer(7, 3000);

    ASSERT_GE(protocol.sent.size(), 12U);
    auto first_header = reinterpret_cast<const RtmpHeader *>(protocol.sent[0]->data());
    EXPECT_EQ(MSG_ACK, first_header->type_id);
    EXPECT_EQ(CHUNK_NETWORK, first_header->chunk_id);
    EXPECT_EQ(4U, protocol.sent[1]->size());
    EXPECT_EQ(std::string("\x01\x02\x03\x04", 4),
              std::string(protocol.sent[1]->data(), protocol.sent[1]->size()));
}

TEST(RtmpProtocolTest, SplitsMessagesAtConfiguredChunkBoundary) {
    CapturingRtmpProtocol protocol;
    protocol.setChunkSize(3);
    protocol.sent.clear();
    protocol.sendMessage(MSG_VIDEO, 1, "abcdefgh", 10, CHUNK_VIDEO);

    ASSERT_EQ(6U, protocol.sent.size());
    EXPECT_EQ(sizeof(RtmpHeader), protocol.sent[0]->size());
    EXPECT_EQ(3U, protocol.sent[1]->size());
    EXPECT_EQ(1U, protocol.sent[2]->size());
    EXPECT_EQ(3U, protocol.sent[3]->size());
    EXPECT_EQ(1U, protocol.sent[4]->size());
    EXPECT_EQ(2U, protocol.sent[5]->size());
    EXPECT_THROW(protocol.sendMessage(MSG_VIDEO, 1, "x", 0, 1), std::runtime_error);
    EXPECT_THROW(protocol.sendMessage(MSG_VIDEO, 1, "x", 0, 64), std::runtime_error);
}

TEST(RtmpProtocolTest, WritesExtendedTimestamps) {
    CapturingRtmpProtocol protocol;
    protocol.sendMessage(MSG_AUDIO, 2, "audio", 0x01020304, CHUNK_AUDIO);
    ASSERT_EQ(3U, protocol.sent.size());
    auto header = reinterpret_cast<const RtmpHeader *>(protocol.sent[0]->data());
    EXPECT_EQ(0xFF, header->time_stamp[0]);
    EXPECT_EQ(4U, protocol.sent[1]->size());
    EXPECT_EQ(std::string("\x01\x02\x03\x04", 4),
              std::string(protocol.sent[1]->data(), protocol.sent[1]->size()));
}

TEST(RtmpProtocolTest, StartsAndCompletesSimpleClientHandshake) {
    CapturingRtmpProtocol protocol;
    bool complete = false;
    protocol.startClientSession([&]() { complete = true; }, false);
    ASSERT_EQ(2U, protocol.sent.size());
    EXPECT_EQ(1U, protocol.sent[0]->size());
    EXPECT_EQ(HANDSHAKE_PLAINTEXT, static_cast<uint8_t>(protocol.sent[0]->data()[0]));
    EXPECT_EQ(sizeof(RtmpHandshake), protocol.sent[1]->size());

    std::string response(1 + 2 * sizeof(RtmpHandshake), '\0');
    response[0] = HANDSHAKE_PLAINTEXT;
    feed(protocol, response.substr(0, 100));
    EXPECT_FALSE(complete);
    feed(protocol, response.substr(100));
    EXPECT_TRUE(complete);
    ASSERT_EQ(3U, protocol.sent.size());
    EXPECT_EQ(sizeof(RtmpHandshake), protocol.sent.back()->size());
}

TEST(RtmpProtocolTest, RejectsUnsupportedHandshakeVersionAndResets) {
    CapturingRtmpProtocol protocol;
    protocol.startClientSession([]() {}, false);
    std::string response(1 + 2 * sizeof(RtmpHandshake), '\0');
    response[0] = 2;
    EXPECT_THROW(feed(protocol, response), std::runtime_error);
    protocol.resetProtocol();

    const char payload[] = "pool";
    auto buffer = protocol.buffer(payload, 4);
    ASSERT_TRUE(buffer);
    EXPECT_EQ(4U, buffer->size());
    EXPECT_EQ("pool", std::string(buffer->data(), buffer->size()));
}

TEST(RtmpProtocolTest, CompletesSimpleServerHandshake) {
    CapturingRtmpProtocol protocol;
    std::string c0c1(1 + sizeof(RtmpHandshake), '\0');
    c0c1[0] = HANDSHAKE_PLAINTEXT;
    feed(protocol, c0c1.substr(0, 200));
    EXPECT_TRUE(protocol.sent.empty());
    feed(protocol, c0c1.substr(200));

    ASSERT_EQ(3U, protocol.sent.size());
    EXPECT_EQ(1U, protocol.sent[0]->size());
    EXPECT_EQ(sizeof(RtmpHandshake), protocol.sent[1]->size());
    EXPECT_EQ(sizeof(RtmpHandshake), protocol.sent[2]->size());
    EXPECT_EQ(HANDSHAKE_PLAINTEXT, static_cast<uint8_t>(protocol.sent[0]->data()[0]));

    feed(protocol, std::string(sizeof(RtmpHandshake), '\0'));
    EXPECT_TRUE(protocol.received.empty());

    std::string message;
    message.push_back(static_cast<char>(CHUNK_VIDEO)); // fmt=0, chunk id=7
    message.append("\0\0\x05", 3);                  // timestamp
    message.append("\0\0\x03", 3);                  // body size
    message.push_back(static_cast<char>(MSG_VIDEO));
    message.append("\x01\0\0\0", 4);               // little-endian stream id
    message += "abc";
    feed(protocol, message);
    ASSERT_EQ(1U, protocol.received.size());
    EXPECT_EQ(MSG_VIDEO, protocol.received[0]->type_id);
    EXPECT_EQ(5U, protocol.received[0]->time_stamp);
    EXPECT_EQ(1U, protocol.received[0]->stream_index);
    EXPECT_EQ("abc", std::string(protocol.received[0]->data(), protocol.received[0]->size()));
}

TEST(RtmpProtocolTest, ServerRejectsUnsupportedHandshakeVersion) {
    CapturingRtmpProtocol protocol;
    std::string c0c1(1 + sizeof(RtmpHandshake), '\0');
    c0c1[0] = 1;
    EXPECT_THROW(feed(protocol, c0c1), std::runtime_error);
}

TEST(RtmpProtocolTest, HandlesInboundProtocolAndUserControls) {
    CapturingRtmpProtocol protocol;
    completeServerHandshake(protocol);

    feed(protocol, chunk(MSG_SET_CHUNK, be32(256)));
    feed(protocol, chunk(MSG_ACK, be32(100)));
    feed(protocol, chunk(MSG_WIN_SIZE, be32(64 * 1024)));
    feed(protocol, chunk(MSG_SET_PEER_BW, be32(500000) + std::string(1, '\x01')));
    feed(protocol, chunk(MSG_USER_CONTROL, be16(CONTROL_STREAM_BEGIN) + be32(7)));
    feed(protocol, chunk(MSG_USER_CONTROL, be16(CONTROL_STREAM_EOF) + be32(7)));
    feed(protocol, chunk(MSG_USER_CONTROL, be16(CONTROL_STREAM_DRY) + be32(8)));
    feed(protocol, chunk(MSG_USER_CONTROL, be16(CONTROL_PING_RESPONSE) + be32(123)));
    feed(protocol, chunk(MSG_USER_CONTROL, be16(99) + be32(0)));

    ASSERT_EQ(1U, protocol.stream_begin.size());
    ASSERT_EQ(1U, protocol.stream_eof.size());
    ASSERT_EQ(1U, protocol.stream_dry.size());
    EXPECT_EQ(7U, protocol.stream_begin[0]);
    EXPECT_EQ(7U, protocol.stream_eof[0]);
    EXPECT_EQ(8U, protocol.stream_dry[0]);

    size_t before_ping = protocol.sent.size();
    feed(protocol, chunk(MSG_USER_CONTROL, be16(CONTROL_PING_REQUEST) + be32(456)));
    EXPECT_GT(protocol.sent.size(), before_ping);
    auto response_header = reinterpret_cast<const RtmpHeader *>(protocol.sent[before_ping]->data());
    EXPECT_EQ(MSG_USER_CONTROL, response_header->type_id);
}

TEST(RtmpProtocolTest, RejectsTruncatedInboundControls) {
    CapturingRtmpProtocol protocol;
    completeServerHandshake(protocol);
    EXPECT_THROW(feed(protocol, chunk(MSG_ACK, "x")), std::runtime_error);

    CapturingRtmpProtocol user_control;
    completeServerHandshake(user_control);
    EXPECT_THROW(feed(user_control, chunk(MSG_USER_CONTROL, "x")), std::runtime_error);

    CapturingRtmpProtocol ping;
    completeServerHandshake(ping);
    EXPECT_THROW(feed(ping, chunk(MSG_USER_CONTROL, be16(CONTROL_PING_REQUEST))), std::runtime_error);
}
