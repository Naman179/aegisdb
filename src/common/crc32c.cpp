// crc32c.cpp – CRC32-C with software table path and optional SSE4.2 hardware path.
//
// Design notes
// ─────────────────────────────────────────────────────────────────────────────
// 1. Pre/post inversion: state = ~init before processing; return ~state after.
//    This makes crc32c(b, n2, crc32c(a, n1)) == crc32c(concat(a,b)) for any
//    split of the input, and crc32c(nullptr, 0) == 0.
//
// 2. The hardware function is decorated with __attribute__((target("sse4.2")))
//    so it is compiled with SSE4.2 instruction support even when the TU is not.
//    Runtime selection uses __builtin_cpu_supports("sse4.2") and a one-time
//    atomic flag.
//
// 3. x86-only code is guarded by preprocessor so the file compiles cleanly on
//    ARM/RISC-V/etc., falling through to the software path.
// ─────────────────────────────────────────────────────────────────────────────
#include "common/crc32c.h"

#include <atomic>
#include <cstdint>

#if defined(__SSE4_2__) || defined(__x86_64__) || defined(__i386__)
#  include <nmmintrin.h>   // _mm_crc32_u8/u32/u64
#endif

#include <mutex>

namespace aegis {

// ─── Software table (Castagnoli polynomial, reflected 0x82F63B78) ────────────

static uint32_t s_table[256];
static std::once_flag s_tableFlag;

static void buildTable() noexcept {
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
        }
        s_table[i] = crc;
    }
}

// Thread-safe one-time table initialization
static void ensureTable() noexcept {
    std::call_once(s_tableFlag, buildTable);
}

static uint32_t crc32c_sw(const uint8_t* data, size_t len, uint32_t state) noexcept {
    for (size_t i = 0; i < len; ++i) {
        state = (state >> 8) ^ s_table[(state ^ data[i]) & 0xFFu];
    }
    return state;
}

// ─── Hardware path (x86 SSE4.2 only) ─────────────────────────────────────────

#if defined(__x86_64__) || defined(__i386__)

__attribute__((target("sse4.2")))
static uint32_t crc32c_hw(const uint8_t* data, size_t len, uint32_t state) noexcept {
    size_t i = 0;

#  if defined(__x86_64__)
    // Process 8 bytes at a time on 64-bit.
    for (; i + 8u <= len; i += 8u) {
        uint64_t chunk =
            static_cast<uint64_t>(data[i    ])
          | static_cast<uint64_t>(data[i + 1]) <<  8
          | static_cast<uint64_t>(data[i + 2]) << 16
          | static_cast<uint64_t>(data[i + 3]) << 24
          | static_cast<uint64_t>(data[i + 4]) << 32
          | static_cast<uint64_t>(data[i + 5]) << 40
          | static_cast<uint64_t>(data[i + 6]) << 48
          | static_cast<uint64_t>(data[i + 7]) << 56;
        state = static_cast<uint32_t>(_mm_crc32_u64(state, chunk));
    }
#  endif

    // Remaining bytes (or all bytes on 32-bit).
    for (; i < len; ++i) {
        state = _mm_crc32_u8(state, data[i]);
    }
    return state;
}

static bool probeSSE42() noexcept {
    return __builtin_cpu_supports("sse4.2");
}

#endif // x86

// ─── Public API ──────────────────────────────────────────────────────────────

uint32_t crc32c(const void* data, size_t len, uint32_t init) {
    ensureTable();

    // Map `init` to internal running state so chaining works correctly:
    //   crc32c(b, n2, crc32c(a, n1)) == crc32c(concat(a, b))
    uint32_t state = ~init;

    if (len == 0 || data == nullptr) {
        return ~state;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);

#if defined(__x86_64__) || defined(__i386__)
    // -1 = not yet detected, 0 = SW, 1 = HW
    static std::atomic<int> useHW{-1};
    int hw = useHW.load(std::memory_order_relaxed);
    if (hw < 0) {
        hw = probeSSE42() ? 1 : 0;
        useHW.store(hw, std::memory_order_relaxed);
    }
    state = (hw == 1) ? crc32c_hw(bytes, len, state)
                      : crc32c_sw(bytes, len, state);
#else
    state = crc32c_sw(bytes, len, state);
#endif

    return ~state;
}

} // namespace aegis
