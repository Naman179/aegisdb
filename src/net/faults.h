#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// faults.h – Network fault injector (Milestone 8)
//
// Supports peer blocking (simulated network partitions), message drops,
// and artificial latency injection. Consulted on RPC sends and receives.
// ─────────────────────────────────────────────────────────────────────────────
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace aegis {

struct FaultConfig {
    std::vector<std::string> blockedPeers;
    double dropRate{0.0};
    uint32_t delayMs{0};
};

class Faults {
public:
    explicit Faults(uint32_t seed = 42);

    void block(const std::string& peer);
    void unblock(const std::string& peer);
    void clearBlocked();
    bool isBlocked(const std::string& peer);

    void setDropRate(double rate);
    double dropRate();

    void setDelay(std::chrono::milliseconds delay);
    std::chrono::milliseconds delay();

    void setConfig(const FaultConfig& config);
    FaultConfig config();

    // Consulted before sending/receiving. If blocked or dropped, returns false.
    // If delay > 0, pauses the current thread for that duration and returns true.
    bool allow(const std::string& peer);

    // Non-blocking inspection
    bool check(const std::string& peer, std::chrono::milliseconds& outDelay);

private:
    std::mutex mu_;
    std::unordered_set<std::string> blocked_;
    double dropRate_{0.0};
    std::chrono::milliseconds delay_{0};
    std::mt19937 rng_;
};

}  // namespace aegis
