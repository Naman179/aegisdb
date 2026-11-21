#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// fake_network.h – In-memory Raft cluster harness for unit tests
//
// Usage:
//   FakeNetwork net({1,2,3});
//   net.tickUntilLeader(500 /*ms*/, 5 /*delivery rounds*/);
//   ASSERT_NE(net.leader(), nullptr);
//
//   net.leader()->propose({...});
//   net.deliver(net.leader()->takePending());
//   net.tickRounds(50, 5);  // let replication complete
// ─────────────────────────────────────────────────────────────────────────────
#include "raft/raft_node.h"

#include <cassert>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace aegis {

class FakeNetwork {
public:
    // Construct a cluster with the given node IDs.
    // Uses small but deterministic timeouts for fast tests.
    FakeNetwork(std::vector<NodeId> ids,
                uint32_t electionMin = 150,
                uint32_t electionMax = 300,
                uint32_t heartbeat   = 50,
                uint32_t seed        = 0)
        : rng_(seed * 7919u + 13u)
    {
        for (NodeId id : ids) {
            std::vector<NodeId> peers;
            for (NodeId other : ids)
                if (other != id) peers.push_back(other);
            nodes_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(id),
                std::forward_as_tuple(id, peers, electionMin, electionMax, heartbeat, seed));
        }
    }

    RaftNode& node(NodeId id) {
        assert(nodes_.count(id));
        return nodes_.at(id);
    }

    // ── Delivery ──────────────────────────────────────────────────────────────

    // Deliver a batch of messages (respecting partitions), collect responses,
    // and repeat for `rounds` iterations.
    void deliver(std::vector<Message> msgs, int rounds = 10) {
        for (int r = 0; r < rounds && !msgs.empty(); ++r) {
            std::vector<Message> responses;
            for (auto& m : msgs) {
                if (dropped(m)) continue;
                if (!nodes_.count(m.to)) continue;
                auto out = nodes_.at(m.to).step(m);
                for (auto& o : out)
                    if (!dropped(o)) responses.push_back(std::move(o));
            }
            msgs = std::move(responses);
        }
    }

    // ── Tick ──────────────────────────────────────────────────────────────────

    // Tick every node by `ms` milliseconds, deliver all produced messages.
    void tick(uint32_t ms, int rounds = 10) {
        std::vector<Message> all;
        for (auto& [id, n] : nodes_) {
            auto out = n.tick(ms);
            for (auto& m : out) all.push_back(std::move(m));
        }
        deliver(std::move(all), rounds);
    }

    // Tick repeatedly in steps of `stepMs` until a leader is elected or
    // `maxMs` total time has elapsed. Returns true if a leader was found.
    bool tickUntilLeader(uint32_t maxMs, uint32_t stepMs = 10) {
        for (uint32_t elapsed = 0; elapsed < maxMs; elapsed += stepMs) {
            tick(stepMs, 10);
            if (leader() != nullptr) return true;
        }
        return false;
    }

    // ── Cluster queries ───────────────────────────────────────────────────────

    // Returns the unique leader, or nullptr if none / multiple.
    RaftNode* leader() {
        RaftNode* found = nullptr;
        for (auto& [id, n] : nodes_) {
            if (partitioned_.count(id)) continue;  // isolated nodes don't count
            if (n.isLeader()) {
                if (found) return nullptr;  // multiple leaders
                found = &n;
            }
        }
        return found;
    }

    int leaderCount() {
        int c = 0;
        for (auto& [id, n] : nodes_)
            if (!partitioned_.count(id) && n.isLeader()) ++c;
        return c;
    }

    // ── Partitions ────────────────────────────────────────────────────────────

    // Drop all messages to/from `id`.
    void isolate(NodeId id)  { partitioned_.insert(id); }
    void heal(NodeId id)     { partitioned_.erase(id); }
    void healAll()            { partitioned_.clear(); }

    // Randomly drop a fraction of all messages (seeded, so reproducible).
    void setDropRate(double p) { dropRate_ = p; }
    bool isIsolated(NodeId id) const { return partitioned_.count(id) != 0; }
    std::vector<NodeId> ids() const {
        std::vector<NodeId> v;
        for (auto& [id, n] : nodes_) { (void)n; v.push_back(id); }
        return v;
    }

private:
    bool dropped(const Message& m) {
        if (partitioned_.count(m.from) || partitioned_.count(m.to)) return true;
        return dropRate_ > 0 &&
               std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < dropRate_;
    }

    std::map<NodeId, RaftNode> nodes_;
    std::set<NodeId>           partitioned_;
    std::mt19937               rng_;
    double                     dropRate_{0.0};
};

}  // namespace aegis
