#include <gtest/gtest.h>

#include "Http/WebSocketSplitter.h"

using namespace mediakit;
using namespace toolkit;

class CapturingWebSocketSplitter : public WebSocketSplitter {
public:
    std::vector<WebSocketHeader> headers;
    std::vector<std::string> payload_parts;
    std::vector<size_t> received;
    size_t completed = 0;
    std::vector<Buffer::Ptr> encoded;

protected:
    void onWebSocketDecodeHeader(const WebSocketHeader &header) override { headers.push_back(header); }
    void onWebSocketDecodePayload(const WebSocketHeader &, const uint8_t *ptr, size_t len, size_t recved) override {
        payload_parts.emplace_back(reinterpret_cast<const char *>(ptr), len);
        received.push_back(recved);
    }
    void onWebSocketDecodeComplete(const WebSocketHeader &) override { ++completed; }
    void onWebSocketEncodeData(Buffer::Ptr buffer) override { encoded.emplace_back(std::move(buffer)); }
};

TEST(WebSocketSplitterTest, EncodesAndDecodesUnmaskedText) {
    CapturingWebSocketSplitter encoder;
    WebSocketHeader header;
    header._fin = true;
    header._reserved = 0;
    header._opcode = WebSocketHeader::TEXT;
    header._mask_flag = false;
    encoder.encode(header, std::make_shared<BufferString>("hello"));
    ASSERT_EQ(2U, encoder.encoded.size());

    std::string wire;
    for (const auto &part : encoder.encoded) wire.append(part->data(), part->size());
    CapturingWebSocketSplitter decoder;
    decoder.decode(reinterpret_cast<uint8_t *>(&wire[0]), wire.size());
    ASSERT_EQ(1U, decoder.headers.size());
    EXPECT_EQ(WebSocketHeader::TEXT, decoder.headers[0]._opcode);
    EXPECT_EQ("hello", decoder.payload_parts[0]);
    EXPECT_EQ(1U, decoder.completed);
}

TEST(WebSocketSplitterTest, HandlesMaskedPayloadInFragments) {
    const uint8_t mask[] = {1, 2, 3, 4};
    std::string wire;
    wire.push_back(static_cast<char>(0x82));
    wire.push_back(static_cast<char>(0x80 | 5));
    wire.append(reinterpret_cast<const char *>(mask), 4);
    const std::string plain = "abcde";
    for (size_t i = 0; i < plain.size(); ++i) wire.push_back(plain[i] ^ mask[i % 4]);

    CapturingWebSocketSplitter decoder;
    decoder.decode(reinterpret_cast<uint8_t *>(&wire[0]), 8);
    decoder.decode(reinterpret_cast<uint8_t *>(&wire[8]), wire.size() - 8);
    ASSERT_EQ(2U, decoder.payload_parts.size());
    EXPECT_EQ("ab", decoder.payload_parts[0]);
    EXPECT_EQ("cde", decoder.payload_parts[1]);
    EXPECT_EQ(1U, decoder.completed);
}

TEST(WebSocketSplitterTest, HandlesExtendedLengthAndMultipleFrames) {
    std::string payload(130, 'x');
    CapturingWebSocketSplitter encoder;
    WebSocketHeader header;
    header._fin = true;
    header._reserved = 0;
    header._opcode = WebSocketHeader::BINARY;
    header._mask_flag = false;
    encoder.encode(header, std::make_shared<BufferString>(payload));
    std::string wire;
    for (const auto &part : encoder.encoded) wire.append(part->data(), part->size());
    wire.append("\x89\x00", 2);

    CapturingWebSocketSplitter decoder;
    decoder.decode(reinterpret_cast<uint8_t *>(&wire[0]), wire.size());
    EXPECT_EQ(2U, decoder.headers.size());
    EXPECT_EQ(130U, decoder.headers[0]._payload_len);
    EXPECT_EQ(WebSocketHeader::PING, decoder.headers[1]._opcode);
    EXPECT_EQ(2U, decoder.completed);
}
