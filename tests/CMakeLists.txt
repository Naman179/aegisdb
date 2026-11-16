add_executable(aegis_tests
    common/codec_test.cpp
    common/crc32c_test.cpp
    common/logger_test.cpp
)

target_include_directories(aegis_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(aegis_tests
    PRIVATE
    aegisdb_common
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(aegis_tests)
