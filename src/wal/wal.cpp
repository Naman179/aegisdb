// ─────────────────────────────────────────────────────────────────────────────
// wal.cpp – Write-Ahead Log implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "wal/wal.h"
#include "common/codec.h"
#include "common/crc32c.h"
#include "common/logger.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
// Internal constants
// ─────────────────────────────────────────────────────────────────────────────
// Record header on disk: crc32c(4) + type(1) + length(4) = 9 bytes
static constexpr size_t kRecordHeaderBytes = 9;

// ─────────────────────────────────────────────────────────────────────────────
// Segment helpers
// ─────────────────────────────────────────────────────────────────────────────

// Canonical segment filename for the given numeric ID.
static std::string segmentPath(const std::string& dir, uint64_t id) {
    char buf[48];
    snprintf(buf, sizeof(buf), "wal-%016llx.seg",
             static_cast<unsigned long long>(id));
    return dir + "/" + buf;
}

// Return all *.seg paths in `dir`, sorted lexicographically (== chronologically
// because the filename encodes the ID as fixed-width hex).
static std::vector<std::string> listSegments(const std::string& dir) {
    std::vector<std::string> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name(ent->d_name);
        // Accept files matching "wal-????????????????.seg" (prefix 4, suffix 4).
        if (name.size() >= 9 &&
            name.substr(0, 4) == "wal-" &&
            name.substr(name.size() - 4) == ".seg") {
            result.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    std::sort(result.begin(), result.end());
    return result;
}

// Extract the numeric segment ID from a path produced by segmentPath().
static uint64_t idFromPath(const std::string& path) {
    // Find the last path separator.
    size_t slash = path.rfind('/');
    std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
    // fname == "wal-<16hex>.seg"; digits start at index 4.
    return strtoull(fname.c_str() + 4, nullptr, 16);
}

// ─────────────────────────────────────────────────────────────────────────────
// CRC helpers
// ─────────────────────────────────────────────────────────────────────────────

// Compute the CRC for a WAL record payload:
//   CRC( type_byte || len_u32_le || data )
static uint32_t recordCrc(uint8_t typeByte, uint32_t len,
                           const uint8_t* data) {
    // Encode length as 4-byte LE.
    uint8_t lenBuf[4] = {
        static_cast<uint8_t>(len        & 0xFFu),
        static_cast<uint8_t>((len >>  8) & 0xFFu),
        static_cast<uint8_t>((len >> 16) & 0xFFu),
        static_cast<uint8_t>((len >> 24) & 0xFFu),
    };
    uint32_t crc = crc32c(&typeByte, 1);
    crc = crc32c(lenBuf, 4, crc);
    if (data && len) crc = crc32c(data, len, crc);
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// WalWriter
// ─────────────────────────────────────────────────────────────────────────────

WalWriter::~WalWriter() {
    if (fd_ >= 0) {
        fsync(fd_);
        close(fd_);
        fd_ = -1;
    }
}

bool WalWriter::fsyncDir() {
    int dfd = ::open(dir_.c_str(), O_RDONLY);
    if (dfd < 0) {
        LOG(LogLevel::ERROR, "WAL fsyncDir open(%s): %s",
            dir_.c_str(), strerror(errno));
        return false;
    }
    bool ok = (::fsync(dfd) == 0);
    ::close(dfd);
    return ok;
}

bool WalWriter::openSegment(uint64_t id) {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }

    segmentId_    = id;
    bytesWritten_ = 0;

    std::string path = segmentPath(dir_, id);
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        LOG(LogLevel::ERROR, "WAL openSegment(%s): %s",
            path.c_str(), strerror(errno));
        return false;
    }

    // Track existing size (we may be reopening an in-progress segment).
    struct stat st;
    if (::fstat(fd_, &st) == 0) {
        bytesWritten_ = static_cast<uint64_t>(st.st_size);
    }

    // fsync the directory so the new filename is durable.
    return fsyncDir();
}

bool WalWriter::rollover() {
    // Ensure the current segment is fully durable before creating a new one.
    if (!sync()) return false;
    return openSegment(segmentId_ + 1);
}

/* static */
std::unique_ptr<WalWriter> WalWriter::open(const std::string& dir,
                                            uint64_t           maxSegmentBytes) {
    // Create the directory if it doesn't exist.
    if (::mkdir(dir.c_str(), 0755) == -1 && errno != EEXIST) {
        LOG(LogLevel::ERROR, "WAL mkdir(%s): %s",
            dir.c_str(), strerror(errno));
        return nullptr;
    }

    // Determine the next segment ID (highest existing + 1, or 0 if none).
    auto segs = listSegments(dir);
    uint64_t nextId = 0;
    if (!segs.empty()) {
        nextId = idFromPath(segs.back()) + 1;
    }

    auto w = std::unique_ptr<WalWriter>(new WalWriter());
    w->dir_             = dir;
    w->maxSegmentBytes_ = maxSegmentBytes;

    if (!w->openSegment(nextId)) return nullptr;
    return w;
}

