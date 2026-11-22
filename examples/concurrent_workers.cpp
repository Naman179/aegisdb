// ─────────────────────────────────────────────────────────────────────────────
// concurrent_workers.cpp – Multi-threaded client pool example for AegisDB
// ─────────────────────────────────────────────────────────────────────────────
#include <aegis/client.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    int numThreads = 4;
    int opsPerThread = 50;
    std::string seed = "127.0.0.1:7001";
    if (argc > 1) seed = argv[1];

    std::cout << "Starting " << numThreads << " concurrent client threads ("
              << opsPerThread << " writes + reads per thread) against seed: " << seed << "\n";

    aegis::ClientConfig config;
    config.clusterAddrs = {seed};
    config.timeoutMs = 3000;
    config.maxRetries = 10;

    aegis::AegisClient client(config);

    std::atomic<int> successWrites{0};
    std::atomic<int> successReads{0};
    std::atomic<int> failedOps{0};

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([t, opsPerThread, &client, &successWrites, &successReads, &failedOps]() {
            for (int i = 0; i < opsPerThread; ++i) {
                std::string key = "item:" + std::to_string(t) + ":" + std::to_string(i);
                std::string val = "payload_data_" + std::to_string(t * 1000 + i);

                uint64_t ver = 0;
                if (client.put(key, val, &ver)) {
                    successWrites++;
                } else {
                    failedOps++;
                    continue;
                }

                std::string readBack;
                if (client.get(key, readBack) && readBack == val) {
                    successReads++;
                } else {
                    failedOps++;
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();

    std::cout << "Results in " << elapsed << " ms:\n"
              << "  Successful writes: " << successWrites.load() << "\n"
              << "  Successful reads:  " << successReads.load() << "\n"
              << "  Failed operations: " << failedOps.load() << "\n";

    return (failedOps.load() == 0) ? 0 : 1;
}
