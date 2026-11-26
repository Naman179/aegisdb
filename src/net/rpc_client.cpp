#include "net/rpc_client.h"
#include "common/logger.h"

#include <cstring>

namespace aegis {

RpcClient::RpcClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

RpcClient::~RpcClient() {
    close();
}

bool RpcClient::connect() {
    std::lock_guard<std::mutex> lk(connMu_);
    net_util::initSockets();

    running_ = false;
    connected_ = false;
    socket_handle_t oldS = fd_.load();
    if (oldS != AEGIS_INVALID_SOCKET) {
        ::shutdown(oldS, AEGIS_SHUT_RDWR);
    }
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    {
        std::lock_guard<std::mutex> slk(sendMu_);
        oldS = fd_.exchange(AEGIS_INVALID_SOCKET);
        if (oldS != AEGIS_INVALID_SOCKET) {
            AEGIS_CLOSE_SOCKET(oldS);
        }
    }
    failAllPending();

    socket_handle_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == AEGIS_INVALID_SOCKET) {
        return false;
    }

    net_util::setTcpNoDelay(s);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (host_ == "localhost" || host_ == "127.0.0.1") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == AEGIS_SOCKET_ERROR) {
        AEGIS_CLOSE_SOCKET(s);
        return false;
    }

    {
        std::lock_guard<std::mutex> slk(sendMu_);
        fd_ = s;
        connected_ = true;
        running_ = true;
    }
    readerThread_ = std::thread(&RpcClient::readerLoop, this);
    return true;
}

std::future<Frame> RpcClient::call(uint8_t msgType, const std::vector<uint8_t>& payload, uint32_t /*timeoutMs*/) {
    if (!connected_ && !connect()) {
        std::promise<Frame> p;
        p.set_exception(std::make_exception_ptr(std::runtime_error("RpcClient not connected")));
        return p.get_future();
    }

    uint64_t reqId = nextReqId_++;
    std::future<Frame> fut;
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        fut = pending_[reqId].get_future();
    }

    std::vector<uint8_t> encoded;
    encodeFrame(msgType, reqId, payload.data(), payload.size(), encoded);

    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(sendMu_);
        socket_handle_t s = fd_.load();
        if (connected_ && s != AEGIS_INVALID_SOCKET) {
            ok = net_util::sendExact(s, encoded.data(), encoded.size());
        }
    }

    if (!ok) {
        std::lock_guard<std::mutex> lk(pendingMu_);
        auto it = pending_.find(reqId);
        if (it != pending_.end()) {
            it->second.set_exception(std::make_exception_ptr(std::runtime_error("Send failed")));
            pending_.erase(it);
        }
    }

    return fut;
}

bool RpcClient::callSync(uint8_t msgType, const std::vector<uint8_t>& payload, Frame& out, uint32_t timeoutMs) {
    auto fut = call(msgType, payload, timeoutMs);
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
        try {
            out = fut.get();
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

void RpcClient::close() {
    std::lock_guard<std::mutex> lk(connMu_);
    running_ = false;
    connected_ = false;

    socket_handle_t s = fd_.load();
    if (s != AEGIS_INVALID_SOCKET) {
        ::shutdown(s, AEGIS_SHUT_RDWR);
    }

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    {
        std::lock_guard<std::mutex> slk(sendMu_);
        s = fd_.exchange(AEGIS_INVALID_SOCKET);
        if (s != AEGIS_INVALID_SOCKET) {
            AEGIS_CLOSE_SOCKET(s);
        }
    }

    failAllPending();
}

void RpcClient::failAllPending() {
    std::lock_guard<std::mutex> lk(pendingMu_);
    for (auto& [id, prom] : pending_) {
        try {
            prom.set_exception(std::make_exception_ptr(std::runtime_error("Connection closed")));
        } catch (...) {}
    }
    pending_.clear();
}

void RpcClient::readerLoop() {
    FrameStream stream;
    std::vector<uint8_t> buffer(64 * 1024);

    while (running_ && connected_) {
        socket_handle_t s = fd_.load();
        if (s == AEGIS_INVALID_SOCKET) break;
        int n = net_util::recvWithTimeout(s, buffer.data(), buffer.size(), 200);
        if (n == 0) {
            // Closed by peer
            break;
        }
        if (n < 0) {
            if (!running_) break;
            // timeout; keep waiting
            continue;
        }

        if (!stream.feed(buffer.data(), static_cast<size_t>(n))) {
            LOG_WARN("RpcClient: invalid frame from %s:%d", host_.c_str(), port_);
            break;
        }

        Frame f;
        while (stream.next(f)) {
            std::promise<Frame> prom;
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(pendingMu_);
                auto it = pending_.find(f.requestId);
                if (it != pending_.end()) {
                    prom = std::move(it->second);
                    pending_.erase(it);
                    found = true;
                }
            }
            if (found) {
                prom.set_value(std::move(f));
            }
        }
    }

    connected_ = false;
    failAllPending();
}

}  // namespace aegis
