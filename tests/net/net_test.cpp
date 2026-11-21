#include "net/tcp_server.h"
#include "net/rpc_client.h"
#include "net/faults.h"

#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

TEST(NetTest, FrameRoundTrip) {
    TcpServer server;
    bool started = server.start(0, [](std::shared_ptr<TcpConnection> conn, Frame f) {
        // Echo back with modified type
        conn->send(f.msgType + 10, f.requestId, f.payload.data(), f.payload.size());
    }, "127.0.0.1");
    ASSERT_TRUE(started);

    RpcClient client("127.0.0.1", server.port());
    ASSERT_TRUE(client.connect());

    std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    auto fut = client.call(5, payload, 2000);
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    Frame resp = fut.get();
    EXPECT_EQ(resp.msgType, 15);
    EXPECT_EQ(resp.payload, payload);

    client.close();
    server.stop();
}

TEST(NetTest, PartialRecvReassembles) {
    std::promise<Frame> receivedPromise;
    auto receivedFut = receivedPromise.get_future();

    TcpServer server;
    bool started = server.start(0, [&receivedPromise](std::shared_ptr<TcpConnection>, Frame f) {
        receivedPromise.set_value(std::move(f));
    }, "127.0.0.1");
    ASSERT_TRUE(started);

    // Send a frame 1 byte at a time using raw socket
    net_util::initSockets();
    socket_handle_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(fd, AEGIS_INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(server.port()));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int connRes = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ASSERT_NE(connRes, AEGIS_SOCKET_ERROR);

    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    std::vector<uint8_t> encoded;
    encodeFrame(42, 9999, payload.data(), payload.size(), encoded);

    // Feed byte-at-a-time with small sleep
    for (size_t i = 0; i < encoded.size(); ++i) {
        ::send(fd, reinterpret_cast<const char*>(&encoded[i]), 1, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_EQ(receivedFut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    Frame got = receivedFut.get();
    EXPECT_EQ(got.msgType, 42);
    EXPECT_EQ(got.requestId, 9999u);
    EXPECT_EQ(got.payload, payload);

    AEGIS_CLOSE_SOCKET(fd);
    server.stop();
}

TEST(NetTest, RpcRequestIdMatching) {
    TcpServer server;
    bool started = server.start(0, [](std::shared_ptr<TcpConnection> conn, Frame f) {
        // Echo back payload with request ID preserved
        conn->send(f.msgType, f.requestId, f.payload.data(), f.payload.size());
    }, "127.0.0.1");
    ASSERT_TRUE(started);

    RpcClient client("127.0.0.1", server.port());
    ASSERT_TRUE(client.connect());

    const int kRequests = 50;
    std::vector<std::future<Frame>> futures;
    futures.reserve(kRequests);

    for (int i = 0; i < kRequests; ++i) {
        std::vector<uint8_t> payload(i + 1, static_cast<uint8_t>(i));
        futures.push_back(client.call(static_cast<uint8_t>(i % 255), payload, 3000));
    }

    for (int i = 0; i < kRequests; ++i) {
        ASSERT_EQ(futures[i].wait_for(std::chrono::seconds(3)), std::future_status::ready);
        Frame resp = futures[i].get();
        EXPECT_EQ(resp.msgType, static_cast<uint8_t>(i % 255));
        std::vector<uint8_t> expected(i + 1, static_cast<uint8_t>(i));
        EXPECT_EQ(resp.payload, expected);
    }

    client.close();
    server.stop();
}

TEST(NetTest, ReconnectAfterPeerRestart) {
    int port = 0;
    {
        TcpServer server;
        ASSERT_TRUE(server.start(0, [](std::shared_ptr<TcpConnection> conn, Frame f) {
            conn->send(f.msgType, f.requestId, f.payload.data(), f.payload.size());
        }, "127.0.0.1"));
        port = server.port();

        RpcClient client("127.0.0.1", port);
        ASSERT_TRUE(client.connect());

        Frame resp;
        ASSERT_TRUE(client.callSync(1, {10, 20}, resp, 1000));
        EXPECT_EQ(resp.msgType, 1);

        // Server stops
        server.stop();

        // Call while down should fail
        Frame failResp;
        EXPECT_FALSE(client.callSync(2, {30}, failResp, 200));

        // Restart server on same port
        TcpServer server2;
        ASSERT_TRUE(server2.start(port, [](std::shared_ptr<TcpConnection> conn, Frame f) {
            conn->send(f.msgType, f.requestId, f.payload.data(), f.payload.size());
        }, "127.0.0.1"));

        // Call again -> client auto-reconnects
        Frame successResp;
        ASSERT_TRUE(client.callSync(3, {40, 50}, successResp, 2000));
        EXPECT_EQ(successResp.msgType, 3);
        EXPECT_EQ(successResp.payload, (std::vector<uint8_t>{40, 50}));

        client.close();
        server2.stop();
    }
}

}  // namespace
}  // namespace aegis
