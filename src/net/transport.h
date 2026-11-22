#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// transport.h – Network transport for Raft RPCs (Milestone 8)
//
// Manages pooled peer connections using RpcClient, serializes Raft RPCs
// using raft_codec, and consults Faults for fault injection.
// ─────────────────────────────────────────────────────────────────────────────
#include "net/faults.h"
#include "net/raft_codec.h"
#include "net/rpc_client.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace aegis {

struct PeerEndpoint {
    std::string host;
    int         port{0};
};

class Transport {
public:
    explicit Transport(NodeId selfId, std::shared_ptr<Faults> faults = nullptr);
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    void addPeer(NodeId id, const std::string& host, int port);
    void removePeer(NodeId id);

    bool requestVote(uint32_t groupId, NodeId peer, const RequestVoteArgs& args, RequestVoteReply& reply, uint32_t timeoutMs = 1000);
    bool appendEntries(uint32_t groupId, NodeId peer, const AppendEntriesArgs& args, AppendEntriesReply& reply, uint32_t timeoutMs = 1000);
    bool installSnapshot(uint32_t groupId, NodeId peer, const InstallSnapshotArgs& args, InstallSnapshotReply& reply, uint32_t timeoutMs = 5000);

    // Convenience overloads defaulting to group 0
    bool requestVote(NodeId peer, const RequestVoteArgs& args, RequestVoteReply& reply, uint32_t timeoutMs = 1000) {
        return requestVote(0, peer, args, reply, timeoutMs);
    }
    bool appendEntries(NodeId peer, const AppendEntriesArgs& args, AppendEntriesReply& reply, uint32_t timeoutMs = 1000) {
        return appendEntries(0, peer, args, reply, timeoutMs);
    }
    bool installSnapshot(NodeId peer, const InstallSnapshotArgs& args, InstallSnapshotReply& reply, uint32_t timeoutMs = 5000) {
        return installSnapshot(0, peer, args, reply, timeoutMs);
    }

    std::shared_ptr<Faults> faults() const { return faults_; }
    NodeId selfId() const { return selfId_; }

    void close();

private:
    RpcClient* getOrCreateClient(NodeId peer);

    NodeId                                                 selfId_;
    std::shared_ptr<Faults>                                faults_;
    std::mutex                                             mu_;
    std::unordered_map<NodeId, PeerEndpoint>               endpoints_;
    std::unordered_map<NodeId, std::unique_ptr<RpcClient>> clients_;
};

}  // namespace aegis
