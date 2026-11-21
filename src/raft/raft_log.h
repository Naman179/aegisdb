#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// raft_log.h – Raft replicated log with snapshot-based compaction
//
// The log is 1-indexed. entries_[0] is a sentinel holding the last compacted
// (snapshot) index/term, so termAt(i) is entries_[i - base_] and most
// off-by-one cases after compaction disappear. Before any snapshot the
// sentinel is {term 0, index 0}, so prevLogIndex=0 always matches.
// ─────────────────────────────────────────────────────────────────────────────
#include "raft/raft_types.h"
#include <vector>

namespace aegis {

class RaftLog {
public:
    RaftLog();

    // ── Leader API ────────────────────────────────────────────────────────────
    // Append a new entry (leader only). Returns the new entry's index.
    LogIndex append(Term term, std::vector<uint8_t> data, bool noop = false);

    // ── Follower API ──────────────────────────────────────────────────────────
    // Try to append `entries` after prevLogIndex/prevLogTerm.
    // Returns false (log mismatch) without modifying the log on failure.
    // Entries already covered by the snapshot are skipped.
    bool tryAppend(LogIndex prevLogIndex, Term prevLogTerm,
                   const std::vector<LogEntry>& entries);

    // ── Accessors ─────────────────────────────────────────────────────────────
    LogIndex lastIndex() const;
    Term     lastTerm()  const;
    Term     termAt(LogIndex idx) const;   // 0 if compacted away / out of range
    const LogEntry* entryAt(LogIndex idx) const;  // nullptr if not in the log

    // First index whose term equals termAt(idx), scanning backwards from idx.
    LogIndex firstIndexOfTerm(LogIndex idx) const;
    // Highest index holding `term`, or 0 if the log has no entry of that term.
    LogIndex lastIndexOfTerm(Term term) const;

    // Returns entries in [from, lastIndex]; `from` is clamped above the snapshot.
    std::vector<LogEntry> entriesFrom(LogIndex from) const;

    // ── Commit / apply ────────────────────────────────────────────────────────
    // Advance commitIndex (monotonically). Clamped to lastIndex.
    void     setCommitIndex(LogIndex idx);
    LogIndex commitIndex() const { return commitIndex_; }
    LogIndex lastApplied() const { return lastApplied_; }

    // Pop entries in (lastApplied, commitIndex] ready to be applied.
    std::vector<LogEntry> takeApplicable();

    // ── Snapshots / compaction ────────────────────────────────────────────────
    LogIndex snapshotIndex() const { return base_; }
    Term     snapshotTerm()  const { return entries_[0].term; }

    // Drop every entry up to and including `index` (must be applied).
    // Returns false if `index` is not an applied index inside the log.
    bool compactTo(LogIndex index);

    // Replace the log prefix with a snapshot received from the leader. If we
    // already hold a matching entry at `index` the suffix after it is kept.
    void installSnapshot(LogIndex index, Term term);

private:
    size_t pos(LogIndex idx) const { return static_cast<size_t>(idx - base_); }
    bool   inLog(LogIndex idx) const {
        return idx >= base_ && idx <= base_ + entries_.size() - 1;
    }

    std::vector<LogEntry> entries_;    // entries_[0] = sentinel (snapshot point)
    LogIndex              base_{0};    // index of the sentinel
    LogIndex              commitIndex_{0};
    LogIndex              lastApplied_{0};
};

}  // namespace aegis
