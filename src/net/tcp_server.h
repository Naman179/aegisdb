#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// tcp_server.h – Concurrent TCP frame server (Milestone 8)
//
// Acceptor thread + per-connection reader thread.
// Decoded frames are handed to a handler (dispatched to a ThreadPool).
// Outgoing frames are protected by a per-connection send mutex.
// ─────────────────────────────────────────────────────────────────────────────
#include "net/frame_stream.h"
#include "net/socket_compat.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

namespace aegis {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    explicit TcpConnection(socket_handle_t fd, std::string remoteAddr = "");
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // Send a frame. Thread-safe; protected by per-connection send mutex.
    bool send(const Frame& frame);
    bool send(uint8_t msgType, uint64_t requestId, const uint8_t* payload, size_t payloadLen);

    void shutdown();
    void close();
    bool isOpen() const { return open_; }
    socket_handle_t fd() const { return fd_.load(); }
    const std::string& remoteAddr() const { return remoteAddr_; }

private:
    std::atomic<socket_handle_t> fd_{AEGIS_INVALID_SOCKET};
    std::string                  remoteAddr_;
    std::mutex                   sendMu_;
    std::atomic<bool>            open_{true};
};

using FrameHandler = std::function<void(std::shared_ptr<TcpConnection>, Frame)>;

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Start listening on host/port and invoke handler on received frames.
    // port 0 means OS-assigned ephemeral port.
    bool start(int port, FrameHandler handler, const std::string& bindHost = "0.0.0.0");
    void stop();

    int port() const { return boundPort_; }
    bool isRunning() const { return running_; }

private:
    void acceptLoop();
    void clientLoop(std::shared_ptr<TcpConnection> conn);

    std::atomic<socket_handle_t>   listenFd_{AEGIS_INVALID_SOCKET};
    int                            boundPort_{0};
    FrameHandler                   handler_;
    std::atomic<bool>              running_{false};
    std::thread                    acceptThread_;
    std::mutex                     connsMu_;
    std::vector<std::shared_ptr<TcpConnection>> connections_;
    std::vector<std::thread>       clientThreads_;
};

}  // namespace aegis
