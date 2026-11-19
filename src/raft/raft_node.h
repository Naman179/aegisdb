#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// raft_node.h – Single Raft group node
//
// Usage (test harness):
//   RaftNode node(id, peers, electionMin, electionMax, heartbeat, seed);
//   auto out = node.tick(deltaMs);       // advance timers → outbound msgs
//   auto out = node.step(inboundMsg);    // process one message → responses
//   bool ok  = node.propose(data);       // leader only; immediate replication
//   auto out = node.takePending();       // drain pending broadcast (after propose)
//   auto app = node.takeApplied();       // consume applied log entries
// ─────────────────────────────────────────────────────────────────────────────
#include "raft/raft_log.h"
#include "raft/raft_types.h"
#include <map>
#include <random>
#include <set>
#include <vector>

namespace aegis {

class RaftNode {
public:
    RaftNode(NodeId              id,
             std::vector<NodeId> peers,
             uint32_t            electionTimeoutMinMs = 150,
             uint32_t            electionTimeoutMaxMs = 300,
             uint32_t            heartbeatIntervalMs  = 50,
             uint32_t            seed                 = 0);

    // Advance time. Returns messages triggered by timeouts (heartbeat / election).
    std::vector<Message> tick(uint32_t ms);

    // Process one inbound message. Returns response messages.
    std::vector<Message> step(const Message& msg);

    // Propose a new entry (leader only). Returns false if not leader.
    // Broadcasts AppendEntries immediately into the pending queue.
    bool propose(std::vector<uint8_t> data);

    // Drain the pending outbound queue (call after propose or as needed).
    std::vector<Message> takePending();

    // Drain entries that have been committed and applied.
    std::vector<LogEntry> takeApplied();

    // ── State queries ─────────────────────────────────────────────────────────
    NodeId   id()           const { return id_; }
    Term     currentTerm()  const { return currentTerm_; }
    RaftRole role()         const { return role_; }
    bool     isLeader()     const { return role_ == RaftRole::kLeader; }
    NodeId   votedFor()     const { return votedFor_; }
    LogIndex commitIndex()  const { return log_.commitIndex(); }
    LogIndex lastLogIndex() const { return log_.lastIndex(); }

private:
    // ── Role transitions ──────────────────────────────────────────────────────
    void becomeFollower(Term term);
    void becomeCandidate();
    void becomeLeader();

    // ── Helpers ───────────────────────────────────────────────────────────────
    void resetElectionTimeout();
    bool isLogUpToDate(LogIndex lastIdx, Term lastTerm) const;
    void maybeAdvanceCommit();
    void broadcastAppendEntries();
    void sendAppendEntries(NodeId to);
    void drainApplicable();

    // ── Message handlers ──────────────────────────────────────────────────────
    void handleRequestVote(const Message& msg, const RequestVoteArgs& args);
    void handleRequestVoteReply(const Message& msg, const RequestVoteReply& rpl);
    void handleAppendEntries(const Message& msg, const AppendEntriesArgs& args);
    void handleAppendEntriesReply(const Message& msg, const AppendEntriesReply& rpl);

    // ── Identity ──────────────────────────────────────────────────────────────
    NodeId              id_;
    std::vector<NodeId> peers_;

    // ── Config ────────────────────────────────────────────────────────────────
    uint32_t electionTimeoutMinMs_;
    uint32_t electionTimeoutMaxMs_;
    uint32_t heartbeatIntervalMs_;

    // ── Persistent state (would be flushed to WAL in M5) ─────────────────────
    Term   currentTerm_{0};
    NodeId votedFor_{kNone};

    // ── Volatile state ────────────────────────────────────────────────────────
    RaftRole role_{RaftRole::kFollower};
    RaftLog  log_;

    // ── Election timer ────────────────────────────────────────────────────────
    uint32_t         electionElapsedMs_{0};
    uint32_t         electionTimeoutMs_{0};
    std::set<NodeId> votesGranted_;
    std::mt19937     rng_;

    // ── Leader state ──────────────────────────────────────────────────────────
    uint32_t                   heartbeatElapsedMs_{0};
    std::map<NodeId, LogIndex> nextIndex_;
    std::map<NodeId, LogIndex> matchIndex_;

    // ── Output queues ─────────────────────────────────────────────────────────
    std::vector<Message>  pending_;   // outbound messages (tick + step + propose)
    std::vector<LogEntry> applied_;   // entries ready for state machine
};

}  // namespace aegis
