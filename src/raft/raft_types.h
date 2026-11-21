#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// raft_types.h – Shared primitive types and RPC message structs for Raft
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <variant>
#include <vector>

namespace aegis {

using Term     = uint64_t;
using NodeId   = uint32_t;
using LogIndex = uint64_t;

static constexpr NodeId kNone = 0;  // "no node" sentinel

enum class RaftRole : uint8_t {
    kFollower  = 0,
    kCandidate = 1,
    kLeader    = 2,
    kPreCandidate = 3,  // PreVote phase: term not yet incremented
};

// ── Log entry ─────────────────────────────────────────────────────────────────
struct LogEntry {
    Term                 term{0};
    LogIndex             index{0};
    std::vector<uint8_t> data;
    bool                 noop{false};  // leader's term-start marker; never applied
};

// ── RequestVote ───────────────────────────────────────────────────────────────
struct RequestVoteArgs {
    Term     term{0};
    NodeId   candidateId{kNone};
    LogIndex lastLogIndex{0};
    Term     lastLogTerm{0};
    bool     preVote{false};  // true: probe only, do not change term/vote
};

struct RequestVoteReply {
    Term term{0};
    bool voteGranted{false};
    bool preVote{false};  // echoes the request's preVote flag
};

// ── AppendEntries ─────────────────────────────────────────────────────────────
struct AppendEntriesArgs {
    Term                  term{0};
    NodeId                leaderId{kNone};
    LogIndex              prevLogIndex{0};
    Term                  prevLogTerm{0};
    std::vector<LogEntry> entries;
    LogIndex              leaderCommit{0};
    uint64_t              readCtx{0};  // ReadIndex round id; 0 = plain heartbeat/append
};

struct AppendEntriesReply {
    Term     term{0};
    bool     success{false};
    LogIndex matchIndex{0};     // highest log index on follower (set on success)
    NodeId   followerId{kNone};
    LogIndex conflictIndex{0};  // fast back-track: first index of conflicting term
    Term     conflictTerm{0};   // fast back-track: term at conflictIndex
    uint64_t readCtx{0};        // echoes AppendEntriesArgs::readCtx
};

// A linearizable read that has been confirmed by a quorum: the caller may
// serve it once the state machine has applied everything up to `index`.
struct ReadState {
    uint64_t id{0};
    LogIndex index{0};
};

// ── Generic message wrapper ───────────────────────────────────────────────────
struct Message {
    NodeId from{kNone};
    NodeId to{kNone};
    std::variant<RequestVoteArgs,   RequestVoteReply,
                 AppendEntriesArgs, AppendEntriesReply> payload;
};

}  // namespace aegis
