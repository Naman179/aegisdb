// snapshot_test.cpp – Milestone 6: log compaction and InstallSnapshot
#include "raft/raft_log.h"
#include "raft/raft_node.h"
#include "fake_network.h"

#include <initializer_list>
#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Build a log of `n` term-1 entries, all committed and applied.
RaftLog appliedLog(int n) {
    RaftLog log;
    for (int i = 0; i < n; ++i) log.append(1, bytes("e" + std::to_string(i + 1)));
    log.setCommitIndex(static_cast<LogIndex>(n));
    log.takeApplicable();
    return log;
}

TEST(RaftLogSnapshot, CompactDropsPrefixButKeepsIndices) {
    RaftLog log = appliedLog(10);
    ASSERT_TRUE(log.compactTo(6));
    EXPECT_EQ(log.snapshotIndex(), 6u);
    EXPECT_EQ(log.snapshotTerm(), 1u);
    EXPECT_EQ(log.lastIndex(), 10u);
    EXPECT_EQ(log.entryAt(6), nullptr) << "compacted entries are gone";
    ASSERT_NE(log.entryAt(7), nullptr);
    EXPECT_EQ(log.entryAt(7)->index, 7u);
    EXPECT_EQ(log.termAt(6), 1u) << "sentinel keeps the snapshot term";
    EXPECT_EQ(log.entriesFrom(1).size(), 4u) << "clamped above the snapshot";
}

TEST(RaftLogSnapshot, CannotCompactBeyondApplied) {
    RaftLog log;
    for (int i = 0; i < 5; ++i) log.append(1, bytes("x"));
    log.setCommitIndex(3);
    log.takeApplicable();
    EXPECT_FALSE(log.compactTo(5));
    EXPECT_TRUE(log.compactTo(3));
}

TEST(RaftLogSnapshot, AppendContinuesAfterCompaction) {
    RaftLog log = appliedLog(5);
    ASSERT_TRUE(log.compactTo(5));
    EXPECT_EQ(log.append(2, bytes("next")), 6u);
    EXPECT_EQ(log.lastIndex(), 6u);
    EXPECT_EQ(log.lastTerm(), 2u);
}

TEST(RaftLogSnapshot, TryAppendSkipsEntriesCoveredBySnapshot) {
    RaftLog log = appliedLog(5);
    ASSERT_TRUE(log.compactTo(5));
    // Leader probes from index 2 (inside our snapshot) and sends 3..7.
    std::vector<LogEntry> ents;
    for (LogIndex i = 3; i <= 7; ++i) ents.push_back(LogEntry{1, i, bytes("n"), false});
    ASSERT_TRUE(log.tryAppend(2, 1, ents));
    EXPECT_EQ(log.lastIndex(), 7u);
}

TEST(RaftLogSnapshot, InstallSnapshotResetsLog) {
    RaftLog log = appliedLog(3);
    log.append(1, bytes("stale-4"));
    log.installSnapshot(20, 3);
    EXPECT_EQ(log.snapshotIndex(), 20u);
    EXPECT_EQ(log.lastIndex(), 20u);
    EXPECT_EQ(log.lastTerm(), 3u);
    EXPECT_EQ(log.commitIndex(), 20u);
    EXPECT_EQ(log.lastApplied(), 20u);
}

TEST(RaftLogSnapshot, InstallSnapshotKeepsMatchingSuffix) {
    RaftLog log = appliedLog(8);
    log.installSnapshot(5, 1);  // we hold index 5 with term 1 -> keep 6..8
    EXPECT_EQ(log.snapshotIndex(), 5u);
    EXPECT_EQ(log.lastIndex(), 8u);
    EXPECT_NE(log.entryAt(8), nullptr);
}

