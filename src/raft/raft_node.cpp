// ─────────────────────────────────────────────────────────────────────────────
// raft_node.cpp – Single Raft group node implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "raft/raft_node.h"
#include "common/logger.h"

#include <algorithm>
#include <cassert>

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

RaftNode::RaftNode(NodeId id, std::vector<NodeId> peers,
                   uint32_t electionTimeoutMinMs,
                   uint32_t electionTimeoutMaxMs,
                   uint32_t heartbeatIntervalMs,
                   uint32_t seed)
    : id_(id)
    , peers_(std::move(peers))
    , electionTimeoutMinMs_(electionTimeoutMinMs)
    , electionTimeoutMaxMs_(electionTimeoutMaxMs)
    , heartbeatIntervalMs_(heartbeatIntervalMs)
    , rng_(seed ^ (static_cast<uint32_t>(id) * 2654435761u))
{
    resetElectionTimeout();
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::resetElectionTimeout() {
    std::uniform_int_distribution<uint32_t> dist(
        electionTimeoutMinMs_, electionTimeoutMaxMs_);
    electionTimeoutMs_ = dist(rng_);
    electionElapsedMs_ = 0;
}

bool RaftNode::isLogUpToDate(LogIndex lastIdx, Term lastTerm) const {
    if (lastTerm != log_.lastTerm())  return lastTerm > log_.lastTerm();
    return lastIdx >= log_.lastIndex();
}

void RaftNode::drainApplicable() {
    for (auto& e : log_.takeApplicable()) {
        if (e.noop) continue;  // protocol-internal, never reaches the state machine
        applied_.push_back(std::move(e));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Role transitions
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::becomeFollower(Term term) {
    if (term > currentTerm_) {
        currentTerm_ = term;
        votedFor_    = kNone;
    }
    role_ = RaftRole::kFollower;
    resetElectionTimeout();
}

void RaftNode::becomeCandidate() {
    role_         = RaftRole::kCandidate;
    currentTerm_++;
    votedFor_     = id_;     // vote for self
    votesGranted_.clear();
    votesGranted_.insert(id_);
    resetElectionTimeout();

    LOG(LogLevel::INFO, "node %u starting election term %llu",
        id_, (unsigned long long)currentTerm_);

    // A single-node cluster already holds a quorum with its own vote.
    if (votesGranted_.size() >= quorum()) { becomeLeader(); return; }

    // Broadcast RequestVote to all peers.
    for (NodeId p : peers_) {
        pending_.push_back(Message{
            id_, p,
            RequestVoteArgs{currentTerm_, id_,
                            log_.lastIndex(), log_.lastTerm()}
        });
    }
}

void RaftNode::becomeLeader() {
    role_               = RaftRole::kLeader;
    heartbeatElapsedMs_ = heartbeatIntervalMs_;  // fire immediately
    checkQuorumElapsedMs_ = 0;
    heardFrom_.clear();

    LogIndex nextIdx = log_.lastIndex() + 1;
    for (NodeId p : peers_) {
        nextIndex_[p]  = nextIdx;
        matchIndex_[p] = 0;
    }

    // Append a no-op in the new term. Committing it commits every earlier
    // entry (Raft §5.4.2 / "Figure 8") and lets the leader learn the true
    // commit index before serving reads.
    log_.append(currentTerm_, {}, /*noop=*/true);

    LOG(LogLevel::INFO, "node %u became LEADER term %llu",
        id_, (unsigned long long)currentTerm_);

    broadcastAppendEntries();
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendEntries helpers (leader side)
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::sendAppendEntries(NodeId to) {
    LogIndex next     = nextIndex_.count(to) ? nextIndex_[to] : 1u;
    LogIndex prevIdx  = next - 1;
    Term     prevTerm = log_.termAt(prevIdx);
    pending_.push_back(Message{
        id_, to,
        AppendEntriesArgs{
            currentTerm_, id_,
            prevIdx, prevTerm,
            log_.entriesFrom(next),
            log_.commitIndex()
        }
    });
}

void RaftNode::broadcastAppendEntries() {
    for (NodeId p : peers_) sendAppendEntries(p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Commit advancement (leader only, Raft §5.3-5.4)
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::maybeAdvanceCommit() {
    const size_t q = quorum();

    for (LogIndex n = log_.lastIndex(); n > log_.commitIndex(); --n) {
        // Only commit entries from the current term (Raft §5.4.2).
        if (log_.termAt(n) != currentTerm_) break;

        size_t count = 1;  // leader
        for (auto& [peer, mi] : matchIndex_)
            if (mi >= n) ++count;

        if (count >= q) {
            log_.setCommitIndex(n);
            drainApplicable();
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// tick
// ─────────────────────────────────────────────────────────────────────────────

bool RaftNode::checkQuorumHealthy() {
    const size_t reachable = heardFrom_.size() + 1;  // + self
    heardFrom_.clear();
    return reachable >= quorum();
}

std::vector<Message> RaftNode::tick(uint32_t ms) {
    pending_.clear();

    if (role_ == RaftRole::kLeader) {
        // CheckQuorum: a leader that cannot reach a majority within one
        // minimum election timeout steps down so it stops accepting writes.
        checkQuorumElapsedMs_ += ms;
        if (checkQuorumElapsedMs_ >= electionTimeoutMinMs_) {
            checkQuorumElapsedMs_ = 0;
            if (!checkQuorumHealthy()) {
                LOG(LogLevel::WARN, "node %u lost quorum contact, stepping down term %llu",
                    id_, (unsigned long long)currentTerm_);
                becomeFollower(currentTerm_);
                return std::move(pending_);
            }
        }
        heartbeatElapsedMs_ += ms;
        if (heartbeatElapsedMs_ >= heartbeatIntervalMs_) {
            heartbeatElapsedMs_ = 0;
            broadcastAppendEntries();
        }
        drainApplicable();
        return std::move(pending_);
    }

    // Follower / Candidate.
    electionElapsedMs_ += ms;
    if (electionElapsedMs_ >= electionTimeoutMs_) {
        becomeCandidate();
    }
    drainApplicable();
    return std::move(pending_);
}

// ─────────────────────────────────────────────────────────────────────────────
// step
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Message> RaftNode::step(const Message& msg) {
    pending_.clear();
    std::visit([&](auto&& args) {
        using T = std::decay_t<decltype(args)>;
        if constexpr (std::is_same_v<T, RequestVoteArgs>)
            handleRequestVote(msg, args);
        else if constexpr (std::is_same_v<T, RequestVoteReply>)
            handleRequestVoteReply(msg, args);
        else if constexpr (std::is_same_v<T, AppendEntriesArgs>)
            handleAppendEntries(msg, args);
        else
            handleAppendEntriesReply(msg, args);
    }, msg.payload);
    return std::move(pending_);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestVote handler
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::handleRequestVote(const Message& msg,
                                  const RequestVoteArgs& args) {
    if (args.term > currentTerm_) becomeFollower(args.term);

    RequestVoteReply rpl;
    rpl.term = currentTerm_;

    bool canVote = (args.term == currentTerm_) &&
                   (votedFor_ == kNone || votedFor_ == args.candidateId) &&
                   isLogUpToDate(args.lastLogIndex, args.lastLogTerm);

    if (canVote) {
        votedFor_       = args.candidateId;
        rpl.voteGranted = true;
        resetElectionTimeout();
        LOG(LogLevel::INFO, "node %u voted for %u term %llu",
            id_, args.candidateId, (unsigned long long)args.term);
    }

    pending_.push_back(Message{id_, msg.from, rpl});
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestVoteReply handler
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::handleRequestVoteReply(const Message& msg,
                                       const RequestVoteReply& rpl) {
    if (rpl.term > currentTerm_) { becomeFollower(rpl.term); return; }
    if (role_ != RaftRole::kCandidate || rpl.term != currentTerm_) return;
    if (!rpl.voteGranted) return;

    votesGranted_.insert(msg.from);
    if (votesGranted_.size() >= quorum()) becomeLeader();
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendEntries handler (follower side)
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::handleAppendEntries(const Message& msg,
                                    const AppendEntriesArgs& args) {
    AppendEntriesReply rpl;
    rpl.followerId = id_;
    rpl.term       = currentTerm_;

    if (args.term < currentTerm_) {
        // Stale leader – reject.
        pending_.push_back(Message{id_, msg.from, rpl});
        return;
    }

    // Valid leader: reset our election timer and ensure we're a follower.
    becomeFollower(args.term);
    rpl.term = currentTerm_;

    if (!log_.tryAppend(args.prevLogIndex, args.prevLogTerm, args.entries)) {
        rpl.success       = false;
        rpl.conflictIndex = log_.lastIndex() + 1;
        rpl.conflictTerm  = 0;
        pending_.push_back(Message{id_, msg.from, rpl});
        return;
    }

    log_.setCommitIndex(args.leaderCommit);
    drainApplicable();

    rpl.success    = true;
    rpl.matchIndex = log_.lastIndex();
    pending_.push_back(Message{id_, msg.from, rpl});
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendEntriesReply handler (leader side)
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::handleAppendEntriesReply(const Message& msg,
                                         const AppendEntriesReply& rpl) {
    if (rpl.term > currentTerm_) { becomeFollower(rpl.term); return; }
    if (role_ != RaftRole::kLeader || rpl.term != currentTerm_) return;

    NodeId f = msg.from;
    heardFrom_.insert(f);  // any same-term reply proves the peer is reachable
    if (rpl.success) {
        matchIndex_[f] = std::max(matchIndex_[f], rpl.matchIndex);
        nextIndex_[f]  = matchIndex_[f] + 1;
        maybeAdvanceCommit();
    } else {
        // Fast back-track.
        if (rpl.conflictIndex > 0)
            nextIndex_[f] = rpl.conflictIndex;
        else if (nextIndex_[f] > 1)
            nextIndex_[f]--;
        // Retry immediately.
        sendAppendEntries(f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// propose / takePending / takeApplied
// ─────────────────────────────────────────────────────────────────────────────

bool RaftNode::propose(std::vector<uint8_t> data) {
    if (!isLeader()) return false;
    log_.append(currentTerm_, std::move(data));
    pending_.clear();
    broadcastAppendEntries();
    return true;
}

std::vector<Message> RaftNode::takePending() {
    return std::move(pending_);
}

std::vector<LogEntry> RaftNode::takeApplied() {
    return std::move(applied_);
}

}  // namespace aegis
