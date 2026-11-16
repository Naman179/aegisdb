#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// codec.h – Wire-frame codec, ByteWriter, and ByteReader
//
// Frame format (little-endian):
//   | length u32 | msgType u8 | requestId u64 | payload ... |
//
// `length` = sizeof(msgType) + sizeof(requestId) + sizeof(payload)
//          = 1 + 8 + payload.size()
//
// Valid length range: [9, 64 MiB].
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aegis {

// ═══════════════════════════════════════════════════════════════════════════
// ByteWriter
// Appends little-endian values into an internal buffer.
// All multi-byte integers are encoded with explicit shifts – no memcpy –
// so the result is correct regardless of host endianness.
// ═══════════════════════════════════════════════════════════════════════════
class ByteWriter {
public:
    void putU8(uint8_t v);
    void putU32(uint32_t v);
    void putU64(uint64_t v);
    void putBytes(const uint8_t* data, size_t len);

    // Encodes as: u32 length prefix (UTF-8 byte count) + raw bytes.
    void putString(const std::string& s);

    const std::vector<uint8_t>& data() const { return buf_; }

    // Moves out the internal buffer (leaves ByteWriter empty).
    std::vector<uint8_t> take() { return std::move(buf_); }

    void   clear() { buf_.clear(); }
    size_t size()  const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

// ═══════════════════════════════════════════════════════════════════════════
// ByteReader
// Bounds-checked cursor over a caller-owned byte buffer.
// Every getter returns false on underflow and does NOT advance the cursor.
// ═══════════════════════════════════════════════════════════════════════════
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t len)
        : data_(data), len_(len), pos_(0) {}

    bool getU8(uint8_t& v);
    bool getU32(uint32_t& v);
    bool getU64(uint64_t& v);
    bool getBytes(uint8_t* out, size_t len);

    // Reads u32 length prefix, then that many bytes into s.
    bool getString(std::string& s);

    size_t remaining() const { return len_ - pos_; }
    size_t pos()       const { return pos_; }
    bool   empty()     const { return pos_ >= len_; }

private:
    const uint8_t* data_;
    size_t         len_;
    size_t         pos_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Frame
// ═══════════════════════════════════════════════════════════════════════════
struct Frame {
    uint8_t              msgType   = 0;
    uint64_t             requestId = 0;
    std::vector<uint8_t> payload;
};

enum class DecodeResult { OK, INCOMPLETE, INVALID };

// length field limits (the `length` field covers msgType+requestId+payload)
static constexpr uint32_t kFrameHeaderSize = 4u;           // 4-byte length field
static constexpr uint32_t kFrameMinLength  = 9u;           // msgType(1)+requestId(8)
static constexpr uint32_t kFrameMaxLength  = 64u << 20;    // 64 MiB

// Encodes one frame and appends it to `out`.
// payload may be nullptr when payloadLen == 0.
void encodeFrame(uint8_t msgType, uint64_t requestId,
                 const uint8_t* payload, size_t payloadLen,
                 std::vector<uint8_t>& out);

// Decodes one frame from buf[0..len).
// On OK sets *bytesConsumed = kFrameHeaderSize + length.
// On INCOMPLETE or INVALID, *bytesConsumed is unspecified.
DecodeResult decodeFrame(const uint8_t* buf, size_t len,
                         Frame& out, size_t* bytesConsumed);

} // namespace aegis
