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

    // ReadIndex (linearizable reads, Raft thesis §6.4). Leader only.
    // Returns a read id (>0), or 0 if this node is not a leader that has
    // committed an entry in its own term yet. Call takePending() afterwards
    // to send the confirming heartbeat round. Once a majority has answered,
    // the read shows up in takeReadStates() with the commit index to wait for.
    uint64_t requestRead();
    std::vector<ReadState> takeReadStates();

    // ── Log compaction (Raft §7) ──────────────────────────────────────────────
    // The application calls this after it has applied entries up to `index` and
    // serialised its state machine into `snapshot`. The log prefix is dropped
    // and the bytes are kept so lagging followers can be sent InstallSnapshot.
    // Returns false if `index` is not an applied index in the log.
    bool compact(LogIndex index, std::vector<uint8_t> snapshot);
    LogIndex snapshotIndex() const { return log_.snapshotIndex(); }

    // Set when a leader's snapshot replaced our state; the application must
    // restore it before consuming further takeApplied() output.
    std::optional<InstalledSnapshot> takeInstalledSnapshot();

    // Drain entries that have been committed and applied.
    std::vector<LogEntry> takeApplied();

    // ── State queries ─────────────────────────────────────────────────────────
    NodeId   id()           const { return id_; }
    Term     currentTerm()  const { return currentTerm_; }
    RaftRole role()         const { return role_; }
    bool     isLeader()     const { return role_ == RaftRole::kLeader; }
    NodeId   votedFor()     const { return votedFor_; }
    NodeId   leaderId()     const { return leaderId_; }  // kNone if unknown
    LogIndex commitIndex()  const { return log_.commitIndex(); }
    LogIndex lastLogIndex() const { return log_.lastIndex(); }
    LogIndex lastApplied()  const { return log_.lastApplied(); }
    const RaftLog& log()    const { return log_; }  // read-only, for tests/diagnostics

private:
    // ── Role transitions ──────────────────────────────────────────────────────
    void becomeFollower(Term term);
    void startPreVote();
    void becomeCandidate();
    void becomeLeader();

    // ── Helpers ───────────────────────────────────────────────────────────────
    void resetElectionTimeout();
    bool isLogUpToDate(LogIndex lastIdx, Term lastTerm) const;
    size_t quorum() const { return (peers_.size() + 1) / 2 + 1; }
    void maybeAdvanceCommit();
    bool checkQuorumHealthy();
    void broadcastAppendEntries();
    void sendAppendEntries(NodeId to, uint64_t readCtx = 0);
    void ackReads(NodeId from, uint64_t ctx);
    void drainApplicable();

    // ── Message handlers ──────────────────────────────────────────────────────
    void handleRequestVote(const Message& msg, const RequestVoteArgs& args);
    void handlePreVote(const Message& msg, const RequestVoteArgs& args);
    void handleRequestVoteReply(const Message& msg, const RequestVoteReply& rpl);
    void handleAppendEntries(const Message& msg, const AppendEntriesArgs& args);
    void handleAppendEntriesReply(const Message& msg, const AppendEntriesReply& rpl);
    void handleInstallSnapshot(const Message& msg, const InstallSnapshotArgs& args);
    void handleInstallSnapshotReply(const Message& msg, const InstallSnapshotReply& rpl);

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
    NodeId leaderId_{kNone};  // last leader heard from this term (volatile)

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
    uint32_t                   checkQuorumElapsedMs_{0};
    std::set<NodeId>           heardFrom_;   // peers that replied this window
    std::map<NodeId, LogIndex> nextIndex_;
    std::map<NodeId, LogIndex> matchIndex_;

    // ── ReadIndex ─────────────────────────────────────────────────────────────
    struct PendingRead {
        uint64_t         id;
        LogIndex         index;
        std::set<NodeId> acks;
    };
    uint64_t                 readSeq_{0};
    std::vector<PendingRead> pendingReads_;
    std::vector<ReadState>   readStates_;

    // ── Snapshots ─────────────────────────────────────────────────────────────
    std::vector<uint8_t>             snapshotData_;      // bytes at log_.snapshotIndex()
    std::optional<InstalledSnapshot> installedSnapshot_;  // pending for the app

    // ── Output queues ─────────────────────────────────────────────────────────
    std::vector<Message>  pending_;   // outbound messages (tick + step + propose)
    std::vector<LogEntry> applied_;   // entries ready for state machine
};

}  // namespace aegis
