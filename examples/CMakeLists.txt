add_executable(aegis_example_crud basic_crud.cpp)
target_include_directories(aegis_example_crud PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(aegis_example_crud PRIVATE
    aegisdb_common
    aegisdb_wal
    aegisdb_kv
    aegisdb_raft
    aegisdb_shard
    aegisdb_net
    aegisdb_server
)

add_executable(aegis_example_concurrent concurrent_workers.cpp)
target_include_directories(aegis_example_concurrent PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(aegis_example_concurrent PRIVATE
    aegisdb_common
    aegisdb_wal
    aegisdb_kv
    aegisdb_raft
    aegisdb_shard
    aegisdb_net
    aegisdb_server
)
