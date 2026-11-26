// ─────────────────────────────────────────────────────────────────────────────
// aegisctl.cpp – Command-line client for AegisDB (Milestone 9)
//
// Usage:
//   aegisctl --node <host:port> put <key> <value>
//   aegisctl --node <host:port> get <key> [--stale]
//   aegisctl --node <host:port> del <key>
//   aegisctl --node <host:port> cas <key> <expectedVersion> <value>
//   aegisctl --node <host:port> status
//
// Automatically follows NOT_LEADER redirects to the current leader node.
// ─────────────────────────────────────────────────────────────────────────────
#include "net/rpc_client.h"
#include "server/client_protocol.h"
#include "shard/hash_ring.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace aegis;

struct ClientConfig {
    std::string nodeAddr{"127.0.0.1:7001"};
    std::string command;
    std::vector<std::string> args;
    bool stale{false};
};

static void printUsage() {
    std::cout << "Usage: aegisctl [options] <command> [args...]\n"
              << "Options:\n"
              << "  --node <host:port>   Node client endpoint (default: 127.0.0.1:7001)\n"
              << "  --stale              Allow stale local reads (get only)\n"
              << "Commands:\n"
              << "  put <key> <value>               Insert or update key\n"
              << "  get <key>                       Retrieve value by key\n"
              << "  del <key>                       Delete key\n"
              << "  cas <key> <expVersion> <value>  Compare-and-swap\n"
              << "  status                          Display cluster status\n"
              << "  top                             Display interactive cluster topology and node health\n"
              << "  routing <key>                   Show consistent-hash ring routing for key\n"
              << "  faults <action> [args...]       Inject network faults (block, unblock, clear, drop, delay)\n";
}

static bool parseHostPort(const std::string& addr, std::string& host, int& port) {
    auto pos = addr.find(':');
    if (pos == std::string::npos) {
        host = addr;
        port = 7001;
        return true;
    }
    host = addr.substr(0, pos);
    try {
        port = std::stoi(addr.substr(pos + 1));
        return true;
    } catch (...) {
        return false;
    }
}

