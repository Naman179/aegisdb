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
};

// ── Log entry ─────────────────────────────────────────────────────────────────
struct LogEntry {
    Term                 term{0};
    LogIndex             index{0};
    std::vector<uint8_t> data;
};

// ── RequestVote ───────────────────────────────────────────────────────────────
struct RequestVoteArgs {
    Term     term{0};
    NodeId   candidateId{kNone};
    LogIndex lastLogIndex{0};
    Term     lastLogTerm{0};
};

struct RequestVoteReply {
    Term term{0};
    bool voteGranted{false};
};

// ── AppendEntries ─────────────────────────────────────────────────────────────
struct AppendEntriesArgs {
    Term                  term{0};
    NodeId                leaderId{kNone};
    LogIndex              prevLogIndex{0};
    Term                  prevLogTerm{0};
    std::vector<LogEntry> entries;
    LogIndex              leaderCommit{0};
};

struct AppendEntriesReply {
    Term     term{0};
    bool     success{false};
    LogIndex matchIndex{0};     // highest log index on follower (set on success)
    NodeId   followerId{kNone};
    LogIndex conflictIndex{0};  // fast back-track: first index of conflicting term
    Term     conflictTerm{0};   // fast back-track: term at conflictIndex
};

// ── Generic message wrapper ───────────────────────────────────────────────────
struct Message {
    NodeId from{kNone};
    NodeId to{kNone};
    std::variant<RequestVoteArgs,   RequestVoteReply,
                 AppendEntriesArgs, AppendEntriesReply> payload;
};

}  // namespace aegis
