#include "server/server.h"
#include "common/logger.h"

#include <chrono>

namespace aegis {

Server::Server(ServerOptions opts)
    : opts_(std::move(opts)),
      faults_(std::make_shared<Faults>()),
      pool_(opts_.workerThreads) {
    transport_ = std::make_unique<Transport>(opts_.nodeId, faults_);
    for (const auto& peer : opts_.peers) {
        if (peer.id != opts_.nodeId) {
            transport_->addPeer(peer.id, peer.host, peer.peerPort);
        }
    }

    // Initialize consistent hash ring
    for (int g = 0; g < opts_.numGroups; ++g) {
        ring_.addGroup(g, opts_.vnodesPerGroup);
    }
}

Server::~Server() {
    stop();
}

void Server::initGroups() {
    std::vector<NodeId> peers;
    for (const auto& p : opts_.peers) {
        if (p.id != opts_.nodeId) {
            peers.push_back(p.id);
        }
    }

    groups_.clear();
    groups_.reserve(opts_.numGroups);

    for (int g = 0; g < opts_.numGroups; ++g) {
        auto replica = std::make_unique<RaftGroupReplica>();
        replica->groupId = g;
        replica->kv = std::make_unique<KvStore>();

        // Preferred leader bias: group g prefers node g + 1
        uint32_t minElection = 150;
        uint32_t maxElection = 300;
        if (opts_.nodeId == static_cast<NodeId>(g + 1)) {
            minElection = 100;
            maxElection = 180;
        } else {
            minElection = 220;
            maxElection = 350;
        }

        replica->raft = std::make_unique<RaftNode>(
            opts_.nodeId,
            peers,
            minElection,
            maxElection,
            50,
            static_cast<uint32_t>(opts_.nodeId * 100 + g)
        );

        groups_.push_back(std::move(replica));
    }
}

bool Server::start() {
    initGroups();
    running_ = true;

    // Start peer server for Raft RPCs
    bool pStarted = peerServer_.start(opts_.peerPort, [this](std::shared_ptr<TcpConnection> conn, Frame f) {
        pool_.enqueue([this, conn, f = std::move(f)]() mutable {
            handlePeerFrame(conn, std::move(f));
        });
    }, "0.0.0.0");
    if (!pStarted) {
        LOG_ERROR("Server %u: failed to start peer server on port %d", static_cast<unsigned>(opts_.nodeId), opts_.peerPort);
        stop();
        return false;
    }

    // Start client server for KV requests
    bool cStarted = clientServer_.start(opts_.clientPort, [this](std::shared_ptr<TcpConnection> conn, Frame f) {
        pool_.enqueue([this, conn, f = std::move(f)]() mutable {
            handleClientFrame(conn, std::move(f));
        });
    }, "0.0.0.0");
    if (!cStarted) {
        LOG_ERROR("Server %u: failed to start client server on port %d", static_cast<unsigned>(opts_.nodeId), opts_.clientPort);
        stop();
        return false;
    }

    tickThread_ = std::thread(&Server::tickLoop, this);
    applyThread_ = std::thread(&Server::applyLoop, this);

    LOG_INFO("Server %u started (clientPort=%d, peerPort=%d, groups=%d)",
             static_cast<unsigned>(opts_.nodeId), clientServer_.port(), peerServer_.port(), opts_.numGroups);
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;

    clientServer_.stop();
    peerServer_.stop();

    if (tickThread_.joinable()) tickThread_.join();
    if (applyThread_.joinable()) applyThread_.join();

    pool_.stop();
    if (transport_) transport_->close();

    // Cancel any waiting promises
    for (auto& rep : groups_) {
        std::lock_guard<std::mutex> lk(rep->mu);
        for (auto& [idx, prom] : rep->waiters) {
            try {
                prom.set_value(KvResult{false, "", 0});
            } catch (...) {}
        }
        rep->waiters.clear();
    }
}

std::string Server::resolveClientAddr(NodeId id) const {
    for (const auto& p : opts_.peers) {
        if (p.id == id) {
            return p.host + ":" + std::to_string(p.clientPort);
        }
    }
    return "";
}

void Server::tickLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!running_) break;

        for (size_t g = 0; g < groups_.size(); ++g) {
            auto& rep = groups_[g];
            std::vector<Message> msgs;
            {
                std::lock_guard<std::mutex> lk(rep->mu);
                msgs = rep->raft->tick(10);
            }

            for (auto& m : msgs) {
                pool_.enqueue([this, g, m = std::move(m)]() {
                    if (std::holds_alternative<RequestVoteArgs>(m.payload)) {
                        const auto& args = std::get<RequestVoteArgs>(m.payload);
                        RequestVoteReply reply;
                        if (transport_->requestVote(m.to, args, reply, 800)) {
                            std::lock_guard<std::mutex> lk(groups_[g]->mu);
                            Message respMsg{m.to, opts_.nodeId, reply};
                            groups_[g]->raft->step(respMsg);
                        }
                    } else if (std::holds_alternative<AppendEntriesArgs>(m.payload)) {
                        const auto& args = std::get<AppendEntriesArgs>(m.payload);
                        AppendEntriesReply reply;
                        if (transport_->appendEntries(m.to, args, reply, 800)) {
                            std::lock_guard<std::mutex> lk(groups_[g]->mu);
                            Message respMsg{m.to, opts_.nodeId, reply};
                            groups_[g]->raft->step(respMsg);
                        }
                    } else if (std::holds_alternative<InstallSnapshotArgs>(m.payload)) {
                        const auto& args = std::get<InstallSnapshotArgs>(m.payload);
                        InstallSnapshotReply reply;
                        if (transport_->installSnapshot(m.to, args, reply, 2000)) {
                            std::lock_guard<std::mutex> lk(groups_[g]->mu);
                            Message respMsg{m.to, opts_.nodeId, reply};
                            groups_[g]->raft->step(respMsg);
                        }
                    }
                });
            }
        }
    }
}

