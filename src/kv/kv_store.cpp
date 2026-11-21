// ─────────────────────────────────────────────────────────────────────────────
// kv_store.cpp – KvStore state-machine implementation (Milestone 3)
// ─────────────────────────────────────────────────────────────────────────────
#include "kv/kv_store.h"
#include "common/codec.h"
#include "common/logger.h"

#include <cstring>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
// KvCommand serialisation
// ─────────────────────────────────────────────────────────────────────────────
// Wire format (all little-endian):
//   type      u8
//   sessionId u64
//   seqNo     u64
//   key       u32-prefixed string
//   value     u32-prefixed string   (empty for kDelete / kGet)

std::vector<uint8_t> KvCommand::encode() const {
    ByteWriter bw;
    bw.putU8(static_cast<uint8_t>(type));
    bw.putU64(sessionId);
    bw.putU64(seqNo);
    bw.putString(key);
    bw.putString(value);
    bw.putU64(expectedVersion);
    return bw.take();
}

bool KvCommand::decode(const uint8_t* data, size_t len, KvCommand& out) {
    ByteReader br(data, len);
    uint8_t typeByte = 0;
    if (!br.getU8(typeByte))         return false;
    if (!br.getU64(out.sessionId))   return false;
    if (!br.getU64(out.seqNo))       return false;
    if (!br.getString(out.key))      return false;
    if (!br.getString(out.value))    return false;
    if (br.remaining() >= sizeof(uint64_t)) {
        br.getU64(out.expectedVersion);
    } else {
        out.expectedVersion = 0;
    }
    out.type = static_cast<KvCommandType>(typeByte);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// KvStore::apply
// ─────────────────────────────────────────────────────────────────────────────
KvResult KvStore::apply(const KvCommand& cmd) {
    // Duplicate detection: return cached result if we've seen this (session, seqNo).
    if (cmd.sessionId != 0) {
        auto sit = sessions_.find(cmd.sessionId);
        if (sit != sessions_.end() && sit->second.seqNo == cmd.seqNo) {
            return sit->second.result;
        }
    }

    KvResult result;

    switch (cmd.type) {
    case KvCommandType::kPut: {
        auto& entry  = store_[cmd.key];
        entry.value  = cmd.value;
        entry.version++;
        result.ok      = true;
        result.version = entry.version;
        result.value   = entry.value;
        break;
    }
    case KvCommandType::kDelete: {
        auto it = store_.find(cmd.key);
        if (it == store_.end()) {
            result.ok      = false;
            result.version = 0;
        } else {
            result.ok      = true;
            result.version = it->second.version;
            store_.erase(it);
        }
        break;
    }
    case KvCommandType::kGet: {
        auto it = store_.find(cmd.key);
        if (it == store_.end()) {
            result.ok      = false;
            result.version = 0;
        } else {
            result.ok      = true;
            result.value   = it->second.value;
            result.version = it->second.version;
        }
        break;
    }
    case KvCommandType::kCas: {
        auto it = store_.find(cmd.key);
        uint64_t curVer = (it != store_.end()) ? it->second.version : 0;
        if (curVer != cmd.expectedVersion) {
            result.ok      = false;
            result.version = curVer;
        } else {
            auto& entry    = store_[cmd.key];
            entry.value    = cmd.value;
            entry.version++;
            result.ok      = true;
            result.version = entry.version;
            result.value   = entry.value;
        }
        break;
    }
    }

    // Cache result for session dedup (skip sessions with id 0 = no session).
    if (cmd.sessionId != 0) {
        sessions_[cmd.sessionId] = {cmd.seqNo, result};
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Raw accessors
// ─────────────────────────────────────────────────────────────────────────────
std::optional<std::string> KvStore::get(const std::string& key) const {
    auto it = store_.find(key);
    if (it == store_.end()) return std::nullopt;
    return it->second.value;
}

uint64_t KvStore::version(const std::string& key) const {
    auto it = store_.find(key);
    return (it == store_.end()) ? 0u : it->second.version;
}

// ─────────────────────────────────────────────────────────────────────────────
// Snapshot / restore
// ─────────────────────────────────────────────────────────────────────────────
// Snapshot format:
//   store_count u32
//   for each entry:
//     key     u32-prefixed string
//     value   u32-prefixed string
//     version u64
//   session_count u32
//   for each session:
//     sessionId u64
//     seqNo     u64
//     result.ok u8
//     result.value u32-prefixed string
//     result.version u64

std::vector<uint8_t> KvStore::snapshot() const {
    ByteWriter bw;
    bw.putU32(static_cast<uint32_t>(store_.size()));
    for (auto& [k, e] : store_) {
        bw.putString(k);
        bw.putString(e.value);
        bw.putU64(e.version);
    }
    bw.putU32(static_cast<uint32_t>(sessions_.size()));
    for (auto& [sid, sc] : sessions_) {
        bw.putU64(sid);
        bw.putU64(sc.seqNo);
        bw.putU8(sc.result.ok ? 1u : 0u);
        bw.putString(sc.result.value);
        bw.putU64(sc.result.version);
    }
    return bw.take();
}

bool KvStore::restore(const uint8_t* data, size_t len) {
    ByteReader br(data, len);

    std::unordered_map<std::string, Entry>     newStore;
    std::unordered_map<uint64_t, SessionCache> newSessions;

    uint32_t storeCount = 0;
    if (!br.getU32(storeCount)) return false;
    for (uint32_t i = 0; i < storeCount; ++i) {
        std::string key, value;
        uint64_t    ver = 0;
        if (!br.getString(key))   return false;
        if (!br.getString(value)) return false;
        if (!br.getU64(ver))      return false;
        newStore[key] = {value, ver};
    }

    uint32_t sessionCount = 0;
    if (!br.getU32(sessionCount)) return false;
    for (uint32_t i = 0; i < sessionCount; ++i) {
        uint64_t sid = 0, seqNo = 0, ver = 0;
        uint8_t  okByte = 0;
        std::string val;
        if (!br.getU64(sid))    return false;
        if (!br.getU64(seqNo))  return false;
        if (!br.getU8(okByte))  return false;
        if (!br.getString(val)) return false;
        if (!br.getU64(ver))    return false;
        newSessions[sid] = {seqNo, {okByte != 0, val, ver}};
    }

    store_    = std::move(newStore);
    sessions_ = std::move(newSessions);
    return true;
}

}  // namespace aegis
