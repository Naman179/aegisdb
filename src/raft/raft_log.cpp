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
    LogIndex idx = static_cast<LogIndex>(entries_.size());
    entries_.push_back(LogEntry{term, idx, std::move(data), noop});
    return idx;
}

// ── Follower: conditional append (Raft §5.3) ─────────────────────────────────

bool RaftLog::tryAppend(LogIndex prevLogIndex, Term prevLogTerm,
                         const std::vector<LogEntry>& entries) {
    // prevLogIndex must exist in our log and have the right term.
    if (prevLogIndex >= static_cast<LogIndex>(entries_.size())) return false;
    if (entries_[prevLogIndex].term != prevLogTerm)              return false;

    LogIndex writeAt = prevLogIndex + 1;
    for (const auto& e : entries) {
        if (writeAt < static_cast<LogIndex>(entries_.size())) {
            if (entries_[writeAt].term != e.term) {
                // Conflicting entry – truncate from here onwards.
                entries_.erase(
                    entries_.begin() + static_cast<ptrdiff_t>(writeAt),
                    entries_.end());
                entries_.push_back(e);
            }
            // else: entry already matches, nothing to do.
        } else {
            entries_.push_back(e);
        }
        ++writeAt;
    }
    return true;
}

// ── Accessors ─────────────────────────────────────────────────────────────────

LogIndex RaftLog::lastIndex() const {
    return static_cast<LogIndex>(entries_.size() - 1);
}

Term RaftLog::lastTerm() const {
    return entries_.back().term;
}

Term RaftLog::termAt(LogIndex idx) const {
    if (idx >= static_cast<LogIndex>(entries_.size())) return 0;
    return entries_[idx].term;
}

const LogEntry* RaftLog::entryAt(LogIndex idx) const {
    if (idx == 0 || idx >= static_cast<LogIndex>(entries_.size())) return nullptr;
    return &entries_[idx];
}

LogIndex RaftLog::firstIndexOfTerm(LogIndex idx) const {
    if (idx == 0 || idx >= static_cast<LogIndex>(entries_.size())) return idx;
    const Term t = entries_[idx].term;
    while (idx > 1 && entries_[idx - 1].term == t) --idx;
    return idx;
}

LogIndex RaftLog::lastIndexOfTerm(Term term) const {
    for (LogIndex i = lastIndex(); i > 0; --i)
        if (entries_[i].term == term) return i;
    return 0;
}

std::vector<LogEntry> RaftLog::entriesFrom(LogIndex from) const {
    if (from >= static_cast<LogIndex>(entries_.size())) return {};
    return {
        entries_.begin() + static_cast<ptrdiff_t>(from),
        entries_.end()
    };
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
        result.push_back(entries_[i]);
    }
    lastApplied_ = commitIndex_;
    return result;
}

}  // namespace aegis
