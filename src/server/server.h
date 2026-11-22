#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// server.h – Multi-group AegisDB Server (Milestones 9 & 10)
//
// Hosts 3 Raft groups per node, routes client keys via HashRing, manages
// WAL persistence, KvStore state machines, background tick/apply threads,
// and client/peer TCP servers.
// ─────────────────────────────────────────────────────────────────────────────
#include "kv/kv_store.h"
#include "net/faults.h"
#include "net/tcp_server.h"
#include "net/transport.h"
#include "raft/raft_node.h"
#include "server/client_protocol.h"
#include "server/thread_pool.h"
#include "shard/hash_ring.h"
#include "wal/wal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aegis {

struct NodePeerInfo {
    NodeId      id{kNone};
    std::string host;
    int         clientPort{0};
    int         peerPort{0};
};

struct ServerOptions {
    NodeId                    nodeId{1};
    int                       clientPort{7001};
    int                       peerPort{8001};
    std::string               dataDir{"./data"};
    std::vector<NodePeerInfo> peers;
    int                       numGroups{3};
    int                       vnodesPerGroup{128};
    size_t                    workerThreads{16};
    uint64_t                  snapshotThreshold{100000};
};

struct RaftGroupReplica {
    int                           groupId{0};
    std::unique_ptr<RaftNode>     raft;
    std::unique_ptr<KvStore>      kv;
    std::mutex                    mu;
    std::unordered_map<LogIndex, std::promise<KvResult>> waiters;
};

class Server {
public:
    explicit Server(ServerOptions opts);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Start all Raft groups, acceptors, and background threads.
    bool start();

    // Gracefully stop all components.
    void stop();

    NodeId nodeId() const { return opts_.nodeId; }
    int clientPort() const { return clientServer_.port(); }
    int peerPort() const { return peerServer_.port(); }
    bool isRunning() const { return running_; }

    std::shared_ptr<Faults> faults() const { return faults_; }
    HashRing& ring() { return ring_; }

    // Propose an operation to a specific group
    KvResult propose(int groupId, const KvCommand& cmd, uint32_t timeoutMs = 2000);

    // Read an operation from a specific group
    KvResult read(int groupId, const std::string& key, bool stale = false, uint32_t timeoutMs = 2000);

    // Get status of all hosted groups
    ClusterStatusResp getClusterStatus();

private:
    void initGroups();
    void handleClientFrame(std::shared_ptr<TcpConnection> conn, Frame frame);
    void handlePeerFrame(std::shared_ptr<TcpConnection> conn, Frame frame);

    void tickLoop();
    void applyLoop();
    void sendGroupMessages(size_t g, std::vector<Message> msgs);

    std::string resolveClientAddr(NodeId id) const;

    ServerOptions                               opts_;
    std::atomic<bool>                           running_{false};
    HashRing                                    ring_;
    std::shared_ptr<Faults>                     faults_;
    std::unique_ptr<Transport>                  transport_;
    ThreadPool                                  pool_;

    TcpServer                                   clientServer_;
    TcpServer                                   peerServer_;

    std::vector<std::unique_ptr<RaftGroupReplica>> groups_;
    std::mutex                                  groupsMu_;

    std::thread                                 tickThread_;
    std::thread                                 applyThread_;
};

}  // namespace aegis
