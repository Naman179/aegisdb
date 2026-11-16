// codec.cpp – ByteWriter, ByteReader, frame encode/decode
#include "common/codec.h"

#include <cstring>

namespace aegis {

// ─── ByteWriter ──────────────────────────────────────────────────────────────

void ByteWriter::putU8(uint8_t v) {
    buf_.push_back(v);
}

void ByteWriter::putU32(uint32_t v) {
    buf_.push_back(static_cast<uint8_t>(v));
    buf_.push_back(static_cast<uint8_t>(v >>  8));
    buf_.push_back(static_cast<uint8_t>(v >> 16));
    buf_.push_back(static_cast<uint8_t>(v >> 24));
}

void ByteWriter::putU64(uint64_t v) {
    buf_.push_back(static_cast<uint8_t>(v));
    buf_.push_back(static_cast<uint8_t>(v >>  8));
    buf_.push_back(static_cast<uint8_t>(v >> 16));
    buf_.push_back(static_cast<uint8_t>(v >> 24));
    buf_.push_back(static_cast<uint8_t>(v >> 32));
    buf_.push_back(static_cast<uint8_t>(v >> 40));
    buf_.push_back(static_cast<uint8_t>(v >> 48));
    buf_.push_back(static_cast<uint8_t>(v >> 56));
}

void ByteWriter::putBytes(const uint8_t* data, size_t len) {
    if (len > 0 && data != nullptr) {
        buf_.insert(buf_.end(), data, data + len);
    }
}

void ByteWriter::putString(const std::string& s) {
    putU32(static_cast<uint32_t>(s.size()));
    buf_.insert(buf_.end(),
                reinterpret_cast<const uint8_t*>(s.data()),
                reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}

// ─── ByteReader ──────────────────────────────────────────────────────────────

bool ByteReader::getU8(uint8_t& v) {
    if (pos_ + 1u > len_) return false;
    v = data_[pos_++];
    return true;
}

bool ByteReader::getU32(uint32_t& v) {
    if (pos_ + 4u > len_) return false;
    v = static_cast<uint32_t>(data_[pos_    ])
      | static_cast<uint32_t>(data_[pos_ + 1]) <<  8
      | static_cast<uint32_t>(data_[pos_ + 2]) << 16
      | static_cast<uint32_t>(data_[pos_ + 3]) << 24;
    pos_ += 4u;
    return true;
}

bool ByteReader::getU64(uint64_t& v) {
    if (pos_ + 8u > len_) return false;
    v = static_cast<uint64_t>(data_[pos_    ])
      | static_cast<uint64_t>(data_[pos_ + 1]) <<  8
      | static_cast<uint64_t>(data_[pos_ + 2]) << 16
      | static_cast<uint64_t>(data_[pos_ + 3]) << 24
      | static_cast<uint64_t>(data_[pos_ + 4]) << 32
      | static_cast<uint64_t>(data_[pos_ + 5]) << 40
      | static_cast<uint64_t>(data_[pos_ + 6]) << 48
      | static_cast<uint64_t>(data_[pos_ + 7]) << 56;
    pos_ += 8u;
    return true;
}

bool ByteReader::getBytes(uint8_t* out, size_t len) {
    if (pos_ + len > len_) return false;
    if (len > 0) {
        std::memcpy(out, data_ + pos_, len);
    }
    pos_ += len;
    return true;
}

bool ByteReader::getString(std::string& s) {
    uint32_t slen = 0;
    if (!getU32(slen)) return false;
    if (pos_ + slen > len_) {
        // Undo the getU32 advance so the cursor is not left in a half-read state.
        pos_ -= 4u;
        return false;
    }
    s.assign(reinterpret_cast<const char*>(data_ + pos_), slen);
    pos_ += slen;
    return true;
}

// ─── Frame encode / decode ───────────────────────────────────────────────────

void encodeFrame(uint8_t msgType, uint64_t requestId,
                 const uint8_t* payload, size_t payloadLen,
                 std::vector<uint8_t>& out) {
    // length field = msgType(1) + requestId(8) + payload
    const uint32_t length = 1u + 8u + static_cast<uint32_t>(payloadLen);

    ByteWriter w;
    w.putU32(length);
    w.putU8(msgType);
    w.putU64(requestId);
    w.putBytes(payload, payloadLen);

    auto bytes = w.take();
    out.insert(out.end(), bytes.begin(), bytes.end());
}

DecodeResult decodeFrame(const uint8_t* buf, size_t len,
                         Frame& out, size_t* bytesConsumed) {
    // Need at least the 4-byte length header.
    if (len < kFrameHeaderSize) {
        return DecodeResult::INCOMPLETE;
    }

    // Decode length without touching ByteReader yet (keep things simple).
    const uint32_t length =
        static_cast<uint32_t>(buf[0])
      | static_cast<uint32_t>(buf[1]) <<  8
      | static_cast<uint32_t>(buf[2]) << 16
      | static_cast<uint32_t>(buf[3]) << 24;

    if (length < kFrameMinLength || length > kFrameMaxLength) {
        return DecodeResult::INVALID;
    }

    const size_t totalNeeded = kFrameHeaderSize + static_cast<size_t>(length);
    if (len < totalNeeded) {
        return DecodeResult::INCOMPLETE;
    }

    // Decode the body (after the 4-byte length header).
    ByteReader r(buf + kFrameHeaderSize, length);

    uint8_t  msgType   = 0;
    uint64_t requestId = 0;
    if (!r.getU8(msgType))    return DecodeResult::INVALID;
    if (!r.getU64(requestId)) return DecodeResult::INVALID;

    const size_t payloadLen = static_cast<size_t>(length) - 1u - 8u;
    out.msgType   = msgType;
    out.requestId = requestId;
    out.payload.resize(payloadLen);
    if (payloadLen > 0) {
        if (!r.getBytes(out.payload.data(), payloadLen)) {
            return DecodeResult::INVALID;
        }
    }

    *bytesConsumed = totalNeeded;
    return DecodeResult::OK;
}

} // namespace aegis