void Server::applyLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!running_) break;

        for (size_t g = 0; g < groups_.size(); ++g) {
            auto& rep = groups_[g];
            std::vector<LogEntry> applied;
            std::optional<InstalledSnapshot> snap;
            {
                std::lock_guard<std::mutex> lk(rep->mu);
                snap = rep->raft->takeInstalledSnapshot();
                applied = rep->raft->takeApplied();
            }

            if (snap.has_value()) {
                rep->kv->restore(snap->data.data(), snap->data.size());
            }

            for (const auto& entry : applied) {
                KvCommand cmd;
                KvResult res;
                if (KvCommand::decode(entry.data.data(), entry.data.size(), cmd)) {
                    res = rep->kv->apply(cmd);
                }

                std::lock_guard<std::mutex> lk(rep->mu);
                auto it = rep->waiters.find(entry.index);
                if (it != rep->waiters.end()) {
                    it->second.set_value(res);
                    rep->waiters.erase(it);
                }
            }

            // Check snapshot compaction threshold
            if (rep->raft->lastApplied() - rep->raft->snapshotIndex() >= opts_.snapshotThreshold) {
                auto snapBytes = rep->kv->snapshot();
                std::lock_guard<std::mutex> lk(rep->mu);
                rep->raft->compact(rep->raft->lastApplied(), snapBytes);
            }
        }
    }
}

