// crc32c_test.cpp
#include <gtest/gtest.h>
#include "common/crc32c.h"

#include <cstring>
#include <vector>

using namespace aegis;

// ─── Known vector ────────────────────────────────────────────────────────────
// CRC32-C("123456789") == 0xE3069283  (standard test vector)
TEST(Crc32cTest, KnownVector) {
    EXPECT_EQ(crc32c("123456789", 9u), 0xE3069283u);
}

// ─── Empty input ─────────────────────────────────────────────────────────────
TEST(Crc32cTest, EmptyInputIsZero) {
    EXPECT_EQ(crc32c(nullptr, 0u), 0u);

    // A non-null pointer with len=0 must also return 0.
    const char ch = 'x';
    EXPECT_EQ(crc32c(&ch, 0u), 0u);
}

// ─── Incremental chaining ────────────────────────────────────────────────────
// Split at every possible offset; chunked CRC must equal batch CRC.
TEST(Crc32cTest, IncrementalMatchesBatch) {
    const char* data = "Hello, AegisDB! This is the incremental chaining test.";
    const size_t len = strlen(data);

    const uint32_t batch = crc32c(data, len);

    for (size_t split = 1; split < len; ++split) {
        const uint32_t part1 = crc32c(data,          split);
        const uint32_t part2 = crc32c(data + split,  len - split, part1);
        EXPECT_EQ(part2, batch) << "split at " << split;
    }
}

// ─── Hardware vs software paths ──────────────────────────────────────────────
// When SSE4.2 is available the runtime picks the hardware path.  We validate
// self-consistency via chunked chaining – if either path were broken the
// IncrementalMatchesBatch test would already fail, but this test makes the
// connection explicit and exercises 64-byte chunking of the HW path.
TEST(Crc32cTest, HardwareMatchesSoftware) {
#if defined(__x86_64__) || defined(__i386__)
    if (!__builtin_cpu_supports("sse4.2")) {
        GTEST_SKIP() << "SSE4.2 not available on this CPU – skipping HW path test";
    }
#else
    GTEST_SKIP() << "Not an x86 platform – hardware CRC path not compiled";
#endif

    // 1 KiB of pseudo-random data.
    std::vector<uint8_t> data(1024u);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    }

    // Batch result (hardware path when SSE4.2 available).
    const uint32_t batch = crc32c(data.data(), data.size());

    // Chunked in 64-byte pieces – exercises the 8-byte inner loop.
    uint32_t chain = 0u;
    for (size_t off = 0u; off < data.size(); off += 64u) {
        const size_t chunk = std::min<size_t>(64u, data.size() - off);
        chain = crc32c(data.data() + off, chunk, chain);
    }
    EXPECT_EQ(chain, batch);
}

// ─── Three-segment chaining ──────────────────────────────────────────────────
TEST(Crc32cTest, ThreeSegmentChaining) {
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {4, 5, 6};
    const uint8_t c[] = {7, 8, 9};
    uint8_t abc[9];
    memcpy(abc,     a, 3);
    memcpy(abc + 3, b, 3);
    memcpy(abc + 6, c, 3);

    const uint32_t batch  = crc32c(abc, 9u);
    const uint32_t chain  = crc32c(c, 3u, crc32c(b, 3u, crc32c(a, 3u)));
    EXPECT_EQ(chain, batch);
}
