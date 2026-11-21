#include "net/transport.h"
#include "common/logger.h"

namespace aegis {

Transport::Transport(NodeId selfId, std::shared_ptr<Faults> faults)
    : selfId_(selfId), faults_(faults ? faults : std::make_shared<Faults>()) {}

Transport::~Transport() {
    close();
}

void Transport::close() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, client] : clients_) {
        client->close();
    }
    clients_.clear();
}

void Transport::addPeer(NodeId id, const std::string& host, int port) {
    std::lock_guard<std::mutex> lk(mu_);
    endpoints_[id] = PeerEndpoint{host, port};
}

void Transport::removePeer(NodeId id) {
    std::lock_guard<std::mutex> lk(mu_);
    endpoints_.erase(id);
    auto it = clients_.find(id);
    if (it != clients_.end()) {
        it->second->close();
        clients_.erase(it);
    }
}

RpcClient* Transport::getOrCreateClient(NodeId peer) {
    auto it = clients_.find(peer);
    if (it != clients_.end() && it->second->isConnected()) {
        return it->second.get();
    }

    auto epIt = endpoints_.find(peer);
    if (epIt == endpoints_.end()) {
        return nullptr;
    }

    if (it != clients_.end()) {
        if (!it->second->isConnected()) {
            it->second->connect();
            return it->second->isConnected() ? it->second.get() : nullptr;
        }
        return it->second.get();
    }

    auto client = std::make_unique<RpcClient>(epIt->second.host, epIt->second.port);
    if (!client->connect()) {
        return nullptr;
    }
    RpcClient* ptr = client.get();
    clients_[peer] = std::move(client);
    return ptr;
}

bool Transport::requestVote(NodeId peer, const RequestVoteArgs& args, RequestVoteReply& reply, uint32_t timeoutMs) {
    std::string peerStr = std::to_string(peer);
    if (!faults_->allow(peerStr)) {
        return false;
    }

    RpcClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        client = getOrCreateClient(peer);
    }
    if (!client) return false;

    std::vector<uint8_t> payload;
    encodeRequestVoteArgs(args, payload);

    Frame resp;
    if (!client->callSync(kMsgRequestVoteReq, payload, resp, timeoutMs)) {
        return false;
    }

    if (!faults_->allow(peerStr)) {
        return false;
    }

    return decodeRequestVoteReply(resp.payload.data(), resp.payload.size(), reply);
}

bool Transport::appendEntries(NodeId peer, const AppendEntriesArgs& args, AppendEntriesReply& reply, uint32_t timeoutMs) {
    std::string peerStr = std::to_string(peer);
    if (!faults_->allow(peerStr)) {
        return false;
    }

    RpcClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        client = getOrCreateClient(peer);
    }
    if (!client) return false;

    std::vector<uint8_t> payload;
    encodeAppendEntriesArgs(args, payload);

    Frame resp;
    if (!client->callSync(kMsgAppendEntriesReq, payload, resp, timeoutMs)) {
        return false;
    }

    if (!faults_->allow(peerStr)) {
        return false;
    }

    return decodeAppendEntriesReply(resp.payload.data(), resp.payload.size(), reply);
}

bool Transport::installSnapshot(NodeId peer, const InstallSnapshotArgs& args, InstallSnapshotReply& reply, uint32_t timeoutMs) {
    std::string peerStr = std::to_string(peer);
    if (!faults_->allow(peerStr)) {
        return false;
    }

    RpcClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        client = getOrCreateClient(peer);
    }
    if (!client) return false;

    std::vector<uint8_t> payload;
    encodeInstallSnapshotArgs(args, payload);

    Frame resp;
    if (!client->callSync(kMsgInstallSnapshotReq, payload, resp, timeoutMs)) {
        return false;
    }

    if (!faults_->allow(peerStr)) {
        return false;
    }

    return decodeInstallSnapshotReply(resp.payload.data(), resp.payload.size(), reply);
}

}  // namespace aegis