KvResult Server::propose(int groupId, const KvCommand& cmd, uint32_t timeoutMs) {
    if (groupId < 0 || static_cast<size_t>(groupId) >= groups_.size()) {
        return KvResult{false, "", 0};
    }
    auto& rep = groups_[groupId];
    std::future<KvResult> fut;
    LogIndex targetIdx = 0;

    {
        std::lock_guard<std::mutex> lk(rep->mu);
        if (!rep->raft->isLeader()) {
            return KvResult{false, "", 0};
        }
        auto bytes = cmd.encode();
        targetIdx = rep->raft->lastLogIndex() + 1;
        fut = rep->waiters[targetIdx].get_future();
        if (!rep->raft->propose(bytes)) {
            rep->waiters.erase(targetIdx);
            return KvResult{false, "", 0};
        }

        auto pending = rep->raft->takePending();
        for (auto& m : pending) {
            pool_.enqueue([this, groupId, m = std::move(m)]() {
                if (std::holds_alternative<AppendEntriesArgs>(m.payload)) {
                    const auto& args = std::get<AppendEntriesArgs>(m.payload);
                    AppendEntriesReply reply;
                    if (transport_->appendEntries(m.to, args, reply, 800)) {
                        std::lock_guard<std::mutex> lk(groups_[groupId]->mu);
                        Message respMsg{m.to, opts_.nodeId, reply};
                        groups_[groupId]->raft->step(respMsg);
                    }
                }
            });
        }
    }

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
        try {
            return fut.get();
        } catch (...) {
            return KvResult{false, "", 0};
        }
    }

    {
        std::lock_guard<std::mutex> lk(rep->mu);
        rep->waiters.erase(targetIdx);
    }
    return KvResult{false, "", 0};
}

KvResult Server::read(int groupId, const std::string& key, bool stale, uint32_t timeoutMs) {
    if (groupId < 0 || static_cast<size_t>(groupId) >= groups_.size()) {
        return KvResult{false, "", 0};
    }
    auto& rep = groups_[groupId];

    if (stale) {
        auto val = rep->kv->get(key);
        if (val.has_value()) {
            return KvResult{true, *val, rep->kv->version(key)};
        }
        return KvResult{false, "", 0};
    }

    // Linearizable ReadIndex
    uint64_t readId = 0;
    {
        std::lock_guard<std::mutex> lk(rep->mu);
        if (!rep->raft->isLeader()) {
            return KvResult{false, "", 0};
        }
        readId = rep->raft->requestRead();
        if (readId == 0) {
            return KvResult{false, "", 0};
        }
        auto pending = rep->raft->takePending();
        for (auto& m : pending) {
            pool_.enqueue([this, groupId, m = std::move(m)]() {
                if (std::holds_alternative<AppendEntriesArgs>(m.payload)) {
                    const auto& args = std::get<AppendEntriesArgs>(m.payload);
                    AppendEntriesReply reply;
                    if (transport_->appendEntries(m.to, args, reply, 800)) {
                        std::lock_guard<std::mutex> lk(groups_[groupId]->mu);
                        Message respMsg{m.to, opts_.nodeId, reply};
                        groups_[groupId]->raft->step(respMsg);
                    }
                }
            });
        }
    }

    // Wait until readState is ready and lastApplied >= readState.index
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        LogIndex readIndex = 0;
        bool confirmed = false;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            auto readStates = rep->raft->takeReadStates();
            for (const auto& rs : readStates) {
                if (rs.id == readId) {
                    readIndex = rs.index;
                    confirmed = true;
                    break;
                }
            }
            if (confirmed && rep->raft->lastApplied() >= readIndex) {
                auto val = rep->kv->get(key);
                if (val.has_value()) {
                    return KvResult{true, *val, rep->kv->version(key)};
                }
                return KvResult{false, "", 0};
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return KvResult{false, "", 0};
}

ClusterStatusResp Server::getClusterStatus() {
    ClusterStatusResp resp;
    resp.nodeId = opts_.nodeId;
    for (const auto& rep : groups_) {
        GroupStatus gs;
        gs.groupId = rep->groupId;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            gs.role = rep->raft->role();
            gs.term = rep->raft->currentTerm();
            gs.leaderId = rep->raft->leaderId();
            gs.commitIndex = rep->raft->commitIndex();
            gs.lastApplied = rep->raft->lastApplied();
        }
        resp.groups.push_back(gs);
    }
    return resp;
}

