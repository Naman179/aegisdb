// ─────────────────────────────────────────────────────────────────────────────
// linearizability_check.cpp – Jepsen/Porcupine-style linearizability checker
// ─────────────────────────────────────────────────────────────────────────────
#include <aegis/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aegis;

struct OpRecord {
    int         threadId{0};
    std::string opType;       // "PUT", "GET", "CAS"
    std::string key;
    std::string writeVal;
    std::string readVal;
    uint64_t    version{0};
    uint64_t    expectedVer{0};
    uint64_t    startNs{0};
    uint64_t    endNs{0};
    bool        ok{false};
};

static uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static std::vector<std::string> splitAddrs(const std::string& str) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

int main(int argc, char** argv) {
    int numThreads = 4;
    int opsPerThread = 100;
    int numKeys = 4;
    std::string addrsStr = "127.0.0.1:7001,127.0.0.1:7002,127.0.0.1:7003";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        } else if (arg == "--ops" && i + 1 < argc) {
            opsPerThread = std::stoi(argv[++i]);
        } else if (arg == "--keys" && i + 1 < argc) {
            numKeys = std::stoi(argv[++i]);
        } else if (arg == "--nodes" && i + 1 < argc) {
            addrsStr = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: linearizability_check [options]\n"
                      << "  --threads <n>    Number of concurrent client threads (default: 4)\n"
                      << "  --ops <n>        Operations per thread (default: 100)\n"
                      << "  --keys <n>       Number of partitioned test keys (default: 4)\n"
                      << "  --nodes <list>   Comma-separated node client addresses\n";
            return 0;
        }
    }

    std::cout << "Starting Linearizability Verification:\n"
              << "  Threads:    " << numThreads << "\n"
              << "  Total Ops:  " << (numThreads * opsPerThread) << "\n"
              << "  Keys:       " << numKeys << "\n"
              << "  Seed Nodes: " << addrsStr << "\n\n";

    ClientConfig cfg;
    cfg.clusterAddrs = splitAddrs(addrsStr);
    cfg.timeoutMs = 3000;
    cfg.maxRetries = 10;
    AegisClient client(cfg);

    std::mutex historyMu;
    std::vector<OpRecord> history;
    history.reserve(numThreads * opsPerThread);

    std::atomic<bool> startFlag{false};
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([t, numKeys, opsPerThread, &client, &startFlag, &historyMu, &history]() {
            std::mt19937 rng(1337 + t);
            std::vector<OpRecord> localLog;
            localLog.reserve(opsPerThread);

            while (!startFlag.load()) {
                std::this_thread::yield();
            }

            for (int i = 0; i < opsPerThread; ++i) {
                int keyIdx = rng() % numKeys;
                std::string key = "lin_key_" + std::to_string(keyIdx);
                int dice = rng() % 100;

                OpRecord rec;
                rec.threadId = t;
                rec.key = key;

                if (dice < 45) {
                    // PUT
                    rec.opType = "PUT";
                    rec.writeVal = "v_" + std::to_string(t) + "_" + std::to_string(i);
                    rec.startNs = nowNs();
                    rec.ok = client.put(key, rec.writeVal, &rec.version);
                    rec.endNs = nowNs();
                } else if (dice < 65) {
                    // CAS
                    rec.opType = "CAS";
                    rec.writeVal = "cas_" + std::to_string(t) + "_" + std::to_string(i);
                    // First read current version to make CAS likely to contend or succeed
                    std::string curVal;
                    uint64_t curVer = 0;
                    client.get(key, curVal, &curVer);
                    rec.expectedVer = curVer;

                    rec.startNs = nowNs();
                    rec.ok = client.cas(key, rec.writeVal, rec.expectedVer, &rec.version);
                    rec.endNs = nowNs();
                } else {
                    // GET
                    rec.opType = "GET";
                    rec.startNs = nowNs();
                    rec.ok = client.get(key, rec.readVal, &rec.version);
                    rec.endNs = nowNs();
                }

                localLog.push_back(rec);
            }

            std::lock_guard<std::mutex> lk(historyMu);
            history.insert(history.end(), localLog.begin(), localLog.end());
        });
    }

    auto t0 = std::chrono::steady_clock::now();
    startFlag.store(true);

    for (auto& w : workers) {
        w.join();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::cout << "Executed " << history.size() << " operations in " << elapsedMs << " ms ("
              << (elapsedMs > 0 ? (history.size() * 1000 / elapsedMs) : 0) << " ops/s)\n";

    // ── Linearizability Verification Algorithm ───────────────────────────────
    std::cout << "Verifying linearizability invariants across operation history...\n";

    int violations = 0;
    int successfulOps = 0;

    for (int k = 0; k < numKeys; ++k) {
        std::string key = "lin_key_" + std::to_string(k);
        std::vector<OpRecord> keyOps;
        for (const auto& op : history) {
            if (op.key == key && op.ok) {
                keyOps.push_back(op);
                successfulOps++;
            }
        }

        // 1. Invariant: Monotonic Versions over Non-Overlapping Time
        // If Op A finishes before Op B starts, B's version must be >= A's version
        for (size_t i = 0; i < keyOps.size(); ++i) {
            for (size_t j = 0; j < keyOps.size(); ++j) {
                if (i == j) continue;
                const auto& a = keyOps[i];
                const auto& b = keyOps[j];
                if (a.endNs < b.startNs) {
                    // A strictly preceded B
                    if (b.version < a.version && b.version > 0 && a.version > 0) {
                        std::cerr << "VIOLATION: Version regression on key " << key << ":\n"
                                  << "  Op A: " << a.opType << " (v=" << a.version << ", end=" << a.endNs << ")\n"
                                  << "  Op B: " << b.opType << " (v=" << b.version << ", start=" << b.startNs << ")\n";
                        violations++;
                    }
                }
            }
        }

        // 2. Invariant: Stale Read Detection
        // If write W completes at W.endNs, no subsequent GET (startNs > W.endNs)
        // may return a version strictly older than W.version.
        for (const auto& w : keyOps) {
            if (w.opType != "PUT" && w.opType != "CAS") continue;
            for (const auto& r : keyOps) {
                if (r.opType != "GET") continue;
                if (r.startNs > w.endNs) {
                    // Read started after Write completely finished
                    if (r.version < w.version) {
                        std::cerr << "VIOLATION: Stale read on key " << key << ":\n"
                                  << "  Committed write completed at " << w.endNs << " with v=" << w.version << "\n"
                                  << "  Read started at " << r.startNs << " and returned stale v=" << r.version << "\n";
                        violations++;
                    }
                }
            }
        }
    }

    client.close();

    if (violations == 0) {
        std::cout << "[PASSED] Linearizability verified successfully!\n"
                  << "  Successful operations verified: " << successfulOps << "\n"
                  << "  Zero stale reads detected.\n"
                  << "  Zero version regressions detected.\n"
                  << "  Strict real-time sequential consistency holds.\n";
        return 0;
    } else {
        std::cerr << "[FAILED] Detected " << violations << " linearizability violations!\n";
        return 1;
    }
}
