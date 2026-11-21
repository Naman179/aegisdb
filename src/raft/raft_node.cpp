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
        leaderId_    = kNone;
    }
    role_ = RaftRole::kFollower;
    pendingReads_.clear();  // unconfirmed reads must be retried on the new leader
    resetElectionTimeout();
}

// PreVote (Raft thesis §9.6): probe whether a real election could succeed
// without touching currentTerm_ or votedFor_. A node that is partitioned away
// never gathers a majority of pre-votes, so its term cannot inflate and it
// cannot force a healthy leader to step down when it rejoins.
void RaftNode::startPreVote() {
    role_     = RaftRole::kPreCandidate;
    leaderId_ = kNone;
    votesGranted_.clear();
    votesGranted_.insert(id_);
    resetElectionTimeout();

    if (votesGranted_.size() >= quorum()) { becomeCandidate(); return; }

    LOG(LogLevel::INFO, "node %u starting pre-vote for term %llu",
        id_, (unsigned long long)(currentTerm_ + 1));

    for (NodeId p : peers_) {
        RequestVoteArgs a{currentTerm_ + 1, id_, log_.lastIndex(), log_.lastTerm()};
        a.preVote = true;
        pending_.push_back(Message{id_, p, a});
    }
}

void RaftNode::becomeCandidate() {
    role_         = RaftRole::kCandidate;
    leaderId_     = kNone;
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
    leaderId_           = id_;
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
    maybeAdvanceCommit();  // single-node cluster: own copy is already a quorum

    LOG(LogLevel::INFO, "node %u became LEADER term %llu",
        id_, (unsigned long long)currentTerm_);

    broadcastAppendEntries();
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendEntries helpers (leader side)
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::sendAppendEntries(NodeId to, uint64_t readCtx) {
    LogIndex next     = nextIndex_.count(to) ? nextIndex_[to] : 1u;
    if (next <= log_.snapshotIndex() && !snapshotData_.empty()) {
        // The entries this follower needs are compacted away: ship the snapshot.
        pending_.push_back(Message{id_, to, InstallSnapshotArgs{
            currentTerm_, id_, log_.snapshotIndex(), log_.snapshotTerm(), snapshotData_}});
        return;
    }
    LogIndex prevIdx  = next - 1;
    Term     prevTerm = log_.termAt(prevIdx);
    pending_.push_back(Message{
        id_, to,
        AppendEntriesArgs{
            currentTerm_, id_,
            prevIdx, prevTerm,
            log_.entriesFrom(next),
            log_.commitIndex(),
            readCtx
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
        startPreVote();
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
        else if constexpr (std::is_same_v<T, AppendEntriesReply>)
            handleAppendEntriesReply(msg, args);
        else if constexpr (std::is_same_v<T, InstallSnapshotArgs>)
            handleInstallSnapshot(msg, args);
        else
            handleInstallSnapshotReply(msg, args);
    }, msg.payload);
    return std::move(pending_);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestVote handler
// ─────────────────────────────────────────────────────────────────────────────

void RaftNode::handlePreVote(const Message& msg, const RequestVoteArgs& args) {
    RequestVoteReply rpl;
    rpl.preVote = true;
    rpl.term    = currentTerm_;

    // Leader stickiness: refuse while we are leader or have recently heard
    // from one (the election timer is reset on every valid leader message).
    const bool sticky = role_ == RaftRole::kLeader ||
                        (leaderId_ != kNone &&
                         electionElapsedMs_ < electionTimeoutMinMs_);
    if (args.term > currentTerm_ && !sticky &&
        isLogUpToDate(args.lastLogIndex, args.lastLogTerm)) {
        rpl.voteGranted = true;
        rpl.term        = args.term;  // echo the probed term
    }
    pending_.push_back(Message{id_, msg.from, rpl});
}

void RaftNode::handleRequestVote(const Message& msg,
                                  const RequestVoteArgs& args) {
    if (args.preVote) { handlePreVote(msg, args); return; }
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
    if (rpl.preVote) {
        if (role_ != RaftRole::kPreCandidate) return;
        if (!rpl.voteGranted) {
            if (rpl.term > currentTerm_) becomeFollower(rpl.term);
            return;
        }
        if (rpl.term != currentTerm_ + 1) return;  // stale probe
        votesGranted_.insert(msg.from);
        if (votesGranted_.size() >= quorum()) becomeCandidate();
        return;
    }
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
    rpl.readCtx    = args.readCtx;

    if (args.term < currentTerm_) {
        // Stale leader – reject.
        pending_.push_back(Message{id_, msg.from, rpl});
        return;
    }

    // Valid leader: reset our election timer and ensure we're a follower.
    becomeFollower(args.term);
    leaderId_ = args.leaderId;
    rpl.term  = currentTerm_;

    if (!log_.tryAppend(args.prevLogIndex, args.prevLogTerm, args.entries)) {
        rpl.success = false;
        if (args.prevLogIndex > log_.lastIndex()) {
            // Log too short: leader should resume right after our last entry.
            rpl.conflictIndex = log_.lastIndex() + 1;
            rpl.conflictTerm  = 0;
        } else {
            // Term mismatch at prevLogIndex: report the whole conflicting term
            // so the leader can skip it in one round trip.
            rpl.conflictTerm  = log_.termAt(args.prevLogIndex);
            rpl.conflictIndex = log_.firstIndexOfTerm(args.prevLogIndex);
        }
        pending_.push_back(Message{id_, msg.from, rpl});
        return;
    }

    // Only entries the leader has just verified match its log may be committed;
    // our own tail beyond them could still be a divergent suffix.
    const LogIndex lastNew = args.prevLogIndex + args.entries.size();
    log_.setCommitIndex(std::min(args.leaderCommit, lastNew));
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
    if (rpl.readCtx != 0) ackReads(f, rpl.readCtx);
    if (rpl.success) {
        matchIndex_[f] = std::max(matchIndex_[f], rpl.matchIndex);
        nextIndex_[f]  = matchIndex_[f] + 1;
        maybeAdvanceCommit();
    } else {
        // Fast back-track.
        LogIndex next = nextIndex_[f];
        if (rpl.conflictTerm != 0) {
            // If we hold entries of the follower's conflicting term, resume
            // just after our last one; otherwise skip the entire term.
            const LogIndex ours = log_.lastIndexOfTerm(rpl.conflictTerm);
            next = ours != 0 ? ours + 1 : rpl.conflictIndex;
        } else if (rpl.conflictIndex > 0) {
            next = rpl.conflictIndex;
        } else if (next > 1) {
            --next;
        }
        nextIndex_[f] = std::max<LogIndex>(1, next);
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
    maybeAdvanceCommit();  // single-node cluster commits without any replies
    pending_.clear();
    broadcastAppendEntries();
    return true;
}

bool RaftNode::compact(LogIndex index, std::vector<uint8_t> snapshot) {
    if (!log_.compactTo(index)) return false;
    snapshotData_ = std::move(snapshot);
    return true;
}

std::optional<InstalledSnapshot> RaftNode::takeInstalledSnapshot() {
    auto out = std::move(installedSnapshot_);
    installedSnapshot_.reset();
    return out;
}

void RaftNode::handleInstallSnapshot(const Message& msg,
                                     const InstallSnapshotArgs& args) {
    InstallSnapshotReply rpl;
    rpl.followerId = id_;
    rpl.term       = currentTerm_;
    if (args.term < currentTerm_) {
        pending_.push_back(Message{id_, msg.from, rpl});
        return;
    }
    becomeFollower(args.term);
    leaderId_ = args.leaderId;
    rpl.term  = currentTerm_;

    if (args.lastIncludedIndex <= log_.commitIndex()) {
        // We are already past this snapshot; just report how far we are.
        rpl.lastIncludedIndex = log_.commitIndex();
    } else {
        log_.installSnapshot(args.lastIncludedIndex, args.lastIncludedTerm);
        snapshotData_      = args.data;
        installedSnapshot_ = InstalledSnapshot{args.lastIncludedIndex,
                                               args.lastIncludedTerm, args.data};
        rpl.lastIncludedIndex = args.lastIncludedIndex;
        LOG(LogLevel::INFO, "node %u installed snapshot through index %llu",
            id_, (unsigned long long)args.lastIncludedIndex);
    }
    pending_.push_back(Message{id_, msg.from, rpl});
}

void RaftNode::handleInstallSnapshotReply(const Message& msg,
                                          const InstallSnapshotReply& rpl) {
    if (rpl.term > currentTerm_) { becomeFollower(rpl.term); return; }
    if (role_ != RaftRole::kLeader || rpl.term != currentTerm_) return;

    NodeId f = msg.from;
    heardFrom_.insert(f);
    matchIndex_[f] = std::max(matchIndex_[f], rpl.lastIncludedIndex);
    nextIndex_[f]  = matchIndex_[f] + 1;
    maybeAdvanceCommit();
    sendAppendEntries(f);  // stream whatever follows the snapshot
}

uint64_t RaftNode::requestRead() {
    if (!isLeader()) return 0;
    // Until we commit an entry from our own term our commit index may lag
    // what previous leaders already committed (hence the election no-op).
    if (log_.termAt(log_.commitIndex()) != currentTerm_) return 0;

    PendingRead pr{++readSeq_, log_.commitIndex(), {id_}};
    if (pr.acks.size() >= quorum()) {  // single-node cluster
        readStates_.push_back(ReadState{pr.id, pr.index});
        return pr.id;
    }
    for (NodeId p : peers_) sendAppendEntries(p, pr.id);
    pendingReads_.push_back(std::move(pr));
    return readSeq_;
}

void RaftNode::ackReads(NodeId from, uint64_t ctx) {
    for (auto& pr : pendingReads_)
        if (pr.id <= ctx) pr.acks.insert(from);  // a later round also confirms earlier ones

    auto it = pendingReads_.begin();
    while (it != pendingReads_.end()) {
        if (it->acks.size() >= quorum()) {
            readStates_.push_back(ReadState{it->id, it->index});
            it = pendingReads_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<ReadState> RaftNode::takeReadStates() {
    return std::move(readStates_);
}

std::vector<Message> RaftNode::takePending() {
    return std::move(pending_);
}

std::vector<LogEntry> RaftNode::takeApplied() {
    return std::move(applied_);
}

}  // namespace aegis
