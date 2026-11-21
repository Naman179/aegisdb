#include "net/raft_codec.h"

namespace aegis {

void encodeRequestVoteArgs(const RequestVoteArgs& args, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU64(args.term);
    w.putU32(args.candidateId);
    w.putU64(args.lastLogIndex);
    w.putU64(args.lastLogTerm);
    w.putU8(args.preVote ? 1 : 0);
    out = w.take();
}

bool decodeRequestVoteArgs(const uint8_t* data, size_t len, RequestVoteArgs& out) {
    ByteReader r(data, len);
    uint8_t pv = 0;
    if (!r.getU64(out.term)) return false;
    if (!r.getU32(out.candidateId)) return false;
    if (!r.getU64(out.lastLogIndex)) return false;
    if (!r.getU64(out.lastLogTerm)) return false;
    if (!r.getU8(pv)) return false;
    out.preVote = (pv != 0);
    return true;
}

void encodeRequestVoteReply(const RequestVoteReply& reply, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU64(reply.term);
    w.putU8(reply.voteGranted ? 1 : 0);
    w.putU8(reply.preVote ? 1 : 0);
    out = w.take();
}

bool decodeRequestVoteReply(const uint8_t* data, size_t len, RequestVoteReply& out) {
    ByteReader r(data, len);
    uint8_t vg = 0, pv = 0;
    if (!r.getU64(out.term)) return false;
    if (!r.getU8(vg)) return false;
    if (!r.getU8(pv)) return false;
    out.voteGranted = (vg != 0);
    out.preVote = (pv != 0);
    return true;
}

void encodeAppendEntriesArgs(const AppendEntriesArgs& args, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU64(args.term);
    w.putU32(args.leaderId);
    w.putU64(args.prevLogIndex);
    w.putU64(args.prevLogTerm);
    w.putU64(args.leaderCommit);
    w.putU64(args.readCtx);
    w.putU32(static_cast<uint32_t>(args.entries.size()));
    for (const auto& e : args.entries) {
        w.putU64(e.term);
        w.putU64(e.index);
        w.putU8(e.noop ? 1 : 0);
        w.putU32(static_cast<uint32_t>(e.data.size()));
        w.putBytes(e.data.data(), e.data.size());
    }
    out = w.take();
}

bool decodeAppendEntriesArgs(const uint8_t* data, size_t len, AppendEntriesArgs& out) {
    ByteReader r(data, len);
    if (!r.getU64(out.term)) return false;
    if (!r.getU32(out.leaderId)) return false;
    if (!r.getU64(out.prevLogIndex)) return false;
    if (!r.getU64(out.prevLogTerm)) return false;
    if (!r.getU64(out.leaderCommit)) return false;
    if (!r.getU64(out.readCtx)) return false;

    uint32_t count = 0;
    if (!r.getU32(count)) return false;
    out.entries.clear();
    out.entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        LogEntry e;
        uint8_t noop = 0;
        uint32_t dataLen = 0;
        if (!r.getU64(e.term)) return false;
        if (!r.getU64(e.index)) return false;
        if (!r.getU8(noop)) return false;
        e.noop = (noop != 0);
        if (!r.getU32(dataLen)) return false;
        e.data.resize(dataLen);
        if (dataLen > 0 && !r.getBytes(e.data.data(), dataLen)) return false;
        out.entries.push_back(std::move(e));
    }
    return true;
}

void encodeAppendEntriesReply(const AppendEntriesReply& reply, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU64(reply.term);
    w.putU8(reply.success ? 1 : 0);
    w.putU64(reply.matchIndex);
    w.putU32(reply.followerId);
    w.putU64(reply.conflictIndex);
    w.putU64(reply.conflictTerm);
    w.putU64(reply.readCtx);
    out = w.take();
}

bool decodeAppendEntriesReply(const uint8_t* data, size_t len, AppendEntriesReply& out) {
    ByteReader r(data, len);
    uint8_t succ = 0;
    if (!r.getU64(out.term)) return false;
    if (!r.getU8(succ)) return false;
    out.success = (succ != 0);
    if (!r.getU64(out.matchIndex)) return false;
    if (!r.getU32(out.followerId)) return false;
    if (!r.getU64(out.conflictIndex)) return false;
    if (!r.getU64(out.conflictTerm)) return false;
    if (!r.getU64(out.readCtx)) return false;
    return true;
}

void encodeInstallSnapshotArgs(const InstallSnapshotArgs& args, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU64(args.term);
    w.putU32(args.leaderId);
    w.putU64(args.lastIncludedIndex);
    w.putU64(args.lastIncludedTerm);
    w.putU32(static_cast<uint32_t>(args.data.size()));
    w.putBytes(args.data.data(), args.data.size());
    out = w.take();
}

bool decodeInstallSnapshotArgs(const uint8_t* data, size_t len, InstallSnapshotArgs& out) {
    ByteReader r(data, len);
    if (!r.getU64(out.term)) return false;
    if (!r.getU32(out.leaderId)) return false;
    if (!r.getU64(out.lastIncludedIndex)) return false;
    if (!r.getU64(out.lastIncludedTerm)) return false;
    uint32_t dataLen = 0;
    if (!r.getU32(dataLen)) return false;
    out.data.resize(dataLen);
    if (dataLen > 0 && !r.getBytes(out.data.data(), dataLen)) return false;
    return true;
}

void encodeInstallSnapshotReply(const InstallSnapshotReply& reply, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.putU64(reply.term);
    w.putU32(reply.followerId);
    w.putU64(reply.lastIncludedIndex);
    out = w.take();
}

bool decodeInstallSnapshotReply(const uint8_t* data, size_t len, InstallSnapshotReply& out) {
    ByteReader r(data, len);
    if (!r.getU64(out.term)) return false;
    if (!r.getU32(out.followerId)) return false;
    if (!r.getU64(out.lastIncludedIndex)) return false;
    return true;
}

}  // namespace aegis
