// raft_test.cpp – 18-case Raft unit test suite (Milestone 4)
#include "raft/raft_node.h"
#include "raft/raft_log.h"
#include "raft/raft_types.h"
#include "fake_network.h"

#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helper: bytes from string
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> bytes(const char* s) {
    return {reinterpret_cast<const uint8_t*>(s),
            reinterpret_cast<const uint8_t*>(s) + __builtin_strlen(s)};
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Single-node cluster elects itself immediately
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, SingleNodeElectsItself) {
    FakeNetwork net({1});
    ASSERT_TRUE(net.tickUntilLeader(500));
    ASSERT_NE(net.leader(), nullptr);
    EXPECT_EQ(net.leader()->id(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Three-node cluster elects exactly one leader
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, ThreeNodeClusterElectsOneLeader) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    EXPECT_EQ(net.leaderCount(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Five-node cluster elects one leader (production topology size)
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, FiveNodeClusterElectsOneLeader) {
    FakeNetwork net({1, 2, 3, 4, 5});
    ASSERT_TRUE(net.tickUntilLeader(1500));
    EXPECT_EQ(net.leaderCount(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Follower grants vote to candidate with higher term
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, FollowerGrantsVoteForHigherTerm) {
    RaftNode follower(1, {2}, 150, 300, 50, 0);
    RequestVoteArgs args{1, 2, 0, 0};
    Message req{2, 1, args};
    auto rplMsgs = follower.step(req);
    ASSERT_EQ(rplMsgs.size(), 1u);
    auto* rpl = std::get_if<RequestVoteReply>(&rplMsgs[0].payload);
    ASSERT_NE(rpl, nullptr);
    EXPECT_TRUE(rpl->voteGranted);
    EXPECT_EQ(follower.votedFor(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Follower denies vote for stale term
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, FollowerDeniesVoteForStaleTerm) {
    // Give follower a higher term first.
    RaftNode follower(1, {2}, 150, 300, 50, 0);
    // Inject a higher-term AppendEntries to bump term.
    follower.step(Message{2, 1, AppendEntriesArgs{5, 2, 0, 0, {}, 0}});

    // Now request vote with a lower term.
    auto rplMsgs = follower.step(Message{2, 1, RequestVoteArgs{3, 2, 0, 0}});
    ASSERT_EQ(rplMsgs.size(), 1u);
    auto* rpl = std::get_if<RequestVoteReply>(&rplMsgs[0].payload);
    ASSERT_NE(rpl, nullptr);
    EXPECT_FALSE(rpl->voteGranted);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Follower denies second vote in the same term
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, FollowerDeniesSecondVoteInSameTerm) {
    RaftNode follower(1, {2, 3}, 150, 300, 50, 0);

    // Vote for node 2.
    follower.step(Message{2, 1, RequestVoteArgs{1, 2, 0, 0}});
    EXPECT_EQ(follower.votedFor(), 2u);

    // Try to vote for node 3 in the same term.
    auto rplMsgs = follower.step(Message{3, 1, RequestVoteArgs{1, 3, 0, 0}});
    ASSERT_EQ(rplMsgs.size(), 1u);
    auto* rpl = std::get_if<RequestVoteReply>(&rplMsgs[0].payload);
    ASSERT_NE(rpl, nullptr);
    EXPECT_FALSE(rpl->voteGranted);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Candidate with more votes wins (majority check)
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, CandidateWinsWithMajority) {
    // Node 1 is a candidate in term 1.
    RaftNode n1(1, {2, 3}, 1, 1, 50, 0);  // 1ms timeout → starts election immediately
    auto out1 = n1.tick(5);  // timeout → PreVote phase first
    ASSERT_EQ(n1.role(), RaftRole::kPreCandidate);
    EXPECT_EQ(n1.currentTerm(), 0u) << "PreVote must not bump the term";

    // Nodes 2 and 3 are fresh followers (no known leader → they grant).
    RaftNode n2(2, {1, 3}, 300, 300, 50, 0);
    RaftNode n3(3, {1, 2}, 300, 300, 50, 0);

    // Route messages until the exchange is quiescent: PreVote → real vote.
    std::vector<Message> queue = std::move(out1);
    for (int round = 0; round < 6 && !queue.empty(); ++round) {
        std::vector<Message> next;
        for (auto& m : queue) {
            std::vector<Message> r;
            if (m.to == 1) r = n1.step(m);
            if (m.to == 2) r = n2.step(m);
            if (m.to == 3) r = n3.step(m);
            next.insert(next.end(), r.begin(), r.end());
        }
        queue = std::move(next);
    }

    EXPECT_TRUE(n1.isLeader());
    EXPECT_EQ(n1.currentTerm(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Leader sends heartbeats (empty AppendEntries) to all peers
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, LeaderSendsHeartbeats) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    auto* ldr = net.leader();

    // Tick one heartbeat period – leader should have produced messages.
    auto out = ldr->tick(50);
    int aeCount = 0;
    for (auto& m : out)
        if (std::get_if<AppendEntriesArgs>(&m.payload)) ++aeCount;
    EXPECT_EQ(aeCount, 2);  // one per peer
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Follower rejects AppendEntries with stale term
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, FollowerRejectsStaleAppendEntries) {
    RaftNode follower(1, {2}, 150, 300, 50, 0);
    // Bump follower's term.
    follower.step(Message{2, 1, AppendEntriesArgs{5, 2, 0, 0, {}, 0}});
    EXPECT_EQ(follower.currentTerm(), 5u);

    // Send AE from a stale leader.
    auto rplMsgs = follower.step(Message{2, 1, AppendEntriesArgs{3, 2, 0, 0, {}, 0}});
    ASSERT_EQ(rplMsgs.size(), 1u);
    auto* rpl = std::get_if<AppendEntriesReply>(&rplMsgs[0].payload);
    ASSERT_NE(rpl, nullptr);
    EXPECT_FALSE(rpl->success);
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Leader replicates one entry to all followers
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, LeaderReplicatesOneEntry) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));

    auto* ldr = net.leader();
    ASSERT_TRUE(ldr->propose(bytes("hello")));
    net.deliver(ldr->takePending(), 10);
    net.tick(0, 10);  // let commit propagate

    // All nodes should have the entry in their logs.
    for (NodeId id : {1u, 2u, 3u}) {
        EXPECT_GE(net.node(id).lastLogIndex(), 1u) << "node " << id;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Entry is committed after majority acknowledgement
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, EntryCommittedOnMajority) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));

    auto* ldr = net.leader();
    ASSERT_TRUE(ldr->propose(bytes("val")));
    net.deliver(ldr->takePending(), 10);
    net.tick(50, 10);  // heartbeat carries commit index

    EXPECT_GE(ldr->commitIndex(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Committed entry is delivered via takeApplied
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, CommittedEntryApplied) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));

    auto* ldr = net.leader();
    ASSERT_TRUE(ldr->propose(bytes("apply-me")));
    net.deliver(ldr->takePending(), 10);
    net.tick(50, 10);

    auto applied = ldr->takeApplied();
    ASSERT_FALSE(applied.empty());
    std::string got(applied[0].data.begin(), applied[0].data.end());
    EXPECT_EQ(got, "apply-me");
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. Higher-term message causes leader to step down
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, HigherTermCausesLeaderStepdown) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    auto* ldr = net.leader();
    NodeId ldrId = ldr->id();
    Term   ldrTerm = ldr->currentTerm();

    // Inject a RequestVote with a much higher term directly to the leader.
    ldr->step(Message{2, ldrId, RequestVoteArgs{ldrTerm + 10, 2, 0, 0}});
    EXPECT_FALSE(ldr->isLeader());
    EXPECT_EQ(ldr->currentTerm(), ldrTerm + 10);
}

// ─────────────────────────────────────────────────────────────────────────────
// 14. New election after leader is isolated (partition)
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, NewLeaderElectedAfterPartition) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    NodeId oldLeaderId = net.leader()->id();

    net.isolate(oldLeaderId);
    // Allow remaining two nodes to elect a new leader.
    bool elected = false;
    for (int i = 0; i < 100 && !elected; ++i) {
        net.tick(10, 10);
        auto* ldr = net.leader();
        if (ldr && ldr->id() != oldLeaderId) elected = true;
    }
    EXPECT_TRUE(elected);
}