int main(int argc, char** argv) {
    ClientConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--node" && i + 1 < argc) {
            cfg.nodeAddr = argv[++i];
        } else if (arg == "--stale") {
            cfg.stale = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (cfg.command.empty()) {
            cfg.command = arg;
        } else {
            cfg.args.push_back(arg);
        }
    }

    if (cfg.command.empty()) {
        printUsage();
        return 1;
    }

    std::string host;
    int port = 7001;
    parseHostPort(cfg.nodeAddr, host, port);

    int maxRedirects = 3;
    uint64_t seq = 1;
    std::string clientId = "aegisctl_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    for (int attempt = 0; attempt <= maxRedirects; ++attempt) {
        RpcClient client(host, port);
        if (!client.connect()) {
            std::cerr << "Error: could not connect to node at " << host << ":" << port << "\n";
            return 1;
        }

        if (cfg.command == "put") {
            if (cfg.args.size() < 2) {
                std::cerr << "Error: put requires <key> and <value>\n";
                return 1;
            }
            KvPutReq req{clientId, seq, cfg.args[0], cfg.args[1]};
            std::vector<uint8_t> payload;
            encodeKvPutReq(req, payload);

            Frame resp;
            if (!client.callSync(kMsgKvPutReq, payload, resp, 3000)) {
                std::cerr << "Error: request timed out\n";
                return 1;
            }
            KvPutResp r;
            decodeKvPutResp(resp.payload.data(), resp.payload.size(), r);
            if (r.status == StatusCode::kNotLeader && !r.leaderAddr.empty()) {
                parseHostPort(r.leaderAddr, host, port);
                continue;
            }
            if (r.status == StatusCode::kOk) {
                std::cout << "OK (version: " << r.version << ")\n";
                return 0;
            } else {
                std::cerr << "Error: status code " << static_cast<int>(r.status) << "\n";
                return 1;
            }

        } else if (cfg.command == "get") {
            if (cfg.args.empty()) {
                std::cerr << "Error: get requires <key>\n";
                return 1;
            }
            KvGetReq req{clientId, seq, cfg.args[0], cfg.stale};
            std::vector<uint8_t> payload;
            encodeKvGetReq(req, payload);

            Frame resp;
            if (!client.callSync(kMsgKvGetReq, payload, resp, 3000)) {
                std::cerr << "Error: request timed out\n";
                return 1;
            }
            KvGetResp r;
            decodeKvGetResp(resp.payload.data(), resp.payload.size(), r);
            if (r.status == StatusCode::kNotLeader && !r.leaderAddr.empty()) {
                parseHostPort(r.leaderAddr, host, port);
                continue;
            }
            if (r.status == StatusCode::kOk) {
                std::cout << r.value << "\n";
                return 0;
            } else if (r.status == StatusCode::kKeyNotFound) {
                std::cerr << "(nil)\n";
                return 2;
            } else {
                std::cerr << "Error: status code " << static_cast<int>(r.status) << "\n";
                return 1;
            }

        } else if (cfg.command == "del") {
            if (cfg.args.empty()) {
                std::cerr << "Error: del requires <key>\n";
                return 1;
            }
            KvDelReq req{clientId, seq, cfg.args[0]};
            std::vector<uint8_t> payload;
            encodeKvDelReq(req, payload);

            Frame resp;
            if (!client.callSync(kMsgKvDelReq, payload, resp, 3000)) {
                std::cerr << "Error: request timed out\n";
                return 1;
            }
            KvDelResp r;
            decodeKvDelResp(resp.payload.data(), resp.payload.size(), r);
            if (r.status == StatusCode::kNotLeader && !r.leaderAddr.empty()) {
                parseHostPort(r.leaderAddr, host, port);
                continue;
            }
            std::cout << (r.deleted ? "OK (deleted)\n" : "(not found)\n");
            return 0;

        } else if (cfg.command == "cas") {
            if (cfg.args.size() < 3) {
                std::cerr << "Error: cas requires <key> <expectedVersion> <value>\n";
                return 1;
            }
            uint64_t expVer = std::stoull(cfg.args[1]);
            KvCasReq req{clientId, seq, cfg.args[0], expVer, cfg.args[2]};
            std::vector<uint8_t> payload;
            encodeKvCasReq(req, payload);

            Frame resp;
            if (!client.callSync(kMsgKvCasReq, payload, resp, 3000)) {
                std::cerr << "Error: request timed out\n";
                return 1;
            }
            KvCasResp r;
            decodeKvCasResp(resp.payload.data(), resp.payload.size(), r);
            if (r.status == StatusCode::kNotLeader && !r.leaderAddr.empty()) {
                parseHostPort(r.leaderAddr, host, port);
                continue;
            }
            if (r.success) {
                std::cout << "OK (version: " << r.version << ")\n";
                return 0;
            } else {
                std::cerr << "CAS Mismatch: current version is " << r.version << "\n";
                return 1;
            }

        } else if (cfg.command == "status") {
            Frame resp;
            if (!client.callSync(kMsgAdminStatusReq, {}, resp, 2000)) {
                std::cerr << "Error: could not fetch status\n";
                return 1;
            }
            ClusterStatusResp status;
            decodeClusterStatusResp(resp.payload.data(), resp.payload.size(), status);
            std::cout << "Node ID: " << status.nodeId << "\n";
            for (const auto& g : status.groups) {
                std::cout << "  Group " << g.groupId << ": role="
                          << (g.role == RaftRole::kLeader ? "Leader" :
                             (g.role == RaftRole::kCandidate ? "Candidate" : "Follower"))
                          << ", term=" << g.term
                          << ", leader=" << g.leaderId
                          << ", commitIndex=" << g.commitIndex
                          << ", applied=" << g.lastApplied << "\n";
            }
            return 0;
        } else if (cfg.command == "routing") {
            if (cfg.args.empty()) {
                std::cerr << "Error: routing requires <key>\n";
                return 1;
            }
            std::string key = cfg.args[0];
            HashRing ring(3, 128);
            int group = ring.groupFor(key);
            uint64_t hashVal = HashRing::hash(key);

            std::cout << "Routing Resolution for Key: '" << key << "'\n"
                      << "  Consistent Hash: 0x" << std::hex << hashVal << std::dec << "\n"
                      << "  Target Group:    Group " << group << "\n";

            Frame resp;
            if (client.callSync(kMsgAdminStatusReq, {}, resp, 2000)) {
                ClusterStatusResp status;
                decodeClusterStatusResp(resp.payload.data(), resp.payload.size(), status);
                for (const auto& g : status.groups) {
                    if (g.groupId == group) {
                        std::cout << "  Leader Node ID:  " << (g.leaderId != kNone ? std::to_string(g.leaderId) : "unknown/election") << "\n"
                                  << "  Group Role:      " << (g.role == RaftRole::kLeader ? "Leader" : "Follower") << "\n"
                                  << "  Current Term:    " << g.term << "\n";
                        break;
                    }
                }
            }
            return 0;

        } else if (cfg.command == "top") {
            std::cout << "========================================================================================\n"
                      << "AegisDB Multi-Raft Cluster Dashboard (3 Groups · 5 Nodes · 15 Replicas)\n"
                      << "========================================================================================\n";
            std::cout << std::left << std::setw(8) << "Node"
                      << std::setw(18) << "Endpoint"
                      << std::setw(18) << "Group 0"
                      << std::setw(18) << "Group 1"
                      << std::setw(18) << "Group 2"
                      << "Status\n";
            std::cout << "----------------------------------------------------------------------------------------\n";

            int onlineCount = 0;
            int basePort = 7000;
            for (int id = 1; id <= 5; ++id) {
                std::string targetHost = host;
                int targetPort = basePort + id;
                RpcClient nodeClient(targetHost, targetPort);
                std::cout << std::left << std::setw(8) << ("Node " + std::to_string(id))
                          << std::setw(18) << (targetHost + ":" + std::to_string(targetPort));

                if (!nodeClient.connect()) {
                    std::cout << std::setw(18) << "-"
                              << std::setw(18) << "-"
                              << std::setw(18) << "-"
                              << "OFFLINE\n";
                    continue;
                }

                Frame resp;
                if (!nodeClient.callSync(kMsgAdminStatusReq, {}, resp, 1500)) {
                    std::cout << std::setw(18) << "-"
                              << std::setw(18) << "-"
                              << std::setw(18) << "-"
                              << "UNRESPONSIVE\n";
                    continue;
                }

                onlineCount++;
                ClusterStatusResp status;
                decodeClusterStatusResp(resp.payload.data(), resp.payload.size(), status);

                auto formatGroup = [](const GroupStatus& g) {
                    std::string r = (g.role == RaftRole::kLeader ? "LEADER" :
                                    (g.role == RaftRole::kCandidate ? "CAND" : "FOLLOWER"));
                    return r + " (T:" + std::to_string(g.term) + ")";
                };

                std::string g0Str = "-", g1Str = "-", g2Str = "-";
                for (const auto& g : status.groups) {
                    if (g.groupId == 0) g0Str = formatGroup(g);
                    else if (g.groupId == 1) g1Str = formatGroup(g);
                    else if (g.groupId == 2) g2Str = formatGroup(g);
                }

                std::cout << std::setw(18) << g0Str
                          << std::setw(18) << g1Str
                          << std::setw(18) << g2Str
                          << "ONLINE\n";
            }
            std::cout << "----------------------------------------------------------------------------------------\n";
            std::cout << "Cluster Health: " << onlineCount << "/5 Nodes Online | Quorum Requirement: 3/5\n";
            return 0;

        } else if (cfg.command == "faults") {
            if (cfg.args.empty()) {
                std::cerr << "Error: faults requires subaction: block <peer>, unblock <peer>, clear, drop <rate 0..1>, delay <ms>\n";
                return 1;
            }
            AdminFaultReq req;
            std::string sub = cfg.args[0];
            if (sub == "block") {
                if (cfg.args.size() < 2) {
                    std::cerr << "Error: faults block requires <peer>\n";
                    return 1;
                }
                req.action = "block";
                req.peer = cfg.args[1];
            } else if (sub == "unblock") {
                if (cfg.args.size() < 2) {
                    std::cerr << "Error: faults unblock requires <peer>\n";
                    return 1;
                }
                req.action = "unblock";
                req.peer = cfg.args[1];
            } else if (sub == "clear") {
                req.action = "clear";
            } else if (sub == "drop") {
                if (cfg.args.size() < 2) {
                    std::cerr << "Error: faults drop requires <rate>\n";
                    return 1;
                }
                req.action = "set_drop";
                req.dropRate = std::stod(cfg.args[1]);
            } else if (sub == "delay") {
                if (cfg.args.size() < 2) {
                    std::cerr << "Error: faults delay requires <ms>\n";
                    return 1;
                }
                req.action = "set_delay";
                req.delayMs = static_cast<uint32_t>(std::stoul(cfg.args[1]));
            } else {
                std::cerr << "Unknown fault subaction: " << sub << "\n";
                return 1;
            }

            std::vector<uint8_t> payload;
            encodeAdminFaultReq(req, payload);

            Frame resp;
            if (!client.callSync(kMsgAdminFaultReq, payload, resp, 2000)) {
                std::cerr << "Error: fault request timed out\n";
                return 1;
            }
            AdminFaultResp r;
            decodeAdminFaultResp(resp.payload.data(), resp.payload.size(), r);
            if (r.ok) {
                std::cout << "OK (" << r.message << ")\n";
                return 0;
            } else {
                std::cerr << "Error: " << r.message << "\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown command: " << cfg.command << "\n";
            return 1;
        }
    }

    std::cerr << "Error: exceeded maximum redirects\n";
    return 1;
}
