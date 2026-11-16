#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// crc32c.h – CRC32-C (Castagnoli) checksum
//
// crc32c(data, len, init=0) computes the CRC32-C checksum.
//
// Chaining:
//   crc32c(b, n2, crc32c(a, n1))  ==  crc32c(concat(a, b))
//
// Pre/post inversion is handled internally; the `init` parameter takes a
// previous CRC value (not the raw running state).
//
// Implementation selects between a table-driven software path and an
// SSE4.2 hardware path at runtime via __builtin_cpu_supports.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>

namespace aegis {

uint32_t crc32c(const void* data, size_t len, uint32_t init = 0);

} // namespace aegis
