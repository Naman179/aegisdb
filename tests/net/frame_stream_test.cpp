// frame_stream_test.cpp – Milestone 8: TCP stream reassembly (NetTest)
#include "net/frame_stream.h"

#include <random>
#include <gtest/gtest.h>

namespace aegis {
namespace {

struct Sent { uint8_t type; uint64_t id; std::vector<uint8_t> payload; };

void append(std::vector<uint8_t>& wire, const Sent& s) {
    encodeFrame(s.type, s.id, s.payload.data(), s.payload.size(), wire);
}

Sent make(uint8_t type, uint64_t id, size_t n, uint8_t fill = 0xAB) {
    return Sent{type, id, std::vector<uint8_t>(n, fill)};
}

void expectEq(const Frame& f, const Sent& s) {
    EXPECT_EQ(f.msgType, s.type);
    EXPECT_EQ(f.requestId, s.id);
    EXPECT_EQ(f.payload, s.payload);
}

TEST(NetTest, StreamYieldsFrameFedWhole) {
    Sent s = make(7, 0x1122334455667788ULL, 100);
    std::vector<uint8_t> wire;
    append(wire, s);

    FrameStream fs;
    ASSERT_TRUE(fs.feed(wire.data(), wire.size()));
    Frame f;
    ASSERT_TRUE(fs.next(f));
    expectEq(f, s);
    EXPECT_FALSE(fs.next(f));
    EXPECT_EQ(fs.buffered(), 0u);
}

TEST(NetTest, PartialRecvReassembles) {
    Sent s = make(3, 42, 500);
    std::vector<uint8_t> wire;
    append(wire, s);

    FrameStream fs;
    Frame f;
    for (size_t i = 0; i < wire.size(); ++i) {   // one byte per recv()
        ASSERT_TRUE(fs.feed(&wire[i], 1));
        if (i + 1 < wire.size()) ASSERT_FALSE(fs.next(f)) << "complete too early at " << i;
    }
    ASSERT_TRUE(fs.next(f));
    expectEq(f, s);
}

TEST(NetTest, MultipleFramesInOneBufferKeepOrder) {
    std::vector<uint8_t> wire;
    std::vector<Sent> sent;
    for (uint64_t i = 0; i < 20; ++i) {
        sent.push_back(make(static_cast<uint8_t>(i), i * 1000, i * 7, static_cast<uint8_t>(i)));
        append(wire, sent.back());
    }
    FrameStream fs;
    ASSERT_TRUE(fs.feed(wire.data(), wire.size()));
    for (auto& s : sent) {
        Frame f;
        ASSERT_TRUE(fs.next(f));
        expectEq(f, s);
    }
    Frame extra;
    EXPECT_FALSE(fs.next(extra));
}

TEST(NetTest, RandomChunkBoundariesReassemble) {
    std::mt19937 rng(7);
    std::vector<uint8_t> wire;
    std::vector<Sent> sent;
    for (int i = 0; i < 200; ++i) {
        sent.push_back(make(static_cast<uint8_t>(rng()), rng(), rng() % 300,
                            static_cast<uint8_t>(rng())));
        append(wire, sent.back());
    }
    FrameStream fs;
    std::vector<Frame> got;
    for (size_t pos = 0; pos < wire.size();) {
        size_t n = std::min<size_t>(1 + rng() % 97, wire.size() - pos);
        ASSERT_TRUE(fs.feed(wire.data() + pos, n));
        pos += n;
        Frame f;
        while (fs.next(f)) got.push_back(f);
    }
    ASSERT_EQ(got.size(), sent.size());
    for (size_t i = 0; i < sent.size(); ++i) expectEq(got[i], sent[i]);
}

TEST(NetTest, EmptyPayloadFrame) {
    Sent s = make(1, 5, 0);
    std::vector<uint8_t> wire;
    append(wire, s);
    FrameStream fs;
    ASSERT_TRUE(fs.feed(wire.data(), wire.size()));
    Frame f;
    ASSERT_TRUE(fs.next(f));
    expectEq(f, s);
}

TEST(NetTest, OversizedLengthFailsTheStream) {
    const uint8_t bad[] = {0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3};  // ~4 GB length
    FrameStream fs;
    EXPECT_FALSE(fs.feed(bad, sizeof bad));
    EXPECT_TRUE(fs.failed());
    Frame f;
    EXPECT_FALSE(fs.next(f));
    EXPECT_FALSE(fs.feed(bad, 1)) << "a failed stream stays failed";
}

TEST(NetTest, TooShortLengthFailsTheStream) {
    const uint8_t bad[] = {3, 0, 0, 0, 1, 2, 3};  // can't hold the 9-byte header
    FrameStream fs;
    EXPECT_FALSE(fs.feed(bad, sizeof bad));
    EXPECT_TRUE(fs.failed());
}

TEST(NetTest, ValidFramesBeforeCorruptionAreStillDelivered) {
    std::vector<uint8_t> wire;
    append(wire, make(2, 9, 4));
    const uint8_t bad[] = {1, 0, 0, 0, 0};
    wire.insert(wire.end(), bad, bad + sizeof bad);

    FrameStream fs;
    EXPECT_FALSE(fs.feed(wire.data(), wire.size()));
    Frame f;
    ASSERT_TRUE(fs.next(f));
    EXPECT_EQ(f.requestId, 9u);
}

}  // namespace
}  // namespace aegis
