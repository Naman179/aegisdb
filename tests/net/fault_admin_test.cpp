#include "net/rpc_client.h"
#include "net/faults.h"
#include "server/client_protocol.h"
#include "server/server.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

TEST(FaultAdminTest, EncodeDecodeRoundtrip) {
    AdminFaultReq req{"block", "127.0.0.1:8002", 0.35, 120};
    std::vector<uint8_t> payload;
    encodeAdminFaultReq(req, payload);

    AdminFaultReq decoded;
    ASSERT_TRUE(decodeAdminFaultReq(payload.data(), payload.size(), decoded));
    EXPECT_EQ(decoded.action, "block");
    EXPECT_EQ(decoded.peer, "127.0.0.1:8002");
    EXPECT_DOUBLE_EQ(decoded.dropRate, 0.35);
    EXPECT_EQ(decoded.delayMs, 120u);

    AdminFaultResp resp{true, "peer blocked successfully"};
    std::vector<uint8_t> respPayload;
    encodeAdminFaultResp(resp, respPayload);

    AdminFaultResp decodedResp;
    ASSERT_TRUE(decodeAdminFaultResp(respPayload.data(), respPayload.size(), decodedResp));
    EXPECT_TRUE(decodedResp.ok);
    EXPECT_EQ(decodedResp.message, "peer blocked successfully");
}

TEST(FaultAdminTest, ServerAdminFaultLiveControl) {
    std::string dataDir = "./data_admin_fault_test";
    std::error_code ec;
    std::filesystem::remove_all(dataDir, ec);

    ServerOptions opts;
    opts.nodeId = 1;
    opts.clientPort = 23101;
    opts.peerPort = 24101;
    opts.dataDir = dataDir;
    opts.peers = {
        {1, "127.0.0.1", 23101, 24101},
        {2, "127.0.0.1", 23102, 24102}
    };
    opts.numGroups = 1;
    opts.vnodesPerGroup = 32;

    Server server(opts);
    ASSERT_TRUE(server.start());

    RpcClient client("127.0.0.1", 23101);
    ASSERT_TRUE(client.connect());

    // 1. Block peer
    {
        AdminFaultReq req{"block", "127.0.0.1:24102", 0.0, 0};
        std::vector<uint8_t> payload;
        encodeAdminFaultReq(req, payload);
        Frame resp;
        ASSERT_TRUE(client.callSync(kMsgAdminFaultReq, payload, resp, 2000));
        AdminFaultResp r;
        ASSERT_TRUE(decodeAdminFaultResp(resp.payload.data(), resp.payload.size(), r));
        EXPECT_TRUE(r.ok);
        EXPECT_TRUE(server.faults()->isBlocked("127.0.0.1:24102"));
    }

    // 2. Set Drop Rate
    {
        AdminFaultReq req{"set_drop", "", 0.45, 0};
        std::vector<uint8_t> payload;
        encodeAdminFaultReq(req, payload);
        Frame resp;
        ASSERT_TRUE(client.callSync(kMsgAdminFaultReq, payload, resp, 2000));
        AdminFaultResp r;
        ASSERT_TRUE(decodeAdminFaultResp(resp.payload.data(), resp.payload.size(), r));
        EXPECT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(server.faults()->dropRate(), 0.45);
    }

    // 3. Set Delay
    {
        AdminFaultReq req{"set_delay", "", 0.0, 75};
        std::vector<uint8_t> payload;
        encodeAdminFaultReq(req, payload);
        Frame resp;
        ASSERT_TRUE(client.callSync(kMsgAdminFaultReq, payload, resp, 2000));
        AdminFaultResp r;
        ASSERT_TRUE(decodeAdminFaultResp(resp.payload.data(), resp.payload.size(), r));
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(server.faults()->delay().count(), 75);
    }

    // 4. Unblock peer
    {
        AdminFaultReq req{"unblock", "127.0.0.1:24102", 0.0, 0};
        std::vector<uint8_t> payload;
        encodeAdminFaultReq(req, payload);
        Frame resp;
        ASSERT_TRUE(client.callSync(kMsgAdminFaultReq, payload, resp, 2000));
        AdminFaultResp r;
        ASSERT_TRUE(decodeAdminFaultResp(resp.payload.data(), resp.payload.size(), r));
        EXPECT_TRUE(r.ok);
        EXPECT_FALSE(server.faults()->isBlocked("127.0.0.1:24102"));
    }

    client.close();
    server.stop();
    std::filesystem::remove_all(dataDir, ec);
}

}  // namespace
}  // namespace aegis
