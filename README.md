# AegisDB

A fault-tolerant, sharded key-value store built in C++17 using Raft consensus.

> **Status**: Milestone 1 complete – codec, CRC32-C, logger, GoogleTest infrastructure.

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
| 2 | WAL | 🔲 |
| 3 | KvStore | 🔲 |
| 4 | Single-group Raft on FakeNetwork | 🔲 |
| 5 | Raft hardening | 🔲 |
| 6 | Snapshots + InstallSnapshot | 🔲 |
| 7 | ChaosTest | 🔲 |
| 8 | TCP layer | 🔲 |
| 9 | Multi-group server, HashRing, aegisd, aegisctl | 🔲 |
| 10 | ClusterFaultTest | 🔲 |
| 11 | aegis_bench + tuning | 🔲 |
| 12 | Rebalance | 🔲 |
| 13 | Docker, CI, README | 🔲 |

---

*Full architecture diagram, WAL format, test inventory, measured results, design trade-offs, and known limitations will be added in milestone 13.*
