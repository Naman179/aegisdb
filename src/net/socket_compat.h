#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// socket_compat.h – Cross-platform TCP socket primitives (POSIX & WinSock)
// ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    using socket_handle_t = SOCKET;
    #define AEGIS_INVALID_SOCKET INVALID_SOCKET
    #define AEGIS_SOCKET_ERROR   SOCKET_ERROR
    #define AEGIS_CLOSE_SOCKET(s) closesocket(s)
    #define AEGIS_SHUT_RDWR      SD_BOTH
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>

    using socket_handle_t = int;
    #define AEGIS_INVALID_SOCKET (-1)
    #define AEGIS_SOCKET_ERROR   (-1)
    #define AEGIS_CLOSE_SOCKET(s) ::close(s)
    #define AEGIS_SHUT_RDWR      SHUT_RDWR
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace aegis {
namespace net_util {

inline bool initSockets() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsaData;
        int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (res != 0) return false;
        initialized = true;
    }
#endif
    return true;
}

inline void setTcpNoDelay(socket_handle_t fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
}

inline void setReuseAddr(socket_handle_t fd) {
    int flag = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&flag), sizeof(flag));
}

// Loop until exactly len bytes are sent or error
inline bool sendExact(socket_handle_t fd, const uint8_t* data, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = ::send(fd, reinterpret_cast<const char*>(data + total), static_cast<int>(len - total), 0);
        if (n <= 0) {
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

// Receive up to maxLen bytes, waiting up to timeoutMs (negative = infinite)
// Returns >0 bytes read, 0 on disconnect, -1 on timeout/error
inline int recvWithTimeout(socket_handle_t fd, uint8_t* buf, size_t maxLen, int timeoutMs) {
#ifdef _WIN32
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int sel = select(0, &readSet, nullptr, nullptr, timeoutMs >= 0 ? &tv : nullptr);
    if (sel <= 0) return sel; // 0 = timeout, <0 = error
    return ::recv(fd, reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0);
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int res = ::poll(&pfd, 1, timeoutMs);
    if (res <= 0) return res; // 0 = timeout, <0 = error
    return static_cast<int>(::recv(fd, buf, maxLen, 0));
#endif
}

}  // namespace net_util
}  // namespace aegis
