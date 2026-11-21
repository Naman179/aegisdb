// ─────────────────────────────────────────────────────────────────────────────
// raft_log.cpp – Raft replicated log implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "raft/raft_log.h"
#include <algorithm>
#include <cassert>

namespace aegis {

RaftLog::RaftLog() {
    // Sentinel entry at index 0 (term 0).
    entries_.push_back(LogEntry{0, 0, {}});
}

// ── Leader: unconditional append ─────────────────────────────────────────────

LogIndex RaftLog::append(Term term, std::vector<uint8_t> data, bool noop) {
    LogIndex idx = lastIndex() + 1;
    entries_.push_back(LogEntry{term, idx, std::move(data), noop});
    return idx;
}

// ── Follower: conditional append (Raft §5.3) ─────────────────────────────────

bool RaftLog::tryAppend(LogIndex prevLogIndex, Term prevLogTerm,
                        const std::vector<LogEntry>& entries) {
    size_t skip = 0;
    if (prevLogIndex < base_) {
        // The leader is probing inside a prefix we have already compacted.
        // Committed entries never change, so skip what the snapshot covers.
        while (skip < entries.size() && entries[skip].index <= base_) ++skip;
        prevLogIndex = base_;
        prevLogTerm  = snapshotTerm();
    }

    // prevLogIndex must exist in our log and have the right term.
    if (!inLog(prevLogIndex))                      return false;
    if (entries_[pos(prevLogIndex)].term != prevLogTerm) return false;

    LogIndex writeAt = prevLogIndex + 1;
    for (size_t i = skip; i < entries.size(); ++i, ++writeAt) {
        const LogEntry& e = entries[i];
        if (inLog(writeAt)) {
            if (entries_[pos(writeAt)].term != e.term) {
                // Conflicting entry – truncate from here onwards.
                entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(pos(writeAt)),
                               entries_.end());
                entries_.push_back(e);
            }
            // else: entry already matches, nothing to do.
        } else {
            entries_.push_back(e);
        }
    }
    return true;
}

// ── Accessors ─────────────────────────────────────────────────────────────────

LogIndex RaftLog::lastIndex() const {
    return base_ + static_cast<LogIndex>(entries_.size() - 1);
}

Term RaftLog::lastTerm() const {
    return entries_.back().term;
}

Term RaftLog::termAt(LogIndex idx) const {
    if (!inLog(idx)) return 0;
    return entries_[pos(idx)].term;
}

const LogEntry* RaftLog::entryAt(LogIndex idx) const {
    if (idx == 0 || idx <= base_ || !inLog(idx)) return nullptr;
    return &entries_[pos(idx)];
}

LogIndex RaftLog::firstIndexOfTerm(LogIndex idx) const {
    if (idx == 0 || !inLog(idx)) return idx;
    const Term t = entries_[pos(idx)].term;
    while (idx > base_ + 1 && entries_[pos(idx - 1)].term == t) --idx;
    return idx;
}

LogIndex RaftLog::lastIndexOfTerm(Term term) const {
    for (LogIndex i = lastIndex(); i > base_; --i)
        if (entries_[pos(i)].term == term) return i;
    return 0;
}

std::vector<LogEntry> RaftLog::entriesFrom(LogIndex from) const {
    if (from <= base_) from = base_ + 1;
    if (from > lastIndex()) return {};
    return {entries_.begin() + static_cast<ptrdiff_t>(pos(from)), entries_.end()};
}

// ── Commit / apply ────────────────────────────────────────────────────────────

void RaftLog::setCommitIndex(LogIndex idx) {
    if (idx > commitIndex_) {
        commitIndex_ = std::min(idx, lastIndex());
    }
}

std::vector<LogEntry> RaftLog::takeApplicable() {
    if (commitIndex_ <= lastApplied_) return {};
    std::vector<LogEntry> result;
    result.reserve(static_cast<size_t>(commitIndex_ - lastApplied_));
    for (LogIndex i = lastApplied_ + 1; i <= commitIndex_; ++i) {
        result.push_back(entries_[pos(i)]);
    }
    lastApplied_ = commitIndex_;
    return result;
}

// ── Snapshots / compaction ────────────────────────────────────────────────────

bool RaftLog::compactTo(LogIndex index) {
    if (index <= base_) return true;                // already compacted
    if (index > lastApplied_ || !inLog(index)) return false;

    LogEntry sentinel{entries_[pos(index)].term, index, {}};
    entries_.erase(entries_.begin(),
                   entries_.begin() + static_cast<ptrdiff_t>(pos(index)));
    entries_[0] = std::move(sentinel);
    base_       = index;
    return true;
}

void RaftLog::installSnapshot(LogIndex index, Term term) {
    if (inLog(index) && index > base_ && entries_[pos(index)].term == term) {
        // We already have this entry: keep everything after it.
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<ptrdiff_t>(pos(index)));
    } else {
        entries_.clear();
        entries_.push_back(LogEntry{});
    }
    entries_[0] = LogEntry{term, index, {}};
    base_       = index;
    commitIndex_ = std::max(commitIndex_, index);
    lastApplied_ = std::max(lastApplied_, index);
}

}  // namespace aegis
