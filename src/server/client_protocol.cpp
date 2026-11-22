#include "server/client_protocol.h"
#include <cstring>

namespace aegis {

void encodeKvGetReq(const KvGetReq& req, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putString(req.clientId);
    w.putU64(req.seq);
    w.putString(req.key);
    w.putU8(req.stale ? 1 : 0);
    out = w.take();
}

bool decodeKvGetReq(const uint8_t* data, size_t len, KvGetReq& out) {
    ByteReader r(data, len);
    uint8_t st = 0;
    if (!r.getString(out.clientId)) return false;
    if (!r.getU64(out.seq)) return false;
    if (!r.getString(out.key)) return false;
    if (!r.getU8(st)) return false;
    out.stale = (st != 0);
    return true;
}

void encodeKvGetResp(const KvGetResp& resp, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(resp.status));
    w.putString(resp.value);
    w.putU64(resp.version);
    w.putU32(resp.leaderId);
    w.putString(resp.leaderAddr);
    out = w.take();
}

bool decodeKvGetResp(const uint8_t* data, size_t len, KvGetResp& out) {
    ByteReader r(data, len);
    uint8_t st = 0;
    if (!r.getU8(st)) return false;
    out.status = static_cast<StatusCode>(st);
    if (!r.getString(out.value)) return false;
    if (!r.getU64(out.version)) return false;
    if (!r.getU32(out.leaderId)) return false;
    if (!r.getString(out.leaderAddr)) return false;
    return true;
}

void encodeKvPutReq(const KvPutReq& req, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putString(req.clientId);
    w.putU64(req.seq);
    w.putString(req.key);
    w.putString(req.value);
    out = w.take();
}

bool decodeKvPutReq(const uint8_t* data, size_t len, KvPutReq& out) {
    ByteReader r(data, len);
    if (!r.getString(out.clientId)) return false;
    if (!r.getU64(out.seq)) return false;
    if (!r.getString(out.key)) return false;
    if (!r.getString(out.value)) return false;
    return true;
}

void encodeKvPutResp(const KvPutResp& resp, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(resp.status));
    w.putU64(resp.version);
    w.putU32(resp.leaderId);
    w.putString(resp.leaderAddr);
    out = w.take();
}

bool decodeKvPutResp(const uint8_t* data, size_t len, KvPutResp& out) {
    ByteReader r(data, len);
    uint8_t st = 0;
    if (!r.getU8(st)) return false;
    out.status = static_cast<StatusCode>(st);
    if (!r.getU64(out.version)) return false;
    if (!r.getU32(out.leaderId)) return false;
    if (!r.getString(out.leaderAddr)) return false;
    return true;
}

void encodeKvDelReq(const KvDelReq& req, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putString(req.clientId);
    w.putU64(req.seq);
    w.putString(req.key);
    out = w.take();
}

bool decodeKvDelReq(const uint8_t* data, size_t len, KvDelReq& out) {
    ByteReader r(data, len);
    if (!r.getString(out.clientId)) return false;
    if (!r.getU64(out.seq)) return false;
    if (!r.getString(out.key)) return false;
    return true;
}

void encodeKvDelResp(const KvDelResp& resp, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(resp.status));
    w.putU8(resp.deleted ? 1 : 0);
    w.putU32(resp.leaderId);
    w.putString(resp.leaderAddr);
    out = w.take();
}

bool decodeKvDelResp(const uint8_t* data, size_t len, KvDelResp& out) {
    ByteReader r(data, len);
    uint8_t st = 0, del = 0;
    if (!r.getU8(st)) return false;
    out.status = static_cast<StatusCode>(st);
    if (!r.getU8(del)) return false;
    out.deleted = (del != 0);
    if (!r.getU32(out.leaderId)) return false;
    if (!r.getString(out.leaderAddr)) return false;
    return true;
}

void encodeKvCasReq(const KvCasReq& req, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putString(req.clientId);
    w.putU64(req.seq);
    w.putString(req.key);
    w.putU64(req.expectedVersion);
    w.putString(req.value);
    out = w.take();
}

