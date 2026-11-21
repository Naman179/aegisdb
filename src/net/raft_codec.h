#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// raft_codec.h – Binary serialization for Raft RPC messages
// ─────────────────────────────────────────────────────────────────────────────
#include "common/codec.h"
#include "raft/raft_types.h"

namespace aegis {

// Message type tags
static constexpr uint8_t kMsgRequestVoteReq       = 0x01;
static constexpr uint8_t kMsgRequestVoteResp      = 0x02;
static constexpr uint8_t kMsgAppendEntriesReq     = 0x03;
static constexpr uint8_t kMsgAppendEntriesResp    = 0x04;
static constexpr uint8_t kMsgInstallSnapshotReq   = 0x05;
static constexpr uint8_t kMsgInstallSnapshotResp  = 0x06;

// Encode / Decode RequestVote
void encodeRequestVoteArgs(const RequestVoteArgs& args, std::vector<uint8_t>& out);
bool decodeRequestVoteArgs(const uint8_t* data, size_t len, RequestVoteArgs& out);

void encodeRequestVoteReply(const RequestVoteReply& reply, std::vector<uint8_t>& out);
bool decodeRequestVoteReply(const uint8_t* data, size_t len, RequestVoteReply& out);

// Encode / Decode AppendEntries
void encodeAppendEntriesArgs(const AppendEntriesArgs& args, std::vector<uint8_t>& out);
bool decodeAppendEntriesArgs(const uint8_t* data, size_t len, AppendEntriesArgs& out);

void encodeAppendEntriesReply(const AppendEntriesReply& reply, std::vector<uint8_t>& out);
bool decodeAppendEntriesReply(const uint8_t* data, size_t len, AppendEntriesReply& out);

// Encode / Decode InstallSnapshot
void encodeInstallSnapshotArgs(const InstallSnapshotArgs& args, std::vector<uint8_t>& out);
bool decodeInstallSnapshotArgs(const uint8_t* data, size_t len, InstallSnapshotArgs& out);

void encodeInstallSnapshotReply(const InstallSnapshotReply& reply, std::vector<uint8_t>& out);
bool decodeInstallSnapshotReply(const uint8_t* data, size_t len, InstallSnapshotReply& out);

}  // namespace aegis
