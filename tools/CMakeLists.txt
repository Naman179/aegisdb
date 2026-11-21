# ─────────────────────────────────────────────────────────────────────────────
# tools/CMakeLists.txt – CLI and benchmark executables
# ─────────────────────────────────────────────────────────────────────────────
add_executable(aegisctl
    aegisctl.cpp
)
target_include_directories(aegisctl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aegisctl PRIVATE aegisdb_server aegisdb_net aegisdb_common)

add_executable(aegis_bench
    aegis_bench.cpp
)
target_include_directories(aegis_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aegis_bench PRIVATE aegisdb_server aegisdb_net aegisdb_common)

add_executable(rebalance
    rebalance.cpp
)
target_include_directories(rebalance PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rebalance PRIVATE aegisdb_server aegisdb_shard aegisdb_net aegisdb_common)
