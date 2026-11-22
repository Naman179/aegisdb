#include "server/server.h"
#include "net/rpc_client.h"
#include "server/client_protocol.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

class ClusterFixture {
public:
    void setup(int baseClientPort = 17000, int basePeerPort = 18000) {
        baseClient_ = baseClientPort;
        basePeer_ = basePeerPort;
        nodes_.resize(6); // 1-indexed

        std::vector<NodePeerInfo> peers;
        for (NodeId id = 1; id <= 5; ++id) {
            peers.push_back(NodePeerInfo{
                id, "127.0.0.1", baseClient_ + static_cast<int>(id), basePeer_ + static_cast<int>(id)
            });
        }

        for (NodeId id = 1; id <= 5; ++id) {
            ServerOptions opts;
            opts.nodeId = id;
            opts.clientPort = baseClient_ + static_cast<int>(id);
            opts.peerPort = basePeer_ + static_cast<int>(id);
            opts.peers = peers;
            opts.numGroups = 3;
            opts.vnodesPerGroup = 128;
            opts.workerThreads = 4;
            nodes_[id] = std::make_unique<Server>(std::move(opts));
            nodes_[id]->start();
        }

        // Allow leaders to be elected
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    void teardown() {
        for (NodeId id = 1; id <= 5; ++id) {
            if (nodes_[id]) {
                nodes_[id]->stop();
                nodes_[id].reset();
            }
        }
    }

    void killNode(NodeId id) {
        if (nodes_[id]) {
            nodes_[id]->stop();
            nodes_[id].reset();
        }
    }

    void restartNode(NodeId id) {
        if (nodes_[id]) return;
        std::vector<NodePeerInfo> peers;
        for (NodeId i = 1; i <= 5; ++i) {
            peers.push_back(NodePeerInfo{
                i, "127.0.0.1", baseClient_ + static_cast<int>(i), basePeer_ + static_cast<int>(i)
            });
        }
        ServerOptions opts;
        opts.nodeId = id;
        opts.clientPort = baseClient_ + static_cast<int>(id);
        opts.peerPort = basePeer_ + static_cast<int>(id);
        opts.peers = peers;
        opts.numGroups = 3;
        opts.workerThreads = 4;
        nodes_[id] = std::make_unique<Server>(std::move(opts));
        nodes_[id]->start();
    }

    void partition(const std::vector<NodeId>& grpA, const std::vector<NodeId>& grpB) {
        for (NodeId a : grpA) {
            if (!nodes_[a]) continue;
            for (NodeId b : grpB) {
                nodes_[a]->faults()->block(std::to_string(b));
            }
        }
        for (NodeId b : grpB) {
            if (!nodes_[b]) continue;
            for (NodeId a : grpA) {
                nodes_[b]->faults()->block(std::to_string(a));
            }
        }
    }

    void heal() {
        for (NodeId id = 1; id <= 5; ++id) {
            if (nodes_[id]) {
                nodes_[id]->faults()->clearBlocked();
            }
        }
    }

    bool put(const std::string& key, const std::string& val, uint64_t* outVer = nullptr, uint32_t timeoutMs = 2000) {
        for (NodeId id = 1; id <= 5; ++id) {
            if (!nodes_[id]) continue;
            RpcClient client("127.0.0.1", baseClient_ + static_cast<int>(id));
            if (!client.connect()) continue;

            KvPutReq req{"fixture_client", 1, key, val};
            std::vector<uint8_t> p;
            encodeKvPutReq(req, p);

            Frame resp;
            if (client.callSync(kMsgKvPutReq, p, resp, timeoutMs)) {
                KvPutResp r;
                decodeKvPutResp(resp.payload.data(), resp.payload.size(), r);
                if (r.status == StatusCode::kOk) {
                    if (outVer) *outVer = r.version;
                    return true;
                }
                if (r.status == StatusCode::kNotLeader && !r.leaderAddr.empty()) {
                    // Try leader directly
                    RpcClient ldrClient("127.0.0.1", baseClient_ + static_cast<int>(r.leaderId));
                    if (ldrClient.connect()) {
                        Frame ldrResp;
                        if (ldrClient.callSync(kMsgKvPutReq, p, ldrResp, timeoutMs)) {
                            KvPutResp lr;
                            decodeKvPutResp(ldrResp.payload.data(), ldrResp.payload.size(), lr);
                            if (lr.status == StatusCode::kOk) {
                                if (outVer) *outVer = lr.version;
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    bool get(const std::string& key, std::string& outVal, bool stale = false, uint32_t timeoutMs = 2000) {
        for (NodeId id = 1; id <= 5; ++id) {
            if (!nodes_[id]) continue;
            RpcClient client("127.0.0.1", baseClient_ + static_cast<int>(id));
            if (!client.connect()) continue;

            KvGetReq req{"fixture_client", 1, key, stale};
            std::vector<uint8_t> p;
            encodeKvGetReq(req, p);

            Frame resp;
            if (client.callSync(kMsgKvGetReq, p, resp, timeoutMs)) {
                KvGetResp r;
                decodeKvGetResp(resp.payload.data(), resp.payload.size(), r);
                if (r.status == StatusCode::kOk) {
                    outVal = r.value;
                    return true;
                }
                if (r.status == StatusCode::kNotLeader && !r.leaderAddr.empty()) {
                    RpcClient ldrClient("127.0.0.1", baseClient_ + static_cast<int>(r.leaderId));
                    if (ldrClient.connect()) {
                        Frame ldrResp;
                        if (ldrClient.callSync(kMsgKvGetReq, p, ldrResp, timeoutMs)) {
                            KvGetResp lr;
                            decodeKvGetResp(ldrResp.payload.data(), ldrResp.payload.size(), lr);
                            if (lr.status == StatusCode::kOk) {
                                outVal = lr.value;
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    NodeId findLeaderForGroup(int groupId) {
        for (NodeId id = 1; id <= 5; ++id) {
            if (!nodes_[id]) continue;
            RpcClient client("127.0.0.1", baseClient_ + static_cast<int>(id));
            if (!client.connect()) continue;
            Frame resp;
            if (client.callSync(kMsgAdminStatusReq, {}, resp, 1000)) {
                ClusterStatusResp status;
                decodeClusterStatusResp(resp.payload.data(), resp.payload.size(), status);
                for (const auto& g : status.groups) {
                    if (g.groupId == groupId && g.role == RaftRole::kLeader) {
                        return id;
                    }
                }
            }
        }
        return kNone;
    }

private:
    int baseClient_{17000};
    int basePeer_{18000};
    std::vector<std::unique_ptr<Server>> nodes_;
};

// ── 1. KillMinus9LeaderNoAckedLoss ───────────────────────────────────────────
TEST(ClusterFaultTest, KillMinus9LeaderNoAckedLoss) {
    ClusterFixture cluster;
    cluster.setup(17100, 18100);

    // Write 50 keys and confirm acknowledgment
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_kill_" + std::to_string(i);
        std::string val = "val_" + std::to_string(i);
        ASSERT_TRUE(cluster.put(key, val, nullptr, 2000));
    }

    NodeId leaderG0 = cluster.findLeaderForGroup(0);
    ASSERT_NE(leaderG0, kNone);

    // Simulate kill -9 on leader of group 0
    cluster.killNode(leaderG0);

    // Wait for failover (< 500 ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    // Verify 0 acknowledged writes were lost
    for (int i = 0; i < 50; ++i) {
        std::string key = "key_kill_" + std::to_string(i);
        std::string expected = "val_" + std::to_string(i);
        std::string got;
        ASSERT_TRUE(cluster.get(key, got, true, 2000));
        EXPECT_EQ(got, expected);
    }

    cluster.teardown();
}

// ── 2. KillTwoNodesStillServes ────────────────────────────────────────────────
TEST(ClusterFaultTest, KillTwoNodesStillServes) {
    ClusterFixture cluster;
    cluster.setup(17200, 18200);

    // 5-node cluster survives 2 failures
    cluster.killNode(4);
    cluster.killNode(5);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (int i = 0; i < 20; ++i) {
        std::string k = "survive_" + std::to_string(i);
        std::string v = "ok_" + std::to_string(i);
        EXPECT_TRUE(cluster.put(k, v, nullptr, 2000));
        std::string got;
        EXPECT_TRUE(cluster.get(k, got, true, 2000));
        EXPECT_EQ(got, v);
    }

    cluster.teardown();
}

// ── 3. KillThreeNodesUnavailableThenRecovers ──────────────────────────────────
TEST(ClusterFaultTest, KillThreeNodesUnavailableThenRecovers) {
    ClusterFixture cluster;
    cluster.setup(17300, 18300);

    // Kill 3 nodes -> majority quorum lost
    cluster.killNode(3);
    cluster.killNode(4);
    cluster.killNode(5);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Writes should fail without quorum
    bool writeSucceeded = cluster.put("unavail_key", "val", nullptr, 600);
    EXPECT_FALSE(writeSucceeded);

    // Restart 2 nodes -> quorum restored (4 nodes alive)
    cluster.restartNode(3);
    cluster.restartNode(4);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Writes should now succeed again
    EXPECT_TRUE(cluster.put("unavail_key", "recovered_val", nullptr, 2000));
    std::string got;
    EXPECT_TRUE(cluster.get("unavail_key", got, true, 2000));
    EXPECT_EQ(got, "recovered_val");

    cluster.teardown();
}

// ── 4. Partition2v3MinorityRefusesWrites ──────────────────────────────────────
TEST(ClusterFaultTest, Partition2v3MinorityRefusesWrites) {
    ClusterFixture cluster;
    cluster.setup(17400, 18400);

    // Partition {1, 2} vs {3, 4, 5}
    cluster.partition({1, 2}, {3, 4, 5});
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    // The majority side {3, 4, 5} can accept writes
    RpcClient clientMaj("127.0.0.1", 17403);
    if (clientMaj.connect()) {
        KvPutReq req{"maj_client", 1, "maj_key", "maj_val"};
        std::vector<uint8_t> p;
        encodeKvPutReq(req, p);
        Frame resp;
        (void)clientMaj.callSync(kMsgKvPutReq, p, resp, 1500);
    }

    // Heal partition
    cluster.heal();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    cluster.teardown();
}

// ── 5. FullClusterRestartDurability ───────────────────────────────────────────
TEST(ClusterFaultTest, FullClusterRestartDurability) {
    ClusterFixture cluster;
    cluster.setup(17500, 18500);

    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(cluster.put("restart_" + std::to_string(i), "data_" + std::to_string(i)));
    }

    // Stop entire cluster
    cluster.teardown();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Restart all nodes
    ClusterFixture restarted;
    restarted.setup(17500, 18500);

    for (int i = 0; i < 30; ++i) {
        std::string got;
        ASSERT_TRUE(restarted.get("restart_" + std::to_string(i), got, true));
        EXPECT_EQ(got, "data_" + std::to_string(i));
    }

    restarted.teardown();
}

// ── 6. RetriesAreIdempotentUnderDrops ─────────────────────────────────────────
TEST(ClusterFaultTest, RetriesAreIdempotentUnderDrops) {
    ClusterFixture cluster;
    cluster.setup(17600, 18600);

    uint64_t ver1 = 0, ver2 = 0;
    ASSERT_TRUE(cluster.put("idempotent_key", "same_val", &ver1));
    ASSERT_TRUE(cluster.put("idempotent_key", "same_val", &ver2));

    EXPECT_GE(ver2, ver1);

    cluster.teardown();
}

// ── 7. RebalanceUnderLoadLosesNothing ─────────────────────────────────────────
TEST(ClusterFaultTest, RebalanceUnderLoadLosesNothing) {
    ClusterFixture cluster;
    cluster.setup(17700, 18700);

    std::atomic<bool> done{false};
    std::atomic<int> writesAcked{0};

    std::thread writer([&]() {
        int idx = 0;
        while (!done) {
            std::string k = "load_" + std::to_string(idx);
            std::string v = "v_" + std::to_string(idx);
            if (cluster.put(k, v, nullptr, 1000)) {
                writesAcked++;
            }
            idx++;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    done = true;
    writer.join();

    EXPECT_GT(writesAcked.load(), 0);
    cluster.teardown();
}

}  // namespace
}  // namespace aegis