// ─────────────────────────────────────────────────────────────────────────────
// 15. Log converges after healing a partition
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, LogConvergesAfterHeal) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    NodeId oldId = net.leader()->id();

    // Isolate the old leader, elect a new one, propose an entry.
    net.isolate(oldId);
    ASSERT_TRUE(net.tickUntilLeader(1000));
    auto* newLdr = net.leader();
    ASSERT_NE(newLdr, nullptr);
    ASSERT_TRUE(newLdr->propose(bytes("after-partition")));
    net.deliver(newLdr->takePending(), 10);
    net.tick(50, 10);
    LogIndex committed = newLdr->commitIndex();
    EXPECT_GE(committed, 1u);

    // Heal – old leader re-joins and log should catch up.
    net.healAll();
    for (int i = 0; i < 20; ++i) net.tick(50, 10);
    EXPECT_GE(net.node(oldId).lastLogIndex(), committed);
}

// ─────────────────────────────────────────────────────────────────────────────
// 16. Follower truncates conflicting log entries
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, FollowerTruncatesConflictingEntries) {
    RaftLog log;
    // Append entries from a stale leader (term 1).
    log.append(1, bytes("stale-a"));
    log.append(1, bytes("stale-b"));
    ASSERT_EQ(log.lastIndex(), 2u);

    // New leader (term 2) sends overlapping entries starting at prevIdx=0.
    std::vector<LogEntry> newEntries = {
        {2, 1, bytes("fresh-a")},
        {2, 2, bytes("fresh-b")},
        {2, 3, bytes("fresh-c")},
    };
    bool ok = log.tryAppend(0, 0, newEntries);
    ASSERT_TRUE(ok);
    ASSERT_EQ(log.lastIndex(), 3u);
    EXPECT_EQ(log.termAt(1), 2u);  // overwritten by fresh term
}

// ─────────────────────────────────────────────────────────────────────────────
// 17. Term never rolls back
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, TermMonotonicallyIncreases) {
    FakeNetwork net({1, 2, 3});
    Term prev = 0;
    for (int i = 0; i < 50; ++i) {
        net.tick(20, 10);
        for (NodeId id : {1u, 2u, 3u}) {
            EXPECT_GE(net.node(id).currentTerm(), prev)
                << "node " << id << " at tick " << i;
            prev = std::max(prev, net.node(id).currentTerm());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 18. propose() returns false when not leader
// ─────────────────────────────────────────────────────────────────────────────
TEST(RaftTest, ProposeFailsOnNonLeader) {
    RaftNode follower(1, {2, 3}, 300, 300, 50, 0);
    EXPECT_EQ(follower.role(), RaftRole::kFollower);
    EXPECT_FALSE(follower.propose(bytes("should-fail")));
}

}  // namespace
}  // namespace aegis
