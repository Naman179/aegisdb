// codec_test.cpp – Tests for Frame encode/decode, ByteWriter, and ByteReader.
#include <gtest/gtest.h>
#include "common/codec.h"

#include <cstring>

using namespace aegis;

// ═══════════════════════════════════════════════════════════════════════════
// Frame encode / decode
// ═══════════════════════════════════════════════════════════════════════════

TEST(CodecTest, EncodeDecodeRoundTrip) {
    constexpr uint8_t  kMsgType   = 0x42;
    constexpr uint64_t kRequestId = 0xDEADBEEFCAFEBABEULL;
    const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};

    std::vector<uint8_t> buf;
    encodeFrame(kMsgType, kRequestId, payload.data(), payload.size(), buf);

    Frame frame;
    size_t consumed = 0;
    ASSERT_EQ(decodeFrame(buf.data(), buf.size(), frame, &consumed), DecodeResult::OK);
    EXPECT_EQ(consumed,         buf.size());
    EXPECT_EQ(frame.msgType,   kMsgType);
    EXPECT_EQ(frame.requestId, kRequestId);
    EXPECT_EQ(frame.payload,   payload);
}

TEST(CodecTest, IncompleteBufferReturnsIncomplete) {
    std::vector<uint8_t> buf;
    // Minimal valid frame (empty payload → length=9, total=13 bytes).
    encodeFrame(1, 100, nullptr, 0, buf);

    Frame frame;
    size_t consumed = 0;

    // 3 bytes: can't read the 4-byte length header.
    EXPECT_EQ(decodeFrame(buf.data(), 3, frame, &consumed), DecodeResult::INCOMPLETE);

    // 4 bytes: header readable, but body not yet available.
    EXPECT_EQ(decodeFrame(buf.data(), 4, frame, &consumed), DecodeResult::INCOMPLETE);

    // One byte short of the full frame.
    EXPECT_EQ(decodeFrame(buf.data(), buf.size() - 1, frame, &consumed),
              DecodeResult::INCOMPLETE);
}

TEST(CodecTest, PayloadPreservedExactly) {
    std::vector<uint8_t> payload(4096);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
    }

    std::vector<uint8_t> buf;
    encodeFrame(0xFF, 0x1234567890ABCDEFULL, payload.data(), payload.size(), buf);

    Frame frame;
    size_t consumed = 0;
    ASSERT_EQ(decodeFrame(buf.data(), buf.size(), frame, &consumed), DecodeResult::OK);
    EXPECT_EQ(frame.payload, payload);
}

TEST(CodecTest, DecodeOversizedLengthIsInvalid) {
    // Build a 4-byte header whose `length` field exceeds 64 MiB.
    const uint32_t bad = kFrameMaxLength + 1u;
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>(bad);
    buf[1] = static_cast<uint8_t>(bad >>  8);
    buf[2] = static_cast<uint8_t>(bad >> 16);
    buf[3] = static_cast<uint8_t>(bad >> 24);

    Frame frame;
    size_t consumed = 0;
    EXPECT_EQ(decodeFrame(buf, 4u, frame, &consumed), DecodeResult::INVALID);
}

TEST(CodecTest, DecodeTooShortLengthIsInvalid) {
    // length < kFrameMinLength (9) is INVALID.
    const uint32_t bad = kFrameMinLength - 1u;  // == 8
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>(bad);
    buf[1] = static_cast<uint8_t>(bad >>  8);
    buf[2] = static_cast<uint8_t>(bad >> 16);
    buf[3] = static_cast<uint8_t>(bad >> 24);

    Frame frame;
    size_t consumed = 0;
    EXPECT_EQ(decodeFrame(buf, 4u, frame, &consumed), DecodeResult::INVALID);
}

