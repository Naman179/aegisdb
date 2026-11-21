# ─────────────────────────────────────────────────────────────────────────────
# tools/CMakeLists.txt – CLI and benchmark executables
# ─────────────────────────────────────────────────────────────────────────────
add_executable(aegisctl
    aegisctl.cpp
)
target_include_directories(aegisctl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aegisctl PRIVATE aegisdb_server aegisdb_net aegisdb_common)
