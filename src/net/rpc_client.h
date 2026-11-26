#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// rpc_client.h – Persistent multiplexed RPC client over TCP (Milestone 8)
//
// Matches responses to requests using 64-bit requestId.
// Many concurrent calls can be in flight on a single connection.
// ─────────────────────────────────────────────────────────────────────────────
#include "net/frame_stream.h"
#include "net/socket_compat.h"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aegis {

class RpcClient {
public:
    RpcClient(std::string host, int port);
    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // Connect (or reconnect) to the server. Returns true if connected.
    bool connect();

    // Call RPC: sends frame with unique requestId and returns a future for response.
    // If disconnected, attempts reconnect once.
    std::future<Frame> call(uint8_t msgType, const std::vector<uint8_t>& payload, uint32_t timeoutMs = 2000);

    // Synchronous convenience wrapper
    bool callSync(uint8_t msgType, const std::vector<uint8_t>& payload, Frame& out, uint32_t timeoutMs = 2000);

    void close();
    bool isConnected() const { return connected_; }
    const std::string& host() const { return host_; }
    int port() const { return port_; }

private:
    void readerLoop();
    void failAllPending();

    std::string                      host_;
    int                              port_;
    std::atomic<socket_handle_t>     fd_{AEGIS_INVALID_SOCKET};
    std::atomic<bool>                connected_{false};
    std::atomic<bool>                running_{false};
    std::atomic<uint64_t>            nextReqId_{1};

    std::mutex                       connMu_;
    std::mutex                       sendMu_;
    std::mutex                       pendingMu_;
    std::unordered_map<uint64_t, std::promise<Frame>> pending_;

    std::thread                      readerThread_;
};

}  // namespace aegis
