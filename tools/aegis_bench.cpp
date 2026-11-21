// ─────────────────────────────────────────────────────────────────────────────
// aegis_bench.cpp – High-concurrency load generator & benchmark tool (Milestone 11)
//
// Measures throughput (ops/sec) and latency percentiles (p50, p90, p99, max)
// across multi-threaded client workers against an AegisDB cluster.
// ─────────────────────────────────────────────────────────────────────────────
#include "net/rpc_client.h"
#include "server/client_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aegis;

struct BenchConfig {
    int                      threads{64};
    int                      durationSec{10};
    double                   readRatio{0.5};
    size_t                   valSize{128};
    std::vector<std::string> nodes{"127.0.0.1:7001", "127.0.0.1:7002", "127.0.0.1:7003"};
};

static void printUsage() {
    std::cout << "Usage: aegis_bench [options]\n"
              << "  --threads <N>         Concurrent client threads (default: 64)\n"
              << "  --duration <sec>      Test duration in seconds (default: 10)\n"
              << "  --read-ratio <ratio>  Fraction of reads [0.0..1.0] (default: 0.5)\n"
              << "  --value-size <bytes>  Payload value size in bytes (default: 128)\n"
              << "  --nodes <endpoints>   Comma-separated host:port list\n";
}

int main(int argc, char** argv) {
    BenchConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            cfg.threads = std::stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            cfg.durationSec = std::stoi(argv[++i]);
        } else if (arg == "--read-ratio" && i + 1 < argc) {
            cfg.readRatio = std::stod(argv[++i]);
        } else if (arg == "--value-size" && i + 1 < argc) {
            cfg.valSize = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--nodes" && i + 1 < argc) {
            cfg.nodes.clear();
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (!tok.empty()) cfg.nodes.push_back(tok);
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
    }

    std::cout << "Starting AegisDB Benchmark...\n"
              << "  Threads:     " << cfg.threads << "\n"
              << "  Duration:    " << cfg.durationSec << "s\n"
              << "  Read ratio:  " << (cfg.readRatio * 100.0) << "%\n"
              << "  Value size:  " << cfg.valSize << " bytes\n"
              << "  Nodes:       " << cfg.nodes.size() << "\n\n";

    std::string valuePayload(cfg.valSize, 'x');

    std::atomic<bool> running{true};
    std::atomic<uint64_t> totalOps{0};
    std::atomic<uint64_t> successOps{0};
    std::atomic<uint64_t> failedOps{0};

    std::mutex latMu;
    std::vector<double> latenciesMs;

    auto workerFn = [&](int threadId) {
        std::mt19937 rng(42 + threadId);
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        std::uniform_int_distribution<uint64_t> keyDist(1, 100000);

        std::string targetNode = cfg.nodes[threadId % cfg.nodes.size()];
        auto pos = targetNode.find(':');
        std::string host = (pos == std::string::npos) ? targetNode : targetNode.substr(0, pos);
        int port = (pos == std::string::npos) ? 7001 : std::stoi(targetNode.substr(pos + 1));

        RpcClient client(host, port);
        client.connect();

        std::vector<double> localLats;
        localLats.reserve(10000);

        uint64_t seq = 1;
        std::string clientId = "bench_" + std::to_string(threadId);

        while (running.load(std::memory_order_relaxed)) {
            bool isRead = coin(rng) < cfg.readRatio;
            std::string key = "key:" + std::to_string(keyDist(rng));

            auto start = std::chrono::steady_clock::now();
            bool ok = false;

            if (isRead) {
                KvGetReq req{clientId, seq++, key, false};
                std::vector<uint8_t> p;
                encodeKvGetReq(req, p);
                Frame resp;
                if (client.callSync(kMsgKvGetReq, p, resp, 1000)) {
                    KvGetResp r;
                    decodeKvGetResp(resp.payload.data(), resp.payload.size(), r);
                    ok = (r.status == StatusCode::kOk || r.status == StatusCode::kKeyNotFound);
                }
            } else {
                KvPutReq req{clientId, seq++, key, valuePayload};
                std::vector<uint8_t> p;
                encodeKvPutReq(req, p);
                Frame resp;
                if (client.callSync(kMsgKvPutReq, p, resp, 1000)) {
                    KvPutResp r;
                    decodeKvPutResp(resp.payload.data(), resp.payload.size(), r);
                    ok = (r.status == StatusCode::kOk);
                }
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start).count();
            double latMs = elapsed / 1000.0;

            totalOps.fetch_add(1, std::memory_order_relaxed);
            if (ok) {
                successOps.fetch_add(1, std::memory_order_relaxed);
                localLats.push_back(latMs);
            } else {
                failedOps.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::lock_guard<std::mutex> lk(latMu);
        latenciesMs.insert(latenciesMs.end(), localLats.begin(), localLats.end());
    };

    std::vector<std::thread> workers;
    workers.reserve(cfg.threads);
    for (int i = 0; i < cfg.threads; ++i) {
        workers.emplace_back(workerFn, i);
    }

    std::this_thread::sleep_for(std::chrono::seconds(cfg.durationSec));
    running.store(false);

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    double totalSec = cfg.durationSec;
    double throughput = (totalSec > 0) ? (successOps.load() / totalSec) : 0.0;

    std::sort(latenciesMs.begin(), latenciesMs.end());
    double p50 = 0.0, p90 = 0.0, p99 = 0.0, maxLat = 0.0;
    if (!latenciesMs.empty()) {
        p50 = latenciesMs[latenciesMs.size() * 50 / 100];
        p90 = latenciesMs[latenciesMs.size() * 90 / 100];
        p99 = latenciesMs[latenciesMs.size() * 99 / 100];
        maxLat = latenciesMs.back();
    }

    std::cout << "\n================ AegisDB Benchmark Results ================\n"
              << std::fixed << std::setprecision(2)
              << "Total operations:   " << totalOps.load() << "\n"
              << "Successful ops:     " << successOps.load() << "\n"
              << "Failed ops:         " << failedOps.load() << "\n"
              << "Throughput:         " << throughput << " ops/sec\n"
              << "-----------------------------------------------------------\n"
              << "Latency p50:        " << p50 << " ms\n"
              << "Latency p90:        " << p90 << " ms\n"
              << "Latency p99:        " << p99 << " ms\n"
              << "Latency max:        " << maxLat << " ms\n"
              << "===========================================================\n";

    return 0;
}
