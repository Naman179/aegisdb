# AegisDB

A fault-tolerant, sharded key-value store built in C++17 using Raft consensus.

> **Status**: Milestones 1–7 complete (foundations, WAL, KV store, Raft with PreVote / CheckQuorum /
> ReadIndex / snapshots, 23-seed chaos suite). Milestone 8 (frame stream) and 9 (HashRing) are
> partly done; the TCP transport, multi-group server and tooling are still to come.

## Architecture

```mermaid
graph TD
    subgraph Cluster["5-node Cluster"]
        n1 --- n2 --- n3 --- n4 --- n5
    end
    subgraph Raft["3 Raft Groups (RF=5, Quorum=3)"]
        g0["g0 (preferred leader: n1)"]
        g1["g1 (preferred leader: n2)"]
        g2["g2 (preferred leader: n3)"]
    end
    subgraph KV["KV State Machine"]
        PUT & GET & DELETE & CAS
    end
    Client --> HashRing --> Raft --> KV
```

## Build

```bash
# Prerequisites: GCC ≥ 11 or Clang ≥ 14, CMake ≥ 3.20, Ninja
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

ASAN / TSAN builds:
```bash
cmake -S . -B build_asan -DCMAKE_BUILD_TYPE=Debug -DAEGIS_ASAN=ON
cmake -S . -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DAEGIS_TSAN=ON
```

## Milestones

| # | Description | Status |
|---|-------------|--------|
| 1 | CMake skeleton, codec, CRC32-C, logger, GoogleTest | ✅ |
| 2 | WAL | ✅ |
| 3 | KvStore | ✅ |
| 4 | Single-group Raft on FakeNetwork | ✅ |
| 5 | Raft hardening (PreVote, CheckQuorum, no-op, fast backtracking, ReadIndex) | ✅ |
| 6 | Snapshots + InstallSnapshot (log compaction, lagging-follower catch-up) | ✅ |
| 7 | ChaosTest (23 seeds: partitions, 10% drops, stale-leader proposals) | ✅ |
| 8 | TCP layer | 🔶 frame stream reassembly done; `TcpServer` / `RpcClient` / `Transport` pending |
| 9 | Multi-group server, HashRing, aegisd, aegisctl | 🔶 `HashRing` done; server, `aegisd`, `aegisctl` pending |
| 10 | ClusterFaultTest | 🔲 |
| 11 | aegis_bench + tuning | 🔲 |
| 12 | Rebalance | 🔲 |
| 13 | Docker, CI, README | 🔲 |

## Raft design notes

`RaftNode` is a deterministic, single-threaded state machine driven by `tick(ms)` and
`step(msg)`; the harness (or, later, the TCP transport) moves messages. That makes every
fault scenario reproducible from a seed.

| Mechanism | Why it exists |
|-----------|---------------|
| PreVote + leader stickiness | A partitioned node never gathers pre-votes, so its term cannot inflate and unseat a healthy leader when it rejoins |
| CheckQuorum | A leader that has not heard from a majority within one minimum election timeout steps down and stops accepting writes |
| Leader no-op | Committing an entry from the new term commits everything before it (the "Figure 8" rule) and unlocks ReadIndex |
| `conflictTerm` fast backtracking | A rejected AppendEntries lets the leader skip a whole conflicting term in one round trip |
| Commit capped at last verified entry | A follower never commits its own unverified tail just because `leaderCommit` is larger |
| ReadIndex | Linearizable reads: record the commit index, confirm leadership with one heartbeat quorum, then wait for apply |
| Snapshots / InstallSnapshot | The log keeps a sentinel at the snapshot index; followers that need compacted entries receive the snapshot |

## Tests

* `RaftTest` (18), `RaftHardening` (16), `RaftLogSnapshot` + `RaftSnapshot` (10) – unit and
  scenario tests on the in-memory `FakeNetwork`.
* `ChaosTest` (23 seeds) – random isolation, 10% message loss and proposals on stale leaders;
  asserts election safety, committed-entry stability, no duplicate applies and convergence.
* `HashRingTest` (8), `NetTest` (8), `KvStoreTest`, `WalTest`, codec/CRC/logger tests.

The WAL uses POSIX directory `fsync`, so it builds and tests on Linux only.

---

*Full architecture diagram, WAL format, test inventory, measured results, design trade-offs, and known limitations will be added in milestone 13.*
