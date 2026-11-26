// ─────────────────────────────────────────────────────────────────────────────
// main.cpp – aegisd server daemon (Milestone 9)
//
// Usage:
//   aegisd --id <1..5> [--client-port <port>] [--peer-port <port>]
//          [--data-dir <path>] [--peers <peer_list>]
// ─────────────────────────────────────────────────────────────────────────────
#include "common/config.h"
#include "common/logger.h"
#include "server/server.h"

#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace aegis;

static std::atomic<bool> gTerminate{false};

static void signalHandler(int /*signum*/) {
    gTerminate = true;
}

static void parsePeers(const std::string& peerStr, std::vector<NodePeerInfo>& peers) {
    std::stringstream ss(peerStr);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::stringstream tss(token);
        std::string sId, host, sClient, sPeer;
        if (std::getline(tss, sId, ':') &&
            std::getline(tss, host, ':') &&
            std::getline(tss, sClient, ':') &&
            std::getline(tss, sPeer, ':')) {
            NodePeerInfo info;
            info.id = static_cast<NodeId>(std::stoul(sId));
            info.host = host;
            info.clientPort = std::stoi(sClient);
            info.peerPort = std::stoi(sPeer);
            peers.push_back(info);
        }
    }
}

int main(int argc, char** argv) {
    ServerOptions opts;
    opts.nodeId = 1;
    opts.clientPort = 7001;
    opts.peerPort = 8001;
    opts.dataDir = "./data/node1";

    std::string peersArg;
    std::string configPath;

    // First pass: check for --config
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        }
    }

    if (!configPath.empty()) {
        ConfigFile cfg;
        if (!cfg.loadFromFile(configPath)) {
            std::cerr << "Error: failed to load config file: " << configPath << "\n";
            return 1;
        }
        if (cfg.has("node_id")) opts.nodeId = static_cast<NodeId>(cfg.getInt("node_id"));
        if (cfg.has("client_port")) opts.clientPort = cfg.getInt("client_port");
        if (cfg.has("peer_port")) opts.peerPort = cfg.getInt("peer_port");
        if (cfg.has("data_dir")) opts.dataDir = cfg.get("data_dir");
        if (cfg.has("num_groups")) opts.numGroups = cfg.getInt("num_groups");
        if (cfg.has("vnodes_per_group")) opts.vnodesPerGroup = cfg.getInt("vnodes_per_group");
        if (cfg.has("worker_threads")) opts.workerThreads = static_cast<size_t>(cfg.getInt("worker_threads"));
        if (cfg.has("snapshot_threshold")) opts.snapshotThreshold = cfg.getU64("snapshot_threshold");
        if (cfg.has("peers")) peersArg = cfg.get("peers");
        else if (cfg.has("cluster.peers")) peersArg = cfg.get("cluster.peers");
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            ++i; // already processed
        } else if (arg == "--id" && i + 1 < argc) {
            opts.nodeId = static_cast<NodeId>(std::stoul(argv[++i]));
            opts.clientPort = 7000 + opts.nodeId;
            opts.peerPort = 8000 + opts.nodeId;
            opts.dataDir = "./data/node" + std::to_string(opts.nodeId);
        } else if (arg == "--client-port" && i + 1 < argc) {
            opts.clientPort = std::stoi(argv[++i]);
        } else if (arg == "--peer-port" && i + 1 < argc) {
            opts.peerPort = std::stoi(argv[++i]);
        } else if (arg == "--data-dir" && i + 1 < argc) {
            opts.dataDir = argv[++i];
        } else if (arg == "--peers" && i + 1 < argc) {
            peersArg = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: aegisd [options]\n"
                      << "  --config <path>        Load settings from INI configuration file\n"
                      << "  --id <nodeId>          Node ID (1..5)\n"
                      << "  --client-port <port>   Client TCP port\n"
                      << "  --peer-port <port>     Raft peer TCP port\n"
                      << "  --data-dir <path>      Storage directory\n"
                      << "  --peers <list>         id:host:clientPort:peerPort,...\n";
            return 0;
        }
    }

    if (!peersArg.empty()) {
        parsePeers(peersArg, opts.peers);
    } else {
        // Default 5-node cluster topology
        for (NodeId id = 1; id <= 5; ++id) {
            opts.peers.push_back(NodePeerInfo{
                id, "127.0.0.1", static_cast<int>(7000 + id), static_cast<int>(8000 + id)
            });
        }
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    Server server(opts);
    if (!server.start()) {
        std::cerr << "Failed to start aegisd node " << opts.nodeId << "\n";
        return 1;
    }

    std::cout << "aegisd node " << opts.nodeId << " running on client port "
              << server.clientPort() << " and peer port " << server.peerPort() << "\n";

    while (!gTerminate && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Shutting down aegisd node " << opts.nodeId << "...\n";
    server.stop();
    return 0;
}
