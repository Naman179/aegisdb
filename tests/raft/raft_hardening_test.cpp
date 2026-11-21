// raft_hardening_test.cpp – Milestone 5: PreVote, CheckQuorum, no-op,
// fast backtracking, commit safety and ReadIndex.
#include "raft/raft_node.h"
#include "fake_network.h"

#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

std::vector<uint8_t> bytes(const char* s) {
    std::string str(s);
    return {str.begin(), str.end()};
}

LogEntry entry(Term t, LogIndex i, const char* d = "x") {
    return LogEntry{t, i, bytes(d), false};
}

// Run the cluster for `ms` virtual milliseconds in 10 ms steps.
void run(FakeNetwork& net, uint32_t ms) {
    for (uint32_t e = 0; e < ms; e += 10) net.tick(10, 10);
}

// Pick `n` follower ids (not the leader) from the cluster.
std::vector<NodeId> followers(FakeNetwork& net, const std::vector<NodeId>& all,
                              size_t n) {
    std::vector<NodeId> out;
    for (NodeId id : all) {
        if (out.size() == n) break;
        if (id != net.leader()->id()) out.push_back(id);
    }
    return out;
}

// ── Leader no-op ─────────────────────────────────────────────────────────────

TEST(RaftHardening, LeaderAppendsNoOpAndCommitsIt) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 100);
    auto* ldr = net.leader();
    ASSERT_NE(ldr, nullptr);
    EXPECT_GE(ldr->lastLogIndex(), 1u);
    EXPECT_GE(ldr->commitIndex(), 1u) << "no-op commits once a majority has it";
    EXPECT_TRUE(ldr->takeApplied().empty()) << "no-ops never reach the state machine";
}

// ── PreVote ──────────────────────────────────────────────────────────────────

TEST(RaftHardening, PreVoteDoesNotInflateIsolatedNodeTerm) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    NodeId victim = followers(net, {1, 2, 3}, 1)[0];
    Term before = net.node(victim).currentTerm();

    net.isolate(victim);
    run(net, 3000);  // many election timeouts

    EXPECT_EQ(net.node(victim).currentTerm(), before);
    EXPECT_EQ(net.node(victim).role(), RaftRole::kPreCandidate);
}

TEST(RaftHardening, PreVoteBlocksDisruptiveRejoin) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    NodeId ldrId = net.leader()->id();
    Term   term  = net.leader()->currentTerm();
    NodeId victim = followers(net, {1, 2, 3}, 1)[0];

    net.isolate(victim);
    run(net, 2000);
    net.heal(victim);
    run(net, 500);

    ASSERT_NE(net.leader(), nullptr);
    EXPECT_EQ(net.leader()->id(), ldrId) << "rejoining node must not unseat the leader";
    EXPECT_EQ(net.leader()->currentTerm(), term);
}

TEST(RaftHardening, PreVoteDeniedWhileLeaderIsActive) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 50);
    NodeId f = followers(net, {1, 2, 3}, 1)[0];

    RequestVoteArgs a{net.node(f).currentTerm() + 1, 99, 1000, 1000};
    a.preVote = true;
    auto out = net.node(f).step(Message{99, f, a});
    ASSERT_EQ(out.size(), 1u);
    auto& rpl = std::get<RequestVoteReply>(out[0].payload);
    EXPECT_TRUE(rpl.preVote);
    EXPECT_FALSE(rpl.voteGranted);
}

// ── CheckQuorum ──────────────────────────────────────────────────────────────

TEST(RaftHardening, CheckQuorumStepsIsolatedLeaderDown) {
    FakeNetwork net({1, 2, 3, 4, 5});
    ASSERT_TRUE(net.tickUntilLeader(1500));
    run(net, 100);
    NodeId ldrId = net.leader()->id();

    net.isolate(ldrId);
    bool stepped = false;
    for (int ms = 0; ms < 400 && !stepped; ms += 10) {
        net.tick(10, 10);
        stepped = !net.node(ldrId).isLeader();
    }
    EXPECT_TRUE(stepped) << "leader must step down within ~one election timeout";
}

TEST(RaftHardening, CheckQuorumKeepsLeaderWithMajority) {
    FakeNetwork net({1, 2, 3, 4, 5});
    ASSERT_TRUE(net.tickUntilLeader(1500));
    run(net, 100);
    NodeId ldrId = net.leader()->id();
    for (NodeId f : followers(net, {1, 2, 3, 4, 5}, 2)) net.isolate(f);

    run(net, 1000);
    EXPECT_TRUE(net.node(ldrId).isLeader()) << "3 of 5 is still a quorum";
}

// ── Availability / quorum ────────────────────────────────────────────────────

TEST(RaftHardening, FailoverUnder500ms) {
    FakeNetwork net({1, 2, 3, 4, 5});
    ASSERT_TRUE(net.tickUntilLeader(1500));
    run(net, 200);
    NodeId old = net.leader()->id();

    net.isolate(old);
    uint32_t elapsed = 0;
    while (net.leader() == nullptr && elapsed < 2000) { net.tick(10, 10); elapsed += 10; }

    ASSERT_NE(net.leader(), nullptr);
    EXPECT_NE(net.leader()->id(), old);
    EXPECT_LT(elapsed, 500u) << "failover took " << elapsed << " ms";
}

TEST(RaftHardening, AgreementWithTwoFollowersDown) {
    FakeNetwork net({1, 2, 3, 4, 5});
    ASSERT_TRUE(net.tickUntilLeader(1500));
    run(net, 100);
    for (NodeId f : followers(net, {1, 2, 3, 4, 5}, 2)) net.isolate(f);

    auto* ldr = net.leader();
    ASSERT_NE(ldr, nullptr);
    LogIndex before = ldr->commitIndex();
    ASSERT_TRUE(ldr->propose(bytes("two-down")));
    net.deliver(ldr->takePending(), 10);
    run(net, 100);
    EXPECT_GT(ldr->commitIndex(), before);
}

