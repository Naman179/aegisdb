add_executable(aegis_tests
    common/codec_test.cpp
    common/crc32c_test.cpp
    common/logger_test.cpp
    # Milestone 2 – WAL
    wal/wal_test.cpp
    # Milestone 3 – KV state machine
    kv/kv_store_test.cpp
    # Milestone 4 – Raft consensus
    raft/fake_network.h
    raft/raft_test.cpp
    # Milestone 5 – Raft hardening
    raft/raft_hardening_test.cpp
    # Milestone 6 – snapshots
    raft/snapshot_test.cpp
    # Milestone 7 – chaos (23 seeds)
    raft/chaos_test.cpp
    # Milestone 9 – consistent hashing
    shard/hash_ring_test.cpp
)

target_include_directories(aegis_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(aegis_tests
    PRIVATE
    aegisdb_common
    aegisdb_wal
    aegisdb_kv
    aegisdb_raft
    aegisdb_shard
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(aegis_tests)