bool decodeKvCasReq(const uint8_t* data, size_t len, KvCasReq& out) {
    ByteReader r(data, len);
    if (!r.getString(out.clientId)) return false;
    if (!r.getU64(out.seq)) return false;
    if (!r.getString(out.key)) return false;
    if (!r.getU64(out.expectedVersion)) return false;
    if (!r.getString(out.value)) return false;
    return true;
}

void encodeKvCasResp(const KvCasResp& resp, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(resp.status));
    w.putU8(resp.success ? 1 : 0);
    w.putU64(resp.version);
    w.putU32(resp.leaderId);
    w.putString(resp.leaderAddr);
    out = w.take();
}

bool decodeKvCasResp(const uint8_t* data, size_t len, KvCasResp& out) {
    ByteReader r(data, len);
    uint8_t st = 0, succ = 0;
    if (!r.getU8(st)) return false;
    out.status = static_cast<StatusCode>(st);
    if (!r.getU8(succ)) return false;
    out.success = (succ != 0);
    if (!r.getU64(out.version)) return false;
    if (!r.getU32(out.leaderId)) return false;
    if (!r.getString(out.leaderAddr)) return false;
    return true;
}

void encodeClusterStatusResp(const ClusterStatusResp& resp, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU32(resp.nodeId);
    w.putU32(static_cast<uint32_t>(resp.groups.size()));
    for (const auto& g : resp.groups) {
        w.putU32(static_cast<uint32_t>(g.groupId));
        w.putU8(static_cast<uint8_t>(g.role));
        w.putU64(g.term);
        w.putU32(g.leaderId);
        w.putU64(g.commitIndex);
        w.putU64(g.lastApplied);
    }
    out = w.take();
}

bool decodeClusterStatusResp(const uint8_t* data, size_t len, ClusterStatusResp& out) {
    ByteReader r(data, len);
    uint32_t count = 0;
    if (!r.getU32(out.nodeId)) return false;
    if (!r.getU32(count)) return false;
    out.groups.clear();
    out.groups.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        GroupStatus g;
        uint32_t gid = 0;
        uint8_t rl = 0;
        if (!r.getU32(gid)) return false;
        g.groupId = static_cast<int>(gid);
        if (!r.getU8(rl)) return false;
        g.role = static_cast<RaftRole>(rl);
        if (!r.getU64(g.term)) return false;
        if (!r.getU32(g.leaderId)) return false;
        if (!r.getU64(g.commitIndex)) return false;
        if (!r.getU64(g.lastApplied)) return false;
        out.groups.push_back(g);
    }
    return true;
}

void encodeAdminFaultReq(const AdminFaultReq& req, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putString(req.action);
    w.putString(req.peer);
    uint64_t dropBits = 0;
    std::memcpy(&dropBits, &req.dropRate, sizeof(double));
    w.putU64(dropBits);
    w.putU32(req.delayMs);
    out = w.take();
}

bool decodeAdminFaultReq(const uint8_t* data, size_t len, AdminFaultReq& out) {
    ByteReader r(data, len);
    uint64_t dropBits = 0;
    if (!r.getString(out.action)) return false;
    if (!r.getString(out.peer)) return false;
    if (!r.getU64(dropBits)) return false;
    std::memcpy(&out.dropRate, &dropBits, sizeof(double));
    if (!r.getU32(out.delayMs)) return false;
    return true;
}

void encodeAdminFaultResp(const AdminFaultResp& resp, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU8(resp.ok ? 1 : 0);
    w.putString(resp.message);
    out = w.take();
}

bool decodeAdminFaultResp(const uint8_t* data, size_t len, AdminFaultResp& out) {
    ByteReader r(data, len);
    uint8_t okByte = 0;
    if (!r.getU8(okByte)) return false;
    out.ok = (okByte != 0);
    if (!r.getString(out.message)) return false;
    return true;
}

}  // namespace aegis