TEST(RaftLogSnapshot, FirstAndLastIndexOfTermRespectSnapshot) {
    RaftLog log;
    for (int i = 0; i < 4; ++i) log.append(1, bytes("a"));
    for (int i = 0; i < 4; ++i) log.append(2, bytes("b"));
    log.setCommitIndex(8);
    log.takeApplicable();
    ASSERT_TRUE(log.compactTo(2));
    EXPECT_EQ(log.firstIndexOfTerm(4), 3u) << "stops at the first uncompacted entry";
    EXPECT_EQ(log.firstIndexOfTerm(7), 5u);
    EXPECT_EQ(log.lastIndexOfTerm(1), 4u);
    EXPECT_EQ(log.lastIndexOfTerm(2), 8u);
    EXPECT_EQ(log.lastIndexOfTerm(9), 0u);
}

// ── Node level ───────────────────────────────────────────────────────────────

void run(FakeNetwork& net, uint32_t ms) {
    for (uint32_t e = 0; e < ms; e += 10) net.tick(10, 10);
}

NodeId someFollower(FakeNetwork& net, std::initializer_list<NodeId> ids) {
    for (NodeId id : ids) if (id != net.leader()->id()) return id;
    return kNone;
}

TEST(RaftSnapshot, LaggingFollowerGetsSnapshot) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 100);
    NodeId lag = someFollower(net, {1, 2, 3});
    net.isolate(lag);

    auto* ldr = net.leader();
    ASSERT_NE(ldr, nullptr);
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(ldr->propose(bytes("cmd" + std::to_string(i))));
        net.deliver(ldr->takePending(), 10);
    }
    run(net, 200);
    LogIndex applied = ldr->lastApplied();
    ASSERT_GE(applied, 20u);

    // Leader snapshots and drops its log prefix; the lagging node can no
    // longer be caught up with AppendEntries alone.
    ASSERT_TRUE(ldr->compact(applied, bytes("STATE@" + std::to_string(applied))));
    ASSERT_TRUE(ldr->propose(bytes("after-snapshot")));
    net.deliver(ldr->takePending(), 10);
    run(net, 100);

    net.heal(lag);
    run(net, 500);

    auto& follower = net.node(lag);
    auto snap = follower.takeInstalledSnapshot();
    ASSERT_TRUE(snap.has_value()) << "follower should have received InstallSnapshot";
    EXPECT_EQ(snap->index, applied);
    EXPECT_EQ(std::string(snap->data.begin(), snap->data.end()),
              "STATE@" + std::to_string(applied));
    EXPECT_EQ(follower.lastLogIndex(), ldr->lastLogIndex());
    EXPECT_EQ(follower.commitIndex(), ldr->commitIndex());

    // Entries after the snapshot arrive through the normal apply stream.
    auto tail = follower.takeApplied();
    ASSERT_FALSE(tail.empty());
    EXPECT_EQ(std::string(tail.back().data.begin(), tail.back().data.end()),
              "after-snapshot");
}

TEST(RaftSnapshot, StaleSnapshotIsIgnored) {
    FakeNetwork net({1, 2, 3});
    ASSERT_TRUE(net.tickUntilLeader(1000));
    run(net, 100);
    auto* ldr = net.leader();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ldr->propose(bytes("c")));
        net.deliver(ldr->takePending(), 10);
    }
    run(net, 100);

    NodeId f = someFollower(net, {1, 2, 3});
    LogIndex commit = net.node(f).commitIndex();
    ASSERT_GE(commit, 5u);

    // A snapshot covering less than we have committed must not rewind us.
    InstallSnapshotArgs old{ldr->currentTerm(), ldr->id(), 2, 1, bytes("old")};
    auto out = net.node(f).step(Message{ldr->id(), f, old});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FALSE(net.node(f).takeInstalledSnapshot().has_value());
    EXPECT_EQ(net.node(f).commitIndex(), commit);
    EXPECT_GE(std::get<InstallSnapshotReply>(out[0].payload).lastIncludedIndex, commit);
}

TEST(RaftSnapshot, CompactRefusedBeyondApplied) {
    FakeNetwork net({1});
    ASSERT_TRUE(net.tickUntilLeader(500));
    auto& n = net.node(1);
    EXPECT_FALSE(n.compact(n.lastApplied() + 5, bytes("x")));
}

}  // namespace
}  // namespace aegis
