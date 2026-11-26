#include "common/config.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace aegis {
namespace {

TEST(ConfigTest, BasicKeyValueAndTypes) {
    std::string ini = R"(
# Server configuration
node_id = 2
client_port = 7002
peer_port: 8002
data_dir = /var/lib/aegisdb/node2
drop_rate = 0.05
election_timeout = 250
enabled = true
disabled_flag = off
)";

    ConfigFile cfg;
    ASSERT_TRUE(cfg.loadFromString(ini));

    EXPECT_TRUE(cfg.has("node_id"));
    EXPECT_EQ(cfg.getInt("node_id"), 2);
    EXPECT_EQ(cfg.getInt("client_port"), 7002);
    EXPECT_EQ(cfg.getInt("peer_port"), 8002);
    EXPECT_EQ(cfg.get("data_dir"), "/var/lib/aegisdb/node2");
    EXPECT_DOUBLE_EQ(cfg.getDouble("drop_rate"), 0.05);
    EXPECT_EQ(cfg.getU64("election_timeout"), 250u);
    EXPECT_TRUE(cfg.getBool("enabled"));
    EXPECT_FALSE(cfg.getBool("disabled_flag"));
}

TEST(ConfigTest, SectionsAndInlineComments) {
    std::string ini = R"(
[server]
id = 3 ; this is node 3
port = 7003 # client port

[raft]
heartbeat_ms = 50
check_quorum = yes
)";

    ConfigFile cfg;
    ASSERT_TRUE(cfg.loadFromString(ini));

    EXPECT_EQ(cfg.getInt("server.id"), 3);
    EXPECT_EQ(cfg.getInt("id"), 3);
    EXPECT_EQ(cfg.getInt("server.port"), 7003);
    EXPECT_EQ(cfg.getInt("port"), 7003);

    EXPECT_EQ(cfg.getInt("raft.heartbeat_ms"), 50);
    EXPECT_TRUE(cfg.getBool("raft.check_quorum"));
}

TEST(ConfigTest, LoadFromFileRoundtrip) {
    std::string tmpPath = "./test_aegis.conf";
    {
        std::ofstream out(tmpPath);
        out << "[cluster]\n";
        out << "peers = 1:127.0.0.1:7001:8001,2:127.0.0.1:7002:8002\n";
        out << "num_groups = 3\n";
    }

    ConfigFile cfg;
    ASSERT_TRUE(cfg.loadFromFile(tmpPath));

    EXPECT_EQ(cfg.get("cluster.peers"), "1:127.0.0.1:7001:8001,2:127.0.0.1:7002:8002");
    EXPECT_EQ(cfg.getInt("cluster.num_groups"), 3);

    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
}

}  // namespace
}  // namespace aegis
