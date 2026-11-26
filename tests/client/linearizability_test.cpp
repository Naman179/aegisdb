#include <aegis/client.h>
#include "server/server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

struct TestOpRecord {
    std::string opType;
    std::string key;
    std::string writeVal;
    std::string readVal;
    uint64_t    version{0};
    uint64_t    startNs{0};
    uint64_t    endNs{0};
    bool        ok{false};
};

static uint64_t getNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

TEST(LinearizabilityTest, ConcurrentReadsAndWritesStrictOrdering) {
    int baseClient = 25100;
    int basePeer = 26100;
    std::string dataDir = "./data_lin_test_" + std::to_string(baseClient);
    std::error_code ec;
    std::filesystem::remove_all(dataDir, ec);

    std::vector<std::unique_ptr<Server>> nodes(6);
    std::vector<NodePeerInfo> peers;
    for (NodeId id = 1; id <= 5; ++id) {
        peers.push_back(NodePeerInfo{
            id, "127.0.0.1", baseClient + static_cast<int>(id), basePeer + static_cast<int>(id)
        });
    }

    for (NodeId id = 1; id <= 5; ++id) {
        ServerOptions opts;
        opts.nodeId = id;
        opts.clientPort = baseClient + static_cast<int>(id);
        opts.peerPort = basePeer + static_cast<int>(id);
        opts.dataDir = dataDir;
        opts.peers = peers;
        opts.numGroups = 3;
        opts.vnodesPerGroup = 64;
        opts.workerThreads = 4;
        nodes[id] = std::make_unique<Server>(std::move(opts));
        nodes[id]->start();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Client Config
    ClientConfig cfg;
    for (NodeId id = 1; id <= 5; ++id) {
        cfg.clusterAddrs.push_back("127.0.0.1:" + std::to_string(baseClient + id));
    }
    cfg.timeoutMs = 2500;
    cfg.maxRetries = 10;
    AegisClient client(cfg);

    const int numThreads = 4;
    const int opsPerThread = 40;
    const int numKeys = 3;

    std::mutex historyMu;
    std::vector<TestOpRecord> history;
    history.reserve(numThreads * opsPerThread);

    std::atomic<bool> startFlag{false};
    std::vector<std::thread> workers;

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([t, opsPerThread, numKeys, &client, &startFlag, &historyMu, &history]() {
            std::mt19937 rng(42 + t);
            std::vector<TestOpRecord> localOps;

            while (!startFlag.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < opsPerThread; ++i) {
                int kIdx = rng() % numKeys;
                std::string key = "account_" + std::to_string(kIdx);
                TestOpRecord rec;
                rec.key = key;

                if ((rng() % 10) < 5) {
                    rec.opType = "PUT";
                    rec.writeVal = "val_" + std::to_string(t) + "_" + std::to_string(i);
                    rec.startNs = getNowNs();
                    rec.ok = client.put(key, rec.writeVal, &rec.version);
                    rec.endNs = getNowNs();
                } else {
                    rec.opType = "GET";
                    rec.startNs = getNowNs();
                    rec.ok = client.get(key, rec.readVal, &rec.version);
                    rec.endNs = getNowNs();
                }
                localOps.push_back(rec);
            }

            std::lock_guard<std::mutex> lk(historyMu);
            history.insert(history.end(), localOps.begin(), localOps.end());
        });
    }

    startFlag.store(true);
    for (auto& w : workers) {
        w.join();
    }

    // Verify Linearizability Invariants
    int violations = 0;
    for (int k = 0; k < numKeys; ++k) {
        std::string key = "account_" + std::to_string(k);
        std::vector<TestOpRecord> keyOps;
        for (const auto& op : history) {
            if (op.key == key && op.ok) {
                keyOps.push_back(op);
            }
        }

        // 1. Version Monotonicity across non-overlapping real-time bounds
        for (size_t i = 0; i < keyOps.size(); ++i) {
            for (size_t j = 0; j < keyOps.size(); ++j) {
                if (i == j) continue;
                if (keyOps[i].endNs < keyOps[j].startNs) {
                    if (keyOps[j].version < keyOps[i].version && keyOps[j].version > 0 && keyOps[i].version > 0) {
                        violations++;
                    }
                }
            }
        }

        // 2. Zero Stale Reads: a GET started after write W completed must not read an older version
        for (const auto& w : keyOps) {
            if (w.opType != "PUT") continue;
            for (const auto& r : keyOps) {
                if (r.opType != "GET") continue;
                if (r.startNs > w.endNs && r.version < w.version) {
                    violations++;
                }
            }
        }
    }

    EXPECT_EQ(violations, 0);

    client.close();

    for (NodeId id = 1; id <= 5; ++id) {
        if (nodes[id]) {
            nodes[id]->stop();
            nodes[id].reset();
        }
    }
    std::filesystem::remove_all(dataDir, ec);
}

}  // namespace
}  // namespace aegis
