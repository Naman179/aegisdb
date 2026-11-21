add_library(aegisdb_common STATIC
    common/codec.cpp
    common/crc32c.cpp
    common/logger.cpp
)

target_include_directories(aegisdb_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(aegisdb_common PUBLIC Threads::Threads)

# ── Milestone 2: Write-Ahead Log ──────────────────────────────────────────────
add_library(aegisdb_wal STATIC
    wal/wal.cpp
)
target_include_directories(aegisdb_wal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegisdb_wal PUBLIC aegisdb_common)

# ── Milestone 3: KV State Machine ─────────────────────────────────────────────
add_library(aegisdb_kv STATIC
    kv/kv_store.cpp
)
target_include_directories(aegisdb_kv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegisdb_kv PUBLIC aegisdb_common)

# ── Milestone 4: Raft consensus ───────────────────────────────────────────────
add_library(aegisdb_raft STATIC
    raft/raft_log.cpp
    raft/raft_node.cpp
)
target_include_directories(aegisdb_raft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegisdb_raft PUBLIC aegisdb_common)

# ── Milestone 9: Consistent-hash ring ─────────────────────────────────────────
add_library(aegisdb_shard STATIC
    shard/hash_ring.cpp
)
target_include_directories(aegisdb_shard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ── Milestone 8: Networking (framing, TCP server, RPC client, transport) ────
add_library(aegisdb_net STATIC
    net/frame_stream.cpp
    net/faults.cpp
    net/tcp_server.cpp
    net/rpc_client.cpp
    net/raft_codec.cpp
    net/transport.cpp
)
target_include_directories(aegisdb_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegisdb_net PUBLIC aegisdb_common aegisdb_raft)
if(WIN32)
    target_link_libraries(aegisdb_net PUBLIC ws2_32)
endif()

# ── Milestone 9: Server & Thread Pool ────────────────────────────────────────
add_library(aegisdb_server STATIC
    server/thread_pool.cpp
    server/client_protocol.cpp
    server/server.cpp
)
target_include_directories(aegisdb_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegisdb_server PUBLIC aegisdb_common aegisdb_net aegisdb_shard aegisdb_kv aegisdb_wal aegisdb_raft)

# ── aegisd daemon executable ─────────────────────────────────────────────────
add_executable(aegisd
    main.cpp
)
target_include_directories(aegisd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aegisd PRIVATE aegisdb_server aegisdb_net aegisdb_raft aegisdb_kv aegisdb_wal aegisdb_shard aegisdb_common)