void Server::handleClientFrame(std::shared_ptr<TcpConnection> conn, Frame frame) {
    if (frame.msgType == kMsgKvGetReq) {
        KvGetReq req;
        if (!decodeKvGetReq(frame.payload.data(), frame.payload.size(), req)) return;
        int g = ring_.groupFor(req.key);
        auto& rep = groups_[g];

        bool isLeader = false;
        NodeId leader = kNone;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            isLeader = rep->raft->isLeader();
            leader = rep->raft->leaderId();
        }

        KvGetResp resp;
        if (!req.stale && !isLeader) {
            resp.status = StatusCode::kNotLeader;
            resp.leaderId = leader;
            resp.leaderAddr = resolveClientAddr(leader);
        } else {
            KvResult res = read(g, req.key, req.stale);
            if (res.ok) {
                resp.status = StatusCode::kOk;
                resp.value = res.value;
                resp.version = res.version;
            } else {
                resp.status = StatusCode::kKeyNotFound;
            }
        }
        std::vector<uint8_t> payload;
        encodeKvGetResp(resp, payload);
        conn->send(kMsgKvGetResp, frame.requestId, payload.data(), payload.size());

    } else if (frame.msgType == kMsgKvPutReq) {
        KvPutReq req;
        if (!decodeKvPutReq(frame.payload.data(), frame.payload.size(), req)) return;
        int g = ring_.groupFor(req.key);
        auto& rep = groups_[g];

        bool isLeader = false;
        NodeId leader = kNone;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            isLeader = rep->raft->isLeader();
            leader = rep->raft->leaderId();
        }

        KvPutResp resp;
        if (!isLeader) {
            resp.status = StatusCode::kNotLeader;
            resp.leaderId = leader;
            resp.leaderAddr = resolveClientAddr(leader);
        } else {
            uint64_t session = 0;
            try { session = std::stoull(req.clientId); } catch (...) {}
            KvCommand cmd{KvCommandType::kPut, session, req.seq, req.key, req.value};
            KvResult res = propose(g, cmd);
            if (res.ok) {
                resp.status = StatusCode::kOk;
                resp.version = res.version;
            } else {
                resp.status = StatusCode::kTimeout;
            }
        }
        std::vector<uint8_t> payload;
        encodeKvPutResp(resp, payload);
        conn->send(kMsgKvPutResp, frame.requestId, payload.data(), payload.size());

    } else if (frame.msgType == kMsgKvDelReq) {
        KvDelReq req;
        if (!decodeKvDelReq(frame.payload.data(), frame.payload.size(), req)) return;
        int g = ring_.groupFor(req.key);
        auto& rep = groups_[g];

        bool isLeader = false;
        NodeId leader = kNone;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            isLeader = rep->raft->isLeader();
            leader = rep->raft->leaderId();
        }

        KvDelResp resp;
        if (!isLeader) {
            resp.status = StatusCode::kNotLeader;
            resp.leaderId = leader;
            resp.leaderAddr = resolveClientAddr(leader);
        } else {
            uint64_t session = 0;
            try { session = std::stoull(req.clientId); } catch (...) {}
            KvCommand cmd{KvCommandType::kDelete, session, req.seq, req.key, ""};
            KvResult res = propose(g, cmd);
            resp.status = StatusCode::kOk;
            resp.deleted = res.ok;
        }
        std::vector<uint8_t> payload;
        encodeKvDelResp(resp, payload);
        conn->send(kMsgKvDelResp, frame.requestId, payload.data(), payload.size());

    } else if (frame.msgType == kMsgKvCasReq) {
        KvCasReq req;
        if (!decodeKvCasReq(frame.payload.data(), frame.payload.size(), req)) return;
        int g = ring_.groupFor(req.key);
        auto& rep = groups_[g];

        bool isLeader = false;
        NodeId leader = kNone;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            isLeader = rep->raft->isLeader();
            leader = rep->raft->leaderId();
        }

        KvCasResp resp;
        if (!isLeader) {
            resp.status = StatusCode::kNotLeader;
            resp.leaderId = leader;
            resp.leaderAddr = resolveClientAddr(leader);
        } else {
            uint64_t session = 0;
            try { session = std::stoull(req.clientId); } catch (...) {}
            KvCommand cmd{KvCommandType::kCas, session, req.seq, req.key, req.value, req.expectedVersion};
            KvResult res = propose(g, cmd);
            if (res.ok) {
                resp.status = StatusCode::kOk;
                resp.success = true;
                resp.version = res.version;
            } else {
                resp.status = StatusCode::kCasMismatch;
                resp.success = false;
                resp.version = res.version;
            }
        }
        std::vector<uint8_t> payload;
        encodeKvCasResp(resp, payload);
        conn->send(kMsgKvCasResp, frame.requestId, payload.data(), payload.size());

    } else if (frame.msgType == kMsgAdminStatusReq) {
        auto status = getClusterStatus();
        std::vector<uint8_t> payload;
        encodeClusterStatusResp(status, payload);
        conn->send(kMsgAdminStatusResp, frame.requestId, payload.data(), payload.size());
    }
}

