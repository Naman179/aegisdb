#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// client_protocol.h – Client-Server KV Wire Protocol (Milestone 9)
// ─────────────────────────────────────────────────────────────────────────────
#include "common/codec.h"
#include "raft/raft_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegis {

enum class StatusCode : uint8_t {
    kOk          = 0,
    kNotLeader   = 1,
    kRetry       = 2,
    kTimeout     = 3,
    kKeyNotFound = 4,
    kCasMismatch = 5,
    kError       = 6,
};

static constexpr uint8_t kMsgKvGetReq       = 0x10;
static constexpr uint8_t kMsgKvGetResp      = 0x11;
static constexpr uint8_t kMsgKvPutReq       = 0x12;
static constexpr uint8_t kMsgKvPutResp      = 0x13;
static constexpr uint8_t kMsgKvDelReq       = 0x14;
static constexpr uint8_t kMsgKvDelResp      = 0x15;
static constexpr uint8_t kMsgKvCasReq       = 0x16;
static constexpr uint8_t kMsgKvCasResp      = 0x17;
static constexpr uint8_t kMsgAdminFaultReq  = 0x30;
static constexpr uint8_t kMsgAdminFaultResp = 0x31;
static constexpr uint8_t kMsgAdminStatusReq = 0x32;
static constexpr uint8_t kMsgAdminStatusResp= 0x33;

// ── KV Requests and Responses ────────────────────────────────────────────────
struct KvGetReq {
    std::string clientId;
    uint64_t    seq{0};
    std::string key;
    bool        stale{false};  // true = fast local read, false = linearizable ReadIndex
};

struct KvGetResp {
    StatusCode  status{StatusCode::kOk};
    std::string value;
    uint64_t    version{0};
    NodeId      leaderId{kNone};
    std::string leaderAddr;
};

struct KvPutReq {
    std::string clientId;
    uint64_t    seq{0};
    std::string key;
    std::string value;
};

struct KvPutResp {
    StatusCode  status{StatusCode::kOk};
    uint64_t    version{0};
    NodeId      leaderId{kNone};
    std::string leaderAddr;
};

struct KvDelReq {
    std::string clientId;
    uint64_t    seq{0};
    std::string key;
};

struct KvDelResp {
    StatusCode  status{StatusCode::kOk};
    bool        deleted{false};
    NodeId      leaderId{kNone};
    std::string leaderAddr;
};

struct KvCasReq {
    std::string clientId;
    uint64_t    seq{0};
    std::string key;
    uint64_t    expectedVersion{0};
    std::string value;
};

struct KvCasResp {
    StatusCode  status{StatusCode::kOk};
    bool        success{false};
    uint64_t    version{0};
    NodeId      leaderId{kNone};
    std::string leaderAddr;
};

struct GroupStatus {
    int         groupId{0};
    RaftRole    role{RaftRole::kFollower};
    Term        term{0};
    NodeId      leaderId{kNone};
    LogIndex    commitIndex{0};
    LogIndex    lastApplied{0};
};

struct ClusterStatusResp {
    NodeId                   nodeId{kNone};
    std::vector<GroupStatus> groups;
};

// ── Serialization Helpers ───────────────────────────────────────────────────
void encodeKvGetReq(const KvGetReq& req, std::vector<uint8_t>& out);
bool decodeKvGetReq(const uint8_t* data, size_t len, KvGetReq& out);

void encodeKvGetResp(const KvGetResp& resp, std::vector<uint8_t>& out);
bool decodeKvGetResp(const uint8_t* data, size_t len, KvGetResp& out);

void encodeKvPutReq(const KvPutReq& req, std::vector<uint8_t>& out);
bool decodeKvPutReq(const uint8_t* data, size_t len, KvPutReq& out);

void encodeKvPutResp(const KvPutResp& resp, std::vector<uint8_t>& out);
bool decodeKvPutResp(const uint8_t* data, size_t len, KvPutResp& out);

void encodeKvDelReq(const KvDelReq& req, std::vector<uint8_t>& out);
bool decodeKvDelReq(const uint8_t* data, size_t len, KvDelReq& out);

void encodeKvDelResp(const KvDelResp& resp, std::vector<uint8_t>& out);
bool decodeKvDelResp(const uint8_t* data, size_t len, KvDelResp& out);

void encodeKvCasReq(const KvCasReq& req, std::vector<uint8_t>& out);
bool decodeKvCasReq(const uint8_t* data, size_t len, KvCasReq& out);

void encodeKvCasResp(const KvCasResp& resp, std::vector<uint8_t>& out);
bool decodeKvCasResp(const uint8_t* data, size_t len, KvCasResp& out);

void encodeClusterStatusResp(const ClusterStatusResp& resp, std::vector<uint8_t>& out);
bool decodeClusterStatusResp(const uint8_t* data, size_t len, ClusterStatusResp& out);

}  // namespace aegis