TEST(RaftHardening, NoCommitWithoutQuorum) {
    FakeNetwork net({1, 2, 3, 4, 5});
    ASSERT_TRUE(net.tickUntilLeader(1500));
    run(net, 100);
    auto* ldr = net.leader();
    for (NodeId f : followers(net, {1, 2, 3, 4, 5}, 3)) net.isolate(f);

    LogIndex before = ldr->commitIndex();
    ASSERT_TRUE(ldr->propose(bytes("no-quorum")));
    net.deliver(ldr->takePending(), 10);
    run(net, 100);  // shorter than CheckQuorum window: still leader, still stuck
    EXPECT_EQ(ldr->commitIndex(), before);
}

// ── Log repair ───────────────────────────────────────────────────────────────

TEST(RaftHardening, FastBacktrackingReportsConflictTerm) {
    RaftNode f(2, {1}, 150, 300, 50, 0);
    // Follower log: term 1 at indices 1-3, term 2 at index 4.
    ASSERT_FALSE(f.step(Message{1, 2, AppendEntriesArgs{
        2, 1, 0, 0, {entry(1,1), entry(1,2), entry(1,3), entry(2,4)}, 0}}).empty());

    // Leader (term 3) probes prev=4 with term 3 -> mismatch with our term 2.
    auto out = f.step(Message{1, 2, AppendEntriesArgs{3, 1, 4, 3, {}, 0}});
    ASSERT_EQ(out.size(), 1u);
    auto& rpl = std::get<AppendEntriesReply>(out[0].payload);
    EXPECT_FALSE(rpl.success);
    EXPECT_EQ(rpl.conflictTerm, 2u);
    EXPECT_EQ(rpl.conflictIndex, 4u) << "first index of the conflicting term";

    // Probe beyond our log: ask leader to resume right after our last entry.
    out = f.step(Message{1, 2, AppendEntriesArgs{3, 1, 9, 3, {}, 0}});
    auto& rpl2 = std::get<AppendEntriesReply>(out[0].payload);
    EXPECT_FALSE(rpl2.success);
    EXPECT_EQ(rpl2.conflictTerm, 0u);
    EXPECT_EQ(rpl2.conflictIndex, 5u);
}

TEST(RaftHardening, FollowerCommitCappedAtLastVerifiedEntry) {
    RaftNode f(2, {1}, 150, 300, 50, 0);
    // Stale tail in term 1 that the new leader never verified.
    f.step(Message{1, 2, AppendEntriesArgs{
        1, 1, 0, 0, {entry(1,1), entry(1,2), entry(1,3), entry(1,4)}, 0}});

    // New leader's heartbeat only verifies index 1, but its commit index is 4.
    f.step(Message{1, 2, AppendEntriesArgs{2, 1, 1, 1, {}, 4}});
    EXPECT_EQ(f.commitIndex(), 1u) << "indices 2-4 are unverified and may be divergent";
}

TEST(RaftHardening, DivergentFollowerConvergesToLeaderLog) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 100);
    NodeId lag = followers(net, {1, 2, 3}, 1)[0];
    net.isolate(lag);

    auto* ldr = net.leader();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ldr->propose(bytes("v")));
        net.deliver(ldr->takePending(), 10);
    }
    run(net, 100);
    net.heal(lag);
    run(net, 500);
    EXPECT_EQ(net.node(lag).lastLogIndex(), ldr->lastLogIndex());
    EXPECT_EQ(net.node(lag).commitIndex(), ldr->commitIndex());
}

// ── ReadIndex ────────────────────────────────────────────────────────────────

TEST(RaftHardening, ReadIndexConfirmedByQuorum) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 100);
    auto* ldr = net.leader();
    ASSERT_TRUE(ldr->propose(bytes("w")));
    net.deliver(ldr->takePending(), 10);
    run(net, 60);

    uint64_t id = ldr->requestRead();
    ASSERT_NE(id, 0u);
    net.deliver(ldr->takePending(), 10);

    auto states = ldr->takeReadStates();
    ASSERT_EQ(states.size(), 1u);
    EXPECT_EQ(states[0].id, id);
    EXPECT_GE(states[0].index, 2u);  // no-op + the write
    EXPECT_LE(states[0].index, ldr->commitIndex());
}

TEST(RaftHardening, ReadIndexRefusedOnFollower) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    NodeId f = followers(net, {1, 2, 3}, 1)[0];
    EXPECT_EQ(net.node(f).requestRead(), 0u);
}

TEST(RaftHardening, ReadIndexNeverConfirmedForIsolatedLeader) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 100);
    NodeId ldrId = net.leader()->id();
    auto& ldr = net.node(ldrId);
    net.isolate(ldrId);

    ASSERT_NE(ldr.requestRead(), 0u);
    net.deliver(ldr.takePending(), 10);  // everything is dropped
    EXPECT_TRUE(ldr.takeReadStates().empty());
}

TEST(RaftHardening, SingleNodeReadIsImmediate) {
    FakeNetwork net({1});
    ASSERT_TRUE(net.tickUntilLeader(500));
    run(net, 50);
    auto& n = net.node(1);
    uint64_t id = n.requestRead();
    ASSERT_NE(id, 0u);
    EXPECT_EQ(n.takeReadStates().size(), 1u);
}

}  // namespace
}  // namespace aegis
