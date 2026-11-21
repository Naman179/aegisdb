#include "net/faults.h"
#include <thread>

namespace aegis {

Faults::Faults(uint32_t seed)
    : rng_(seed) {}

void Faults::block(const std::string& peer) {
    std::lock_guard<std::mutex> lk(mu_);
    blocked_.insert(peer);
}

void Faults::unblock(const std::string& peer) {
    std::lock_guard<std::mutex> lk(mu_);
    blocked_.erase(peer);
}

void Faults::clearBlocked() {
    std::lock_guard<std::mutex> lk(mu_);
    blocked_.clear();
}

bool Faults::isBlocked(const std::string& peer) {
    std::lock_guard<std::mutex> lk(mu_);
    return blocked_.find(peer) != blocked_.end();
}

void Faults::setDropRate(double rate) {
    std::lock_guard<std::mutex> lk(mu_);
    dropRate_ = rate < 0.0 ? 0.0 : (rate > 1.0 ? 1.0 : rate);
}

double Faults::dropRate() {
    std::lock_guard<std::mutex> lk(mu_);
    return dropRate_;
}

void Faults::setDelay(std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lk(mu_);
    delay_ = delay;
}

std::chrono::milliseconds Faults::delay() {
    std::lock_guard<std::mutex> lk(mu_);
    return delay_;
}

void Faults::setConfig(const FaultConfig& config) {
    std::lock_guard<std::mutex> lk(mu_);
    blocked_.clear();
    for (const auto& p : config.blockedPeers) {
        blocked_.insert(p);
    }
    dropRate_ = config.dropRate < 0.0 ? 0.0 : (config.dropRate > 1.0 ? 1.0 : config.dropRate);
    delay_ = std::chrono::milliseconds(config.delayMs);
}

FaultConfig Faults::config() {
    std::lock_guard<std::mutex> lk(mu_);
    FaultConfig cfg;
    for (const auto& p : blocked_) {
        cfg.blockedPeers.push_back(p);
    }
    cfg.dropRate = dropRate_;
    cfg.delayMs = static_cast<uint32_t>(delay_.count());
    return cfg;
}

bool Faults::check(const std::string& peer, std::chrono::milliseconds& outDelay) {
    std::lock_guard<std::mutex> lk(mu_);
    if (blocked_.find(peer) != blocked_.end()) {
        return false;
    }
    if (dropRate_ > 0.0) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) < dropRate_) {
            return false;
        }
    }
    outDelay = delay_;
    return true;
}

bool Faults::allow(const std::string& peer) {
    std::chrono::milliseconds waitTime{0};
    if (!check(peer, waitTime)) {
        return false;
    }
    if (waitTime.count() > 0) {
        std::this_thread::sleep_for(waitTime);
    }
    return true;
}

}  // namespace aegis