TEST(CodecTest, TwoFramesBackToBack) {
    std::vector<uint8_t> buf;
    const std::vector<uint8_t> p1 = {0xAA};
    const std::vector<uint8_t> p2 = {0xBB, 0xCC};
    encodeFrame(1u, 10u, p1.data(), p1.size(), buf);
    encodeFrame(2u, 20u, p2.data(), p2.size(), buf);

    Frame f1, f2;
    size_t c1 = 0, c2 = 0;
    ASSERT_EQ(decodeFrame(buf.data(),       buf.size(),      f1, &c1), DecodeResult::OK);
    ASSERT_EQ(decodeFrame(buf.data() + c1,  buf.size() - c1, f2, &c2), DecodeResult::OK);

    EXPECT_EQ(f1.msgType,   1u);   EXPECT_EQ(f1.requestId, 10u);
    EXPECT_EQ(f1.payload,   p1);
    EXPECT_EQ(f2.msgType,   2u);   EXPECT_EQ(f2.requestId, 20u);
    EXPECT_EQ(f2.payload,   p2);
    EXPECT_EQ(c1 + c2,      buf.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// ByteWriter / ByteReader
// ═══════════════════════════════════════════════════════════════════════════

TEST(ByteWriterReaderTest, RoundTripAllTypes) {
    ByteWriter w;
    const std::vector<uint8_t> raw = {0x01, 0x02, 0x03};

    w.putU8(0xABu);
    w.putU32(0x12345678u);
    w.putU64(0xDEADBEEFCAFEBABEULL);
    w.putString("hello");
    w.putBytes(raw.data(), raw.size());

    auto bytes = w.take();
    ByteReader r(bytes.data(), bytes.size());

    uint8_t  u8  = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    std::string s;
    uint8_t  out[3] = {};

    EXPECT_TRUE(r.getU8(u8));     EXPECT_EQ(u8,  0xABu);
    EXPECT_TRUE(r.getU32(u32));   EXPECT_EQ(u32, 0x12345678u);
    EXPECT_TRUE(r.getU64(u64));   EXPECT_EQ(u64, 0xDEADBEEFCAFEBABEULL);
    EXPECT_TRUE(r.getString(s));  EXPECT_EQ(s,   "hello");
    EXPECT_TRUE(r.getBytes(out, 3u));
    EXPECT_EQ(out[0], 0x01u);
    EXPECT_EQ(out[1], 0x02u);
    EXPECT_EQ(out[2], 0x03u);
    EXPECT_TRUE(r.empty());
}

TEST(ByteWriterReaderTest, LittleEndianEncoding) {
    // Verify the actual byte order written to the buffer.
    ByteWriter w;
    w.putU32(0x01020304u);
    auto bytes = w.take();

    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x04u);  // least-significant byte first
    EXPECT_EQ(bytes[1], 0x03u);
    EXPECT_EQ(bytes[2], 0x02u);
    EXPECT_EQ(bytes[3], 0x01u);

    ByteWriter w2;
    w2.putU64(0x0102030405060708ULL);
    auto bytes2 = w2.take();
    ASSERT_EQ(bytes2.size(), 8u);
    EXPECT_EQ(bytes2[0], 0x08u);
    EXPECT_EQ(bytes2[7], 0x01u);
}

TEST(ByteWriterReaderTest, ReaderRejectsUnderflow) {
    // Buffer has only 3 bytes; getU32 must fail without advancing.
    uint8_t buf[3] = {0x01, 0x02, 0x03};
    ByteReader r(buf, 3u);

    uint32_t v = 0;
    EXPECT_FALSE(r.getU32(v));  // needs 4, only 3 available

    // Cursor must not have moved.
    uint8_t b = 0;
    EXPECT_TRUE(r.getU8(b));
    EXPECT_EQ(b, 0x01u);

    // Underflow on getBytes.
    uint8_t out[3] = {};
    EXPECT_FALSE(r.getBytes(out, 3u));  // 2 bytes left, need 3

    // Underflow on getU64.
    ByteReader r2(buf, 3u);
    uint64_t u64 = 0;
    EXPECT_FALSE(r2.getU64(u64));

    // getString underflow when prefix says more bytes than available.
    uint8_t longStr[6] = {0x05, 0x00, 0x00, 0x00, 'a', 'b'};  // says 5 bytes, only 2 follow
    ByteReader r3(longStr, 6u);
    std::string s;
    EXPECT_FALSE(r3.getString(s));
    // cursor must be back before the prefix
    EXPECT_EQ(r3.pos(), 0u);
}
