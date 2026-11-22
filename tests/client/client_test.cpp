#include "aegis/client.h"
#include "server/server.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

class ClientSdkClusterFixture {
public:
    ClientSdkClusterFixture() = default;
    ~ClientSdkClusterFixture() {
        teardown();
        if (!dataDir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dataDir_, ec);
        }
    }

    void setup(int baseClientPort = 21000, int basePeerPort = 22000) {
        baseClient_ = baseClientPort;
        basePeer_ = basePeerPort;
        dataDir_ = "./data_sdk_test_" + std::to_string(baseClientPort);
        nodes_.resize(6);

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
            opts.dataDir = dataDir_;
            opts.peers = peers;
            opts.numGroups = 3;
            opts.vnodesPerGroup = 128;
            opts.workerThreads = 4;
            nodes_[id] = std::make_unique<Server>(std::move(opts));
            nodes_[id]->start();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }

    void teardown() {
        for (NodeId id = 1; id <= 5; ++id) {
            if (nodes_[id]) {
                nodes_[id]->stop();
                nodes_[id].reset();
            }
        }
    }

    std::vector<std::string> clientAddresses() const {
        std::vector<std::string> addrs;
        for (int i = 1; i <= 5; ++i) {
            addrs.push_back("127.0.0.1:" + std::to_string(baseClient_ + i));
        }
        return addrs;
    }

private:
    int baseClient_{21000};
    int basePeer_{22000};
    std::string dataDir_;
    std::vector<std::unique_ptr<Server>> nodes_;
};

TEST(ClientSdkTest, HashRingRoutingMatches) {
    ClientConfig cfg;
    cfg.numGroups = 3;
    cfg.vnodesPerGroup = 128;
    AegisClient client(cfg);

    HashRing ring;
    for (int g = 0; g < 3; ++g) ring.addGroup(g, 128);

    for (int i = 0; i < 100; ++i) {
        std::string k = "test_key_" + std::to_string(i);
        EXPECT_EQ(client.groupFor(k), ring.groupFor(k));
    }
}

TEST(ClientSdkTest, PutGetDelCasRoundtrip) {
    ClientSdkClusterFixture cluster;
    cluster.setup(21100, 22100);

    ClientConfig cfg;
    cfg.clusterAddrs = cluster.clientAddresses();
    cfg.clientId = "test_sdk_user";
    AegisClient client(cfg);

    // 1. Put
    uint64_t v1 = 0;
    ASSERT_TRUE(client.put("user:alice", "developer", &v1));
    EXPECT_GT(v1, 0u);

    // 2. Get
    std::string val;
    uint64_t v2 = 0;
    ASSERT_TRUE(client.get("user:alice", val, &v2));
    EXPECT_EQ(val, "developer");
    EXPECT_EQ(v2, v1);

    // 3. CAS - expected version mismatch
    EXPECT_FALSE(client.cas("user:alice", "lead", v1 + 99));

    // 4. CAS - expected version match
    uint64_t v3 = 0;
    ASSERT_TRUE(client.cas("user:alice", "lead", v1, &v3));
    EXPECT_GT(v3, v1);

    ASSERT_TRUE(client.get("user:alice", val));
    EXPECT_EQ(val, "lead");

    // 5. Delete
    ASSERT_TRUE(client.del("user:alice"));
    EXPECT_FALSE(client.get("user:alice", val));

    cluster.teardown();
}

TEST(ClientSdkTest, TransparentLeaderRedirect) {
    ClientSdkClusterFixture cluster;
    cluster.setup(21200, 22200);

    // Provide only node 5 as seed, which is likely NOT the leader for group 0
    ClientConfig cfg;
    cfg.clusterAddrs = {"127.0.0.1:21205"};
    cfg.clientId = "redirect_client";
    AegisClient client(cfg);

    // Put should automatically follow the NOT_LEADER redirect and succeed
    uint64_t v = 0;
    ASSERT_TRUE(client.put("redirect_key", "redirect_value", &v));
    EXPECT_GT(v, 0u);

    std::string val;
    ASSERT_TRUE(client.get("redirect_key", val));
    EXPECT_EQ(val, "redirect_value");

    cluster.teardown();
}

}  // namespace
}  // namespace aegis
