#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// frame_stream.h – Stream reassembly of wire frames (Milestone 8)
//
// TCP is a byte stream: recv() may return half a frame or several frames.
// FrameStream buffers incoming bytes and yields whole Frames (the wire format
// itself lives in common/codec.h) as they complete.
// ─────────────────────────────────────────────────────────────────────────────
#include "common/codec.h"

#include <deque>
#include <vector>

namespace aegis {

class FrameStream {
public:
    // Append received bytes and extract every frame that is now complete.
    // Returns false once the stream is corrupt (bad length); the decoder then
    // stays failed and the connection should be closed.
    bool feed(const uint8_t* data, size_t len);

    // Pop the next complete frame; false if none is ready yet.
    bool next(Frame& out);

    bool   failed()   const { return failed_; }
    size_t buffered() const { return buf_.size() - pos_; }

private:
    std::vector<uint8_t> buf_;
    size_t               pos_{0};
    std::deque<Frame>    ready_;
    bool                 failed_{false};
};

}  // namespace aegis
