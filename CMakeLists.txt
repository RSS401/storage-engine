cmake_minimum_required(VERSION 3.20)

project(
    RohanDB
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
    rohandb
    src/database.cpp
    src/disk_manager.cpp
    src/meta_manager.cpp
    src/database_identity.cpp
    src/slotted_page.cpp
    src/buffer_pool_manager.cpp
    src/record_manager.cpp
    src/bplus_tree.cpp
    src/storage_engine.cpp
    src/crc32c.cpp
    src/range_iterator.cpp
    src/wal_file.cpp
    src/log_manager.cpp
    src/operation_context.cpp
)

target_include_directories(
    rohandb
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_executable(
    rohandb_cli
    app/main.cpp
)

target_link_libraries(
    rohandb_cli
    PRIVATE
        rohandb
)

target_compile_options(rohandb PUBLIC -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
option(SE_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SE_SANITIZE)
    target_compile_options(rohandb PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rohandb PUBLIC -fsanitize=address,undefined)
endif()
enable_testing()

add_executable(
    rohandb_tests
    tests/database_test.cpp
)

target_link_libraries(
    rohandb_tests
    PRIVATE
        rohandb
)

add_test(
    NAME database_test
    COMMAND rohandb_tests
)

add_executable(rohandb_serialization_tests tests/serialization_test.cpp)
target_link_libraries(rohandb_serialization_tests PRIVATE rohandb)
add_test(NAME serialization_test COMMAND rohandb_serialization_tests)

add_executable(rohandb_storage_tests tests/storage_test.cpp)
target_link_libraries(rohandb_storage_tests PRIVATE rohandb)
add_test(NAME storage_test COMMAND rohandb_storage_tests)

add_executable(rohandb_buffer_tests tests/buffer_test.cpp)
target_link_libraries(rohandb_buffer_tests PRIVATE rohandb)
add_test(NAME buffer_test COMMAND rohandb_buffer_tests)

add_executable(rohandb_engine_tests tests/engine_test.cpp)
target_link_libraries(rohandb_engine_tests PRIVATE rohandb)
add_test(NAME engine_test COMMAND rohandb_engine_tests)

add_executable(db_inspect tools/db_inspect.cpp)
target_link_libraries(db_inspect PRIVATE rohandb)
add_executable(rohandb_benchmark tools/benchmark.cpp)
target_link_libraries(rohandb_benchmark PRIVATE rohandb)

option(SE_CHECKSUMS "Enable milestone 2 checksum format" ON)
if(SE_CHECKSUMS)
    target_compile_definitions(rohandb PUBLIC ROHANDB_CHECKSUMS)
endif()

add_executable(rohandb_robustness_tests tests/robustness_test.cpp)
target_link_libraries(rohandb_robustness_tests PRIVATE rohandb)
add_test(NAME robustness_test COMMAND rohandb_robustness_tests)

option(SE_WAL "Enable milestone 3 WAL format for file-backed engines" ON)
if(SE_WAL)
    if(NOT SE_CHECKSUMS)
        message(FATAL_ERROR "WAL requires checksums")
    endif()
    target_compile_definitions(rohandb PUBLIC ROHANDB_WAL)
endif()

if(SE_WAL)
    add_executable(rohandb_recovery_tests tests/recovery_test.cpp)
    target_link_libraries(rohandb_recovery_tests PRIVATE rohandb)
    add_test(NAME recovery_test COMMAND rohandb_recovery_tests)
endif()

if(SE_WAL)
    add_executable(rohandb_log_failure_tests tests/log_failure_test.cpp)
    target_link_libraries(rohandb_log_failure_tests PRIVATE rohandb)
    add_test(NAME log_failure_test COMMAND rohandb_log_failure_tests)
endif()
if(SE_WAL)
    add_executable(rohandb_bootstrap_identity_tests tests/bootstrap_identity_test.cpp)
    target_link_libraries(rohandb_bootstrap_identity_tests PRIVATE rohandb)
    add_test(NAME bootstrap_identity_test COMMAND rohandb_bootstrap_identity_tests)
endif()

add_executable(rohandb_profile_lookup tools/profile_lookup.cpp)
target_link_libraries(rohandb_profile_lookup PRIVATE rohandb)
