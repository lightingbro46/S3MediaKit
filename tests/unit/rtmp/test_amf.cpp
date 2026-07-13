#include <gtest/gtest.h>

#include "Network/Buffer.h"
#include "Rtmp/amf.h"

using toolkit::BufferLikeString;

static AMFValue decodeValue(const AMFEncoder &encoder) {
    BufferLikeString buffer(encoder.data());
    AMFDecoder decoder(buffer, 0);
    return decoder.load<AMFValue>();
}

TEST(AmfValueTest, ConvertsScalarTypes) {
    AMFValue text("hello");
    AMFValue number(12.5);
    AMFValue integer(42);
    AMFValue boolean(true);

    EXPECT_EQ(AMF_STRING, text.type());
    EXPECT_EQ("hello", text.as_string());
    EXPECT_DOUBLE_EQ(12.5, number.as_number());
    EXPECT_EQ(12, number.as_integer());
    EXPECT_TRUE(number.as_boolean());
    EXPECT_EQ(42, integer.as_integer());
    EXPECT_DOUBLE_EQ(42.0, integer.as_number());
    EXPECT_TRUE(boolean.as_boolean());
    EXPECT_EQ("true", boolean.to_string());
    EXPECT_THROW(text.as_number(), std::runtime_error);
    EXPECT_THROW(number.as_string(), std::runtime_error);
}

TEST(AmfValueTest, CopiesAndClearsValues) {
    AMFValue original("payload");
    AMFValue copy(original);
    original.clear();
    EXPECT_EQ("", original.as_string());
    EXPECT_EQ("payload", copy.as_string());

    AMFValue assigned;
    assigned = AMFValue(99);
    EXPECT_EQ(99, assigned.as_integer());
    EXPECT_TRUE(static_cast<bool>(assigned));
    EXPECT_FALSE(static_cast<bool>(AMFValue()));
}

TEST(AmfValueTest, ManagesObjectsAndArrays) {
    AMFValue object(AMF_OBJECT);
    object.set("name", AMFValue("camera"));
    object.set("enabled", AMFValue(true));
    EXPECT_EQ("camera", object["name"].as_string());
    EXPECT_EQ(AMF_NULL, object["missing"].type());

    size_t fields = 0;
    object.object_for_each([&](const std::string &, const AMFValue &) { ++fields; });
    EXPECT_EQ(2U, fields);
    EXPECT_THROW(AMFValue(1).set("x", AMFValue(2)), std::runtime_error);
    EXPECT_THROW(AMFValue(1).object_for_each([](const std::string &, const AMFValue &) {}),
                 std::runtime_error);

    AMFValue array(AMF_STRICT_ARRAY);
    array.add(AMFValue(1));
    array.add(AMFValue("two"));
    EXPECT_EQ("strict_array", array.to_string());
    EXPECT_THROW(object.add(AMFValue()), std::runtime_error);
}

TEST(AmfCodecTest, RoundTripsScalars) {
    AMFEncoder encoder;
    encoder << std::string("hello");
    BufferLikeString buffer(encoder.data());
    AMFDecoder decoder(buffer, 0);
    EXPECT_EQ("hello", decoder.load<std::string>());

    encoder.clear();
    encoder << 123.25;
    BufferLikeString number_buffer(encoder.data());
    AMFDecoder number_decoder(number_buffer, 0);
    EXPECT_DOUBLE_EQ(123.25, number_decoder.load<double>());

    encoder.clear();
    encoder << true;
    BufferLikeString bool_buffer(encoder.data());
    AMFDecoder bool_decoder(bool_buffer, 0);
    EXPECT_TRUE(bool_decoder.load<bool>());
}

TEST(AmfCodecTest, RoundTripsCompositeValues) {
    AMFValue object(AMF_OBJECT);
    object.set("name", AMFValue("cam"));
    object.set("count", AMFValue(3));
    AMFEncoder object_encoder;
    object_encoder << object;
    AMFValue decoded_object = decodeValue(object_encoder);
    EXPECT_EQ("cam", decoded_object["name"].as_string());
    EXPECT_EQ(3, decoded_object["count"].as_integer());

    AMFValue ecma(AMF_ECMA_ARRAY);
    ecma.set("ok", AMFValue(true));
    AMFEncoder ecma_encoder;
    ecma_encoder << ecma;
    EXPECT_TRUE(decodeValue(ecma_encoder)["ok"].as_boolean());

    AMFValue array(AMF_STRICT_ARRAY);
    array.add(AMFValue(1));
    array.add(AMFValue("two"));
    AMFEncoder array_encoder;
    array_encoder << array;
    EXPECT_EQ(AMF_STRICT_ARRAY, decodeValue(array_encoder).type());
}

TEST(AmfCodecTest, HandlesNullUndefinedAndMalformedData) {
    AMFEncoder encoder;
    encoder << nullptr << AMFValue(AMF_UNDEFINED);
    BufferLikeString buffer(encoder.data());
    AMFDecoder decoder(buffer, 0);
    EXPECT_EQ(AMF_NULL, decoder.load<AMFValue>().type());
    EXPECT_EQ(AMF_UNDEFINED, decoder.load<AMFValue>().type());

    BufferLikeString empty("");
    AMFDecoder empty_decoder(empty, 0);
    EXPECT_THROW(empty_decoder.load<AMFValue>(), std::runtime_error);

    BufferLikeString wrong(std::string(1, '\x01'));
    AMFDecoder wrong_decoder(wrong, 0);
    EXPECT_THROW(wrong_decoder.load<std::string>(), std::runtime_error);
}