void Server::handlePeerFrame(std::shared_ptr<TcpConnection> conn, Frame frame) {
    if (frame.payload.size() < sizeof(uint32_t)) return;

    // First 4 bytes of peer frames identify the group index
    ByteReader br(frame.payload.data(), frame.payload.size());
    uint32_t groupId = 0;
    if (!br.getU32(groupId) || groupId >= groups_.size()) return;

    auto& rep = groups_[groupId];
    const uint8_t* raftData = frame.payload.data() + sizeof(uint32_t);
    size_t raftLen = frame.payload.size() - sizeof(uint32_t);

    if (frame.msgType == kMsgRequestVoteReq) {
        RequestVoteArgs args;
        if (!decodeRequestVoteArgs(raftData, raftLen, args)) return;

        Message m{args.candidateId, opts_.nodeId, args};
        std::vector<Message> out;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            out = rep->raft->step(m);
        }
        for (const auto& resp : out) {
            if (std::holds_alternative<RequestVoteReply>(resp.payload)) {
                std::vector<uint8_t> payload;
                encodeRequestVoteReply(std::get<RequestVoteReply>(resp.payload), payload);
                conn->send(kMsgRequestVoteResp, frame.requestId, payload.data(), payload.size());
            }
        }
    } else if (frame.msgType == kMsgAppendEntriesReq) {
        AppendEntriesArgs args;
        if (!decodeAppendEntriesArgs(raftData, raftLen, args)) return;

        Message m{args.leaderId, opts_.nodeId, args};
        std::vector<Message> out;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            out = rep->raft->step(m);
        }
        for (const auto& resp : out) {
            if (std::holds_alternative<AppendEntriesReply>(resp.payload)) {
                std::vector<uint8_t> payload;
                encodeAppendEntriesReply(std::get<AppendEntriesReply>(resp.payload), payload);
                conn->send(kMsgAppendEntriesResp, frame.requestId, payload.data(), payload.size());
            }
        }
    } else if (frame.msgType == kMsgInstallSnapshotReq) {
        InstallSnapshotArgs args;
        if (!decodeInstallSnapshotArgs(raftData, raftLen, args)) return;

        Message m{args.leaderId, opts_.nodeId, args};
        std::vector<Message> out;
        {
            std::lock_guard<std::mutex> lk(rep->mu);
            out = rep->raft->step(m);
        }
        for (const auto& resp : out) {
            if (std::holds_alternative<InstallSnapshotReply>(resp.payload)) {
                std::vector<uint8_t> payload;
                encodeInstallSnapshotReply(std::get<InstallSnapshotReply>(resp.payload), payload);
                conn->send(kMsgInstallSnapshotResp, frame.requestId, payload.data(), payload.size());
            }
        }
    }
}

}  // namespace aegis
