// snapshot_test.cpp – Milestone 6: log compaction and InstallSnapshot
#include "raft/raft_log.h"
#include "raft/raft_node.h"
#include "fake_network.h"

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

}  // namespace
}  // namespace aegis
