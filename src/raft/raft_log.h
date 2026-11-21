#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// raft_log.h – Raft replicated log
//
// The log is 1-indexed.  entries_[0] is a dummy entry with term=0, index=0
// that simplifies boundary conditions (prevLogIndex=0 always matches).
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
    bool tryAppend(LogIndex prevLogIndex, Term prevLogTerm,
                   const std::vector<LogEntry>& entries);

    // ── Accessors ─────────────────────────────────────────────────────────────
    LogIndex lastIndex() const;
    Term     lastTerm()  const;
    Term     termAt(LogIndex idx) const;   // 0 if out of range
    const LogEntry* entryAt(LogIndex idx) const;

    // First index whose term equals termAt(idx), scanning backwards from idx.
    LogIndex firstIndexOfTerm(LogIndex idx) const;
    // Highest index holding `term`, or 0 if the log has no entry of that term.
    LogIndex lastIndexOfTerm(Term term) const;

    // Returns entries in [from, lastIndex], empty if from > lastIndex.
    std::vector<LogEntry> entriesFrom(LogIndex from) const;

    // ── Commit / apply ────────────────────────────────────────────────────────
    // Advance commitIndex (monotonically). Clamped to lastIndex.
    void     setCommitIndex(LogIndex idx);
    LogIndex commitIndex() const { return commitIndex_; }
    LogIndex lastApplied() const { return lastApplied_; }

    // Pop entries in (lastApplied, commitIndex] ready to be applied.
    std::vector<LogEntry> takeApplicable();

private:
    std::vector<LogEntry> entries_;    // entries_[0] = dummy
    LogIndex              commitIndex_{0};
    LogIndex              lastApplied_{0};
};

}  // namespace aegis