bool WalWriter::append(WalRecordType type, const uint8_t* data, size_t len) {
    uint8_t  typeByte = static_cast<uint8_t>(type);
    uint32_t len32    = static_cast<uint32_t>(len);
    uint32_t crc      = recordCrc(typeByte, len32, data);

    // Serialise the record header + payload using ByteWriter.
    ByteWriter bw;
    bw.putU32(crc);
    bw.putU8(typeByte);
    bw.putU32(len32);
    if (data && len) bw.putBytes(data, len);

    const auto& buf = bw.data();
    ssize_t written = ::write(fd_, buf.data(), buf.size());
    if (written != static_cast<ssize_t>(buf.size())) {
        LOG(LogLevel::ERROR, "WAL write: %s", strerror(errno));
        return false;
    }
    bytesWritten_ += static_cast<uint64_t>(buf.size());

    // Roll over if the segment has grown beyond the configured limit.
    if (bytesWritten_ >= maxSegmentBytes_) {
        return rollover();
    }
    return true;
}

bool WalWriter::append(WalRecordType type, const std::string& data) {
    return append(type,
                  reinterpret_cast<const uint8_t*>(data.data()),
                  data.size());
}

bool WalWriter::sync() {
    if (fd_ < 0) return false;
    return ::fsync(fd_) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// WalReader
// ─────────────────────────────────────────────────────────────────────────────

WalReader::~WalReader() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

/* static */
std::unique_ptr<WalReader> WalReader::open(const std::string& dir) {
    auto segs = listSegments(dir);
    // An empty WAL directory is valid; next() will return false immediately.
    auto r = std::unique_ptr<WalReader>(new WalReader());
    r->segments_ = std::move(segs);
    r->segIdx_   = 0;
    r->ok_       = true;
    r->fd_       = -1;

    if (!r->segments_.empty()) {
        r->fd_ = ::open(r->segments_[0].c_str(), O_RDONLY);
        if (r->fd_ < 0) {
            LOG(LogLevel::ERROR, "WAL WalReader::open(%s): %s",
                r->segments_[0].c_str(), strerror(errno));
            return nullptr;
        }
    }
    return r;
}

bool WalReader::openNextSegment() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    ++segIdx_;
    if (segIdx_ >= segments_.size()) return false;

    fd_ = ::open(segments_[segIdx_].c_str(), O_RDONLY);
    if (fd_ < 0) {
        LOG(LogLevel::ERROR, "WAL WalReader::openNextSegment(%s): %s",
            segments_[segIdx_].c_str(), strerror(errno));
        ok_ = false;
        return false;
    }
    return true;
}

bool WalReader::next(WalRecord* out) {
    // Outer loop: advance through segments when a segment is exhausted.
    for (;;) {
        if (fd_ < 0) {
            // Try to open the next segment; if none remain, replay is complete.
            if (!openNextSegment()) return false;
        }

        // Read the fixed 9-byte record header.
        uint8_t hdr[kRecordHeaderBytes];
        ssize_t n = ::read(fd_, hdr, sizeof(hdr));
        if (n == 0) {
            // Clean end-of-segment; move to next.
            if (!openNextSegment()) return false;
            continue;
        }
        if (n != static_cast<ssize_t>(sizeof(hdr))) {
            // Torn header – treat as end of WAL (not a CRC error).
            return false;
        }

        // Decode header fields.
        ByteReader br(hdr, sizeof(hdr));
        uint32_t storedCrc = 0;
        uint8_t  typeByte  = 0;
        uint32_t len       = 0;
        if (!br.getU32(storedCrc) || !br.getU8(typeByte) || !br.getU32(len)) {
            return false;
        }

        // Read payload.
        std::vector<uint8_t> payload(len);
        if (len > 0) {
            ssize_t pn = ::read(fd_, payload.data(), len);
            if (pn != static_cast<ssize_t>(len)) {
                // Torn payload.
                return false;
            }
        }

        // Verify CRC.
        uint32_t computedCrc = recordCrc(typeByte, len,
                                          len ? payload.data() : nullptr);
        if (computedCrc != storedCrc) {
            LOG(LogLevel::WARN, "WAL CRC mismatch – stopping replay");
            ok_ = false;
            return false;
        }

        out->type = static_cast<WalRecordType>(typeByte);
        out->data = std::move(payload);
        return true;
    }
}

}  // namespace aegis
