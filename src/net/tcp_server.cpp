#include "net/tcp_server.h"
#include "common/logger.h"

#include <cstring>

namespace aegis {

TcpConnection::TcpConnection(socket_handle_t fd, std::string remoteAddr)
    : fd_(fd), remoteAddr_(std::move(remoteAddr)) {
    if (fd != AEGIS_INVALID_SOCKET) {
        net_util::setTcpNoDelay(fd);
    }
}

TcpConnection::~TcpConnection() {
    close();
}

void TcpConnection::shutdown() {
    open_.store(false);
    socket_handle_t s = fd_.load();
    if (s != AEGIS_INVALID_SOCKET) {
        ::shutdown(s, AEGIS_SHUT_RDWR);
    }
}

void TcpConnection::close() {
    shutdown();
    std::lock_guard<std::mutex> lk(sendMu_);
    socket_handle_t s = fd_.exchange(AEGIS_INVALID_SOCKET);
    if (s != AEGIS_INVALID_SOCKET) {
        AEGIS_CLOSE_SOCKET(s);
    }
}

bool TcpConnection::send(const Frame& frame) {
    return send(frame.msgType, frame.requestId, frame.payload.data(), frame.payload.size());
}

bool TcpConnection::send(uint8_t msgType, uint64_t requestId, const uint8_t* payload, size_t payloadLen) {
    if (!open_) return false;
    socket_handle_t s = fd_.load();
    if (s == AEGIS_INVALID_SOCKET) return false;

    std::vector<uint8_t> encoded;
    encodeFrame(msgType, requestId, payload, payloadLen, encoded);

    std::lock_guard<std::mutex> lk(sendMu_);
    s = fd_.load();
    if (!open_ || s == AEGIS_INVALID_SOCKET) return false;
    return net_util::sendExact(s, encoded.data(), encoded.size());
}

TcpServer::TcpServer() = default;

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(int port, FrameHandler handler, const std::string& bindHost) {
    net_util::initSockets();
    handler_ = std::move(handler);

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ == AEGIS_INVALID_SOCKET) {
        LOG_ERROR("TcpServer: failed to create socket");
        return false;
    }

    net_util::setReuseAddr(listenFd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bindHost.empty() || bindHost == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        inet_pton(AF_INET, bindHost.c_str(), &addr.sin_addr);
    }

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == AEGIS_SOCKET_ERROR) {
        LOG_ERROR("TcpServer: bind failed on port %d", port);
        AEGIS_CLOSE_SOCKET(listenFd_);
        listenFd_ = AEGIS_INVALID_SOCKET;
        return false;
    }

    if (port == 0) {
        sockaddr_in boundAddr{};
        socklen_t len = sizeof(boundAddr);
        if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&boundAddr), &len) == 0) {
            boundPort_ = ntohs(boundAddr.sin_port);
        }
    } else {
        boundPort_ = port;
    }

    if (::listen(listenFd_, 128) == AEGIS_SOCKET_ERROR) {
        LOG_ERROR("TcpServer: listen failed");
        AEGIS_CLOSE_SOCKET(listenFd_);
        listenFd_ = AEGIS_INVALID_SOCKET;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&TcpServer::acceptLoop, this);
    return true;
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;

    socket_handle_t fd = listenFd_.exchange(AEGIS_INVALID_SOCKET);
    if (fd != AEGIS_INVALID_SOCKET) {
        ::shutdown(fd, AEGIS_SHUT_RDWR);
        AEGIS_CLOSE_SOCKET(fd);
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // Unblock all client recv/send threads first
    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lk(connsMu_);
        for (auto& conn : connections_) {
            conn->shutdown();
        }
        threadsToJoin = std::move(clientThreads_);
        clientThreads_.clear();
    }

    // Wait for all client threads to finish before closing fds
    for (auto& t : threadsToJoin) {
        if (t.joinable()) {
            t.join();
        }
    }

    // Now all client threads are joined; close all connection sockets safely
    {
        std::lock_guard<std::mutex> lk(connsMu_);
        for (auto& conn : connections_) {
            conn->close();
        }
        connections_.clear();
    }
}

void TcpServer::acceptLoop() {
    while (running_) {
        socket_handle_t lfd = listenFd_.load();
        if (lfd == AEGIS_INVALID_SOCKET) break;

        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        socket_handle_t clientFd = ::accept(lfd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

        if (clientFd == AEGIS_INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }

        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        std::string remote = std::string(ipBuf) + ":" + std::to_string(ntohs(clientAddr.sin_port));

        auto conn = std::make_shared<TcpConnection>(clientFd, remote);
        {
            std::lock_guard<std::mutex> lk(connsMu_);
            connections_.push_back(conn);
            clientThreads_.emplace_back(&TcpServer::clientLoop, this, conn);
        }
    }
}

void TcpServer::clientLoop(std::shared_ptr<TcpConnection> conn) {
    FrameStream stream;
    std::vector<uint8_t> buffer(64 * 1024);

    while (running_ && conn->isOpen()) {
        int n = net_util::recvWithTimeout(conn->fd(), buffer.data(), buffer.size(), 200);
        if (n == 0) {
            // Peer closed
            break;
        }
        if (n < 0) {
            if (!running_ || !conn->isOpen()) break;
            // timeout; loop and check running flag
            continue;
        }

        if (!stream.feed(buffer.data(), static_cast<size_t>(n))) {
            LOG_WARN("TcpServer: corrupt frame received from %s", conn->remoteAddr().c_str());
            break;
        }

        Frame f;
        while (stream.next(f)) {
            if (handler_) {
                handler_(conn, std::move(f));
            }
        }
    }

    conn->close();
}

}  // namespace aegis
