#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// client.h – Official AegisDB C++ Client SDK
//
// Features:
// • Transparent consistent-hash key routing across Raft groups
// • Automatic leader discovery and NOT_LEADER caching
// • Seamless failover retry on leader changes and network drops
// • At-most-once semantics with monotonically incremented sequence numbers
// ─────────────────────────────────────────────────────────────────────────────
#include "net/rpc_client.h"
#include "server/client_protocol.h"
#include "shard/hash_ring.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aegis {

struct ClientConfig {
    std::vector<std::string> clusterAddrs{"127.0.0.1:7001"};
    int                      numGroups{3};
    int                      vnodesPerGroup{128};
    std::string              clientId{"aegis_client"};
    uint32_t                 timeoutMs{2500};
    uint32_t                 maxRetries{6};
};

class AegisClient {
public:
    explicit AegisClient(ClientConfig config = ClientConfig{})
        : config_(std::move(config)),
          seqNo_(1) {
        for (int g = 0; g < config_.numGroups; ++g) {
            ring_.addGroup(g, config_.vnodesPerGroup);
        }
    }

    ~AegisClient() {
        close();
    }

    void close() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [addr, client] : pool_) {
            client->close();
        }
        pool_.clear();
    }

    bool put(const std::string& key, const std::string& val, uint64_t* outVersion = nullptr) {
        uint64_t seq = seqNo_++;
        int group = ring_.groupFor(key);

        for (uint32_t attempt = 0; attempt < config_.maxRetries; ++attempt) {
            std::string targetAddr = getTargetAddr(group, attempt);
            if (targetAddr.empty()) break;

            auto client = getClient(targetAddr);
            if (!client) continue;

            KvPutReq req{config_.clientId, seq, key, val};
            std::vector<uint8_t> payload;
            encodeKvPutReq(req, payload);

            Frame resp;
            if (client->callSync(kMsgKvPutReq, payload, resp, config_.timeoutMs)) {
                KvPutResp r;
                if (decodeKvPutResp(resp.payload.data(), resp.payload.size(), r)) {
                    if (r.status == StatusCode::kOk) {
                        if (outVersion) *outVersion = r.version;
                        updateLeader(group, targetAddr);
                        return true;
                    }
                    if (r.status == StatusCode::kNotLeader) {
                        if (!r.leaderAddr.empty()) {
                            updateLeader(group, r.leaderAddr);
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        return false;
    }

    bool get(const std::string& key, std::string& outVal, uint64_t* outVersion = nullptr, bool stale = false) {
        uint64_t seq = seqNo_++;
        int group = ring_.groupFor(key);

        for (uint32_t attempt = 0; attempt < config_.maxRetries; ++attempt) {
            std::string targetAddr = getTargetAddr(group, attempt);
            if (targetAddr.empty()) break;

            auto client = getClient(targetAddr);
            if (!client) continue;

            KvGetReq req{config_.clientId, seq, key, stale};
            std::vector<uint8_t> payload;
            encodeKvGetReq(req, payload);

            Frame resp;
            if (client->callSync(kMsgKvGetReq, payload, resp, config_.timeoutMs)) {
                KvGetResp r;
                if (decodeKvGetResp(resp.payload.data(), resp.payload.size(), r)) {
                    if (r.status == StatusCode::kOk) {
                        outVal = r.value;
                        if (outVersion) *outVersion = r.version;
                        updateLeader(group, targetAddr);
                        return true;
                    }
                    if (r.status == StatusCode::kKeyNotFound) {
                        return false;
                    }
                    if (r.status == StatusCode::kNotLeader) {
                        if (!r.leaderAddr.empty()) {
                            updateLeader(group, r.leaderAddr);
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        return false;
    }

    bool del(const std::string& key, uint64_t* outVersion = nullptr) {
        uint64_t seq = seqNo_++;
        int group = ring_.groupFor(key);

        for (uint32_t attempt = 0; attempt < config_.maxRetries; ++attempt) {
            std::string targetAddr = getTargetAddr(group, attempt);
            if (targetAddr.empty()) break;

            auto client = getClient(targetAddr);
            if (!client) continue;

            KvDelReq req{config_.clientId, seq, key};
            std::vector<uint8_t> payload;
            encodeKvDelReq(req, payload);

            Frame resp;
            if (client->callSync(kMsgKvDelReq, payload, resp, config_.timeoutMs)) {
                KvDelResp r;
                if (decodeKvDelResp(resp.payload.data(), resp.payload.size(), r)) {
                    if (r.status == StatusCode::kOk) {
                        if (outVersion) *outVersion = r.version;
                        updateLeader(group, targetAddr);
                        return true;
                    }
                    if (r.status == StatusCode::kKeyNotFound) {
                        return false;
                    }
                    if (r.status == StatusCode::kNotLeader) {
                        if (!r.leaderAddr.empty()) {
                            updateLeader(group, r.leaderAddr);
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        return false;
    }

    bool cas(const std::string& key, const std::string& val, uint64_t expectedVersion, uint64_t* outVersion = nullptr) {
        uint64_t seq = seqNo_++;
        int group = ring_.groupFor(key);

        for (uint32_t attempt = 0; attempt < config_.maxRetries; ++attempt) {
            std::string targetAddr = getTargetAddr(group, attempt);
            if (targetAddr.empty()) break;

            auto client = getClient(targetAddr);
            if (!client) continue;

            KvCasReq req{config_.clientId, seq, key, val, expectedVersion};
            std::vector<uint8_t> payload;
            encodeKvCasReq(req, payload);

            Frame resp;
            if (client->callSync(kMsgKvCasReq, payload, resp, config_.timeoutMs)) {
                KvCasResp r;
                if (decodeKvCasResp(resp.payload.data(), resp.payload.size(), r)) {
                    if (r.status == StatusCode::kOk) {
                        if (outVersion) *outVersion = r.version;
                        updateLeader(group, targetAddr);
                        return true;
                    }
                    if (r.status == StatusCode::kVersionMismatch) {
                        return false;
                    }
                    if (r.status == StatusCode::kNotLeader) {
                        if (!r.leaderAddr.empty()) {
                            updateLeader(group, r.leaderAddr);
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        return false;
    }

    int groupFor(const std::string& key) const {
        return ring_.groupFor(key);
    }

private:
    std::shared_ptr<RpcClient> getClient(const std::string& addr) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pool_.find(addr);
        if (it != pool_.end() && it->second->isConnected()) {
            return it->second;
        }

        size_t colon = addr.rfind(':');
        if (colon == std::string::npos) return nullptr;
        std::string host = addr.substr(0, colon);
        int port = std::stoi(addr.substr(colon + 1));

        auto client = std::make_shared<RpcClient>(host, port);
        if (client->connect()) {
            pool_[addr] = client;
            return client;
        }
        return nullptr;
    }

    std::string getTargetAddr(int group, uint32_t attempt) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = groupLeaders_.find(group);
        if (it != groupLeaders_.end() && !it->second.empty() && attempt == 0) {
            return it->second;
        }
        if (config_.clusterAddrs.empty()) return "";
        return config_.clusterAddrs[attempt % config_.clusterAddrs.size()];
    }

    void updateLeader(int group, const std::string& addr) {
        std::lock_guard<std::mutex> lk(mu_);
        groupLeaders_[group] = addr;
    }

    ClientConfig                                     config_;
    HashRing                                         ring_;
    std::atomic<uint64_t>                            seqNo_{1};
    std::mutex                                       mu_;
    std::unordered_map<int, std::string>             groupLeaders_;
    std::unordered_map<std::string, std::shared_ptr<RpcClient>> pool_;
};

}  // namespace aegis
