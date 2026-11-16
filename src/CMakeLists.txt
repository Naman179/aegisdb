add_library(aegisdb_common STATIC
    common/codec.cpp
    common/crc32c.cpp
    common/logger.cpp
)

target_include_directories(aegisdb_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(aegisdb_common PUBLIC Threads::Threads)
