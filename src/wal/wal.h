#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// wal.h – Write-Ahead Log (WAL) for AegisDB
//
// On-disk segment format
// ───────────────────────
// Segment files are named  wal-<016hex>.seg  and stored in a single directory.
// Each file is a contiguous sequence of records; there is no per-segment header.
//
// Record layout (little-endian):
//   crc32c   u32   CRC of (type_byte || len_u32_le || data)
//   type     u8    WalRecordType
//   length   u32   payload length in bytes
//   data     [length]
//
// A torn record (partial write at the tail of the last segment) is detected by
// a short read or a CRC mismatch and silently terminates replay.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
// WalRecordType
// ─────────────────────────────────────────────────────────────────────────────
enum class WalRecordType : uint8_t {
    kData       = 1,  // Generic key-value mutation.
    kCheckpoint = 2,  // Safe truncation point.
    kBarrier    = 3,  // Epoch / group boundary.
};

// ─────────────────────────────────────────────────────────────────────────────
// WalRecord – decoded record returned by WalReader::next()
// ─────────────────────────────────────────────────────────────────────────────
struct WalRecord {
    WalRecordType        type{WalRecordType::kData};
    std::vector<uint8_t> data;
};

// ─────────────────────────────────────────────────────────────────────────────
// WalWriter
// Appends records to a WAL directory; rolls over to a new segment file when
// the current segment exceeds maxSegmentBytes.
//
// Thread-safety: NOT thread-safe.  Callers must serialise access.
// ─────────────────────────────────────────────────────────────────────────────
class WalWriter {
public:
    static constexpr uint64_t kDefaultMaxSegmentBytes = 64ULL * 1024 * 1024;  // 64 MiB

    // Open (or create) a WAL in `dir`.  Returns nullptr on error.
    static std::unique_ptr<WalWriter> open(
        const std::string& dir,
        uint64_t           maxSegmentBytes = kDefaultMaxSegmentBytes);

    ~WalWriter();

    // Append one record.  Returns false on I/O error.
    bool append(WalRecordType type, const uint8_t* data, size_t len);
    bool append(WalRecordType type, const std::string& data);

    // fsync the current segment to durable storage.
    bool sync();

    uint64_t currentSegmentId() const { return segmentId_; }
    uint64_t bytesWritten()     const { return bytesWritten_; }

private:
    WalWriter() = default;

    // Opens segment `id` for appending; fsyncs the directory afterwards.
    bool openSegment(uint64_t id);
    // Syncs and opens the next segment.
    bool rollover();
    // fsync the WAL directory (makes the new segment filename durable).
    bool fsyncDir();

    std::string dir_;
    uint64_t    maxSegmentBytes_{kDefaultMaxSegmentBytes};
    uint64_t    segmentId_{0};
    uint64_t    bytesWritten_{0};
    int         fd_{-1};
};

// ─────────────────────────────────────────────────────────────────────────────
// WalReader
// Replays all records from a WAL directory in write order (oldest segment
// first, records within a segment in append order).
// ─────────────────────────────────────────────────────────────────────────────
class WalReader {
public:
    // Open a WAL directory for sequential replay.  Returns nullptr on error.
    static std::unique_ptr<WalReader> open(const std::string& dir);

    ~WalReader();

    // Advance to the next record.  Returns false when done or on error.
    bool next(WalRecord* out);

    // True unless a CRC mismatch was encountered (torn write).
    bool ok() const { return ok_; }

private:
    WalReader() = default;

    bool openNextSegment();

    std::vector<std::string> segments_;   // sorted segment paths
    size_t                   segIdx_{0};
    int                      fd_{-1};
    bool                     ok_{true};
};

}  // namespace aegis
