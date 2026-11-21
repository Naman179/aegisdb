// chaos_test.cpp – Milestone 7: randomised fault schedules over 23 seeds.
//
// Each seed runs a 5-node cluster for several simulated seconds while a seeded
// RNG isolates/heals nodes (never more than 2 at once), drops 10% of messages
// and proposes commands. Invariants checked:
//   * Election safety     – never two leaders in the same term.
//   * State-machine safety – any index committed anywhere always holds the
//     same entry (term + data), now and for the rest of the run (durability).
//   * Applied streams     – every node applies a prefix of one common sequence,
//     with no duplicates.
//   * Convergence         – after healing, one leader and identical logs.
#include "raft/raft_node.h"
#include "fake_network.h"

#include <map>
#include <random>
#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

class ChaosTest : public ::testing::TestWithParam<uint32_t> {};

TEST_P(ChaosTest, SafetyAndConvergenceUnderFaults) {
    const uint32_t seed = GetParam();
    const std::vector<NodeId> ids{1, 2, 3, 4, 5};
    FakeNetwork net(ids, 150, 300, 50, seed);
    net.setDropRate(0.10);
    std::mt19937 rng(seed * 104729u + 1);

    std::map<LogIndex, std::pair<Term, std::vector<uint8_t>>> committed;  // canonical log
    std::map<NodeId, std::vector<std::string>> applied;
    std::map<Term, NodeId> leaderOfTerm;
    int proposals = 0;

    auto checkInvariants = [&](int step) {
        for (NodeId id : ids) {
            const RaftNode& n = net.node(id);
            if (n.isLeader()) {
                auto [it, fresh] = leaderOfTerm.emplace(n.currentTerm(), id);
                ASSERT_TRUE(fresh || it->second == id)
                    << "two leaders in term " << n.currentTerm() << " seed " << seed
                    << " step " << step;
            }
            for (LogIndex i = 1; i <= n.commitIndex(); ++i) {
                const LogEntry* e = n.log().entryAt(i);
                ASSERT_NE(e, nullptr);
                auto [it, fresh] = committed.emplace(i, std::make_pair(e->term, e->data));
                if (!fresh) {
                    ASSERT_EQ(it->second.first, e->term)
                        << "committed index " << i << " changed term; seed " << seed
                        << " step " << step << " node " << id;
                    ASSERT_EQ(it->second.second, e->data)
                        << "committed index " << i << " changed data; seed " << seed;
                }
            }
        }
    };

    for (int step = 0; step < 400; ++step) {
        // Random topology changes (max 2 nodes isolated so a quorum can exist).
        if (rng() % 100 < 12) {
            NodeId victim = ids[rng() % ids.size()];
            size_t isolated = 0;
            for (NodeId id : ids) isolated += net.isIsolated(id);
            if (net.isIsolated(victim)) net.heal(victim);
            else if (isolated < 2)      net.isolate(victim);
        }
        // Propose on any node that believes it is leader - including stale,
        // isolated ones whose entries must later be truncated.
        std::vector<NodeId> believers;
        for (NodeId id : ids) if (net.node(id).isLeader()) believers.push_back(id);
        if (!believers.empty() && rng() % 100 < 50) {
            RaftNode& ldr = net.node(believers[rng() % believers.size()]);
            std::string cmd = "s" + std::to_string(seed) + "-" + std::to_string(proposals++);
            if (ldr.propose({cmd.begin(), cmd.end()}))
                net.deliver(ldr.takePending(), 10);
        }
        net.tick(10, 10);
        for (NodeId id : ids)
            for (auto& e : net.node(id).takeApplied())
                applied[id].emplace_back(e.data.begin(), e.data.end());

        checkInvariants(step);
        if (HasFatalFailure()) return;
    }

    // Heal everything, stop dropping, let the cluster settle.
    net.healAll();
    net.setDropRate(0.0);
    for (int i = 0; i < 300; ++i) {
        net.tick(10, 10);
        for (NodeId id : ids)
            for (auto& e : net.node(id).takeApplied())
                applied[id].emplace_back(e.data.begin(), e.data.end());
    }
    checkInvariants(-1);
    if (HasFatalFailure()) return;

    ASSERT_NE(net.leader(), nullptr) << "no single leader after healing, seed " << seed;

    // Identical logs and commit indices everywhere.
    const RaftNode& ref = net.node(ids[0]);
    for (NodeId id : ids) {
        const RaftNode& n = net.node(id);
        EXPECT_EQ(n.lastLogIndex(), ref.lastLogIndex()) << "node " << id << " seed " << seed;
        EXPECT_EQ(n.commitIndex(), ref.commitIndex()) << "node " << id << " seed " << seed;
        for (LogIndex i = 1; i <= ref.lastLogIndex(); ++i) {
            ASSERT_NE(n.log().entryAt(i), nullptr);
            EXPECT_EQ(n.log().entryAt(i)->term, ref.log().entryAt(i)->term);
            EXPECT_EQ(n.log().entryAt(i)->data, ref.log().entryAt(i)->data);
        }
    }

    // Applied streams: same sequence on every node, no duplicates.
    const auto& base = applied[ids[0]];
    std::map<std::string, int> seen;
    for (auto& s : base) EXPECT_EQ(++seen[s], 1) << "duplicate apply of " << s;
    for (NodeId id : ids) EXPECT_EQ(applied[id], base) << "node " << id << " seed " << seed;
}

INSTANTIATE_TEST_SUITE_P(Seeds, ChaosTest, ::testing::Range(0u, 23u));

}  // namespace
}  // namespace aegis
