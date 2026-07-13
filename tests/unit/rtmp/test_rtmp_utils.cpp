#include <gtest/gtest.h>

#include "Rtmp/utils.h"

TEST(RtmpUtilsTest, LoadsAndStoresIntegersWithExplicitByteOrder) {
    unsigned char data[4] = {0};
    set_be32(data, 0x12345678);
    EXPECT_EQ(0x12345678U, load_be32(data));
    EXPECT_EQ(0x123456U, load_be24(data));

    set_le32(data, 0x89ABCDEF);
    EXPECT_EQ(0x89ABCDEFU, load_le32(data));
    EXPECT_EQ(0xEFCDU, load_be16(data));

    set_be24(data, 0xA1B2C3);
    EXPECT_EQ(0xA1B2C3U, load_be24(data));
}
