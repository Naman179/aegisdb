#include "net/frame_stream.h"

namespace aegis {

bool FrameStream::feed(const uint8_t* data, size_t len) {
    if (failed_) return false;
    buf_.insert(buf_.end(), data, data + len);

    for (;;) {
        Frame  f;
        size_t used = 0;
        DecodeResult r = decodeFrame(buf_.data() + pos_, buf_.size() - pos_, f, &used);
        if (r == DecodeResult::INCOMPLETE) break;
        if (r == DecodeResult::INVALID) { failed_ = true; return false; }
        ready_.push_back(std::move(f));
        pos_ += used;
    }

    // Reclaim the consumed prefix once it dominates the buffer.
    if (pos_ > 0 && pos_ >= buf_.size() / 2) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(pos_));
        pos_ = 0;
    }
    return true;
}

bool FrameStream::next(Frame& out) {
    if (ready_.empty()) return false;
    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

}  // namespace aegis
