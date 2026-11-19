#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// kv_store.h – In-memory KV state machine for AegisDB (Milestone 3)
//
// KvStore is the deterministic state machine that Raft drives.  Every command
// must be encoded as a KvCommand, serialised to bytes, appended to the WAL
// (and to the Raft log), and then applied via KvStore::apply() in log order.
//
// Design decisions
// ────────────────
// • Versioning: every Put increments a per-key version counter.  Delete sets
//   version to 0 and clears the value.
// • Sessions: a client session ID (u64) prevents duplicate command execution
//   (at-most-once semantics for the client-facing API).  The store caches the
//   last result per session.
// • Snapshots: snapshot() serialises the full state; restore() replaces it.
//   Both use ByteWriter / ByteReader so the format is endian-independent.
// • No external dependencies beyond aegisdb_common.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
// KvCommandType
// ─────────────────────────────────────────────────────────────────────────────
enum class KvCommandType : uint8_t {
    kPut    = 1,
    kDelete = 2,
    kGet    = 3,  // read-only; recorded only for linearisability proof tests
};

// ─────────────────────────────────────────────────────────────────────────────
// KvCommand – the payload of one Raft log entry
// ─────────────────────────────────────────────────────────────────────────────
struct KvCommand {
    KvCommandType type{KvCommandType::kPut};
    uint64_t      sessionId{0};  // duplicate-detection key
    uint64_t      seqNo{0};      // per-session sequence number
    std::string   key;
    std::string   value;         // unused for kDelete / kGet

    // Serialise to / from bytes (little-endian, no external deps).
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* data, size_t len, KvCommand& out);
};

// ─────────────────────────────────────────────────────────────────────────────
// KvResult – returned by KvStore::apply()
// ─────────────────────────────────────────────────────────────────────────────
struct KvResult {
    bool        ok{true};          // false → key not found (Get/Delete)
    std::string value;             // set for successful Get
    uint64_t    version{0};        // current version after the command
};

// ─────────────────────────────────────────────────────────────────────────────
// KvStore
// ─────────────────────────────────────────────────────────────────────────────
class KvStore {
public:
    KvStore() = default;

    // Apply a command and return the result.
    // Duplicate (sessionId, seqNo) pairs return the cached result immediately
    // without mutating state.
    KvResult apply(const KvCommand& cmd);

    // Raw accessors (for tests / snapshots).
    std::optional<std::string> get(const std::string& key) const;
    uint64_t                   version(const std::string& key) const;
    size_t                     size() const { return store_.size(); }

    // Snapshot / restore (full serialisation of the KV state).
    std::vector<uint8_t> snapshot() const;
    bool                 restore(const uint8_t* data, size_t len);

private:
    struct Entry {
        std::string value;
        uint64_t    version{0};
    };

    // Duplicate-detection cache: maps (sessionId → (seqNo, KvResult)).
    struct SessionCache {
        uint64_t  seqNo{0};
        KvResult  result;
    };

    std::unordered_map<std::string, Entry>     store_;
    std::unordered_map<uint64_t, SessionCache> sessions_;
};

}  // namespace aegis
