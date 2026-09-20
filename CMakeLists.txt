# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
project(minikv VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BRPC_ROOT "$ENV{HOME}/projects/minikv/.deps/brpc-install" CACHE PATH "Installed bRPC prefix")
set(ENV{PKG_CONFIG_PATH} "${BRPC_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
find_package(PkgConfig REQUIRED)
pkg_check_modules(BRPC REQUIRED IMPORTED_TARGET brpc=1.18.0)
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)
find_package(gflags REQUIRED)
find_library(ROCKSDB_LIBRARY rocksdb REQUIRED)
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h REQUIRED)

protobuf_generate_cpp(PROTO_SOURCES PROTO_HEADERS proto/minikv.proto)
add_library(minikv_proto ${PROTO_SOURCES} ${PROTO_HEADERS})
target_include_directories(minikv_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(minikv_proto PUBLIC protobuf::libprotobuf)

add_library(minikv_store src/store.cpp)
target_include_directories(minikv_store PUBLIC src)
target_include_directories(minikv_store SYSTEM PUBLIC ${ROCKSDB_INCLUDE_DIR})
target_link_libraries(minikv_store PUBLIC ${ROCKSDB_LIBRARY} Threads::Threads)

add_executable(minikv_server src/server_main.cpp src/kv_service.cpp)
target_link_libraries(minikv_server PRIVATE minikv_store minikv_proto PkgConfig::BRPC gflags Threads::Threads)
add_executable(minikv_cli src/client_main.cpp)
target_link_libraries(minikv_cli PRIVATE minikv_proto PkgConfig::BRPC gflags Threads::Threads)
add_executable(minikv_bench src/bench_main.cpp)
target_link_libraries(minikv_bench PRIVATE minikv_proto PkgConfig::BRPC gflags Threads::Threads)

foreach(target minikv_store minikv_server minikv_cli minikv_bench)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(store_test tests/store_test.cpp)
    target_compile_options(store_test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(store_test PRIVATE minikv_store)
    add_test(NAME store_contract COMMAND store_test)
    add_executable(ttl_test tests/ttl_test.cpp)
    target_compile_options(ttl_test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ttl_test PRIVATE minikv_store)
    add_test(NAME ttl_contract COMMAND ttl_test)
    set_tests_properties(ttl_contract PROPERTIES TIMEOUT 30)
    add_test(NAME rpc_integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py
        $<TARGET_FILE:minikv_server> $<TARGET_FILE:minikv_cli>)
    add_executable(bench_stats_test tests/bench_stats_test.cpp)
    target_include_directories(bench_stats_test PRIVATE src)
    target_compile_options(bench_stats_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME bench_stats COMMAND bench_stats_test)
    add_test(NAME benchmark_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_test.py
        $<TARGET_FILE:minikv_server> $<TARGET_FILE:minikv_bench> $<TARGET_FILE:minikv_cli>)
    set_tests_properties(benchmark_contract PROPERTIES TIMEOUT 90)
    add_test(NAME benchmark_runner COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_runner_test.py
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/compare-wal.py $<TARGET_FILE_DIR:minikv_server>)
    set_tests_properties(benchmark_runner PROPERTIES TIMEOUT 90)
    add_test(NAME benchmark_runner_unit COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_runner_unit_test.py
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/compare-wal.py)
    set_tests_properties(benchmark_runner_unit PROPERTIES TIMEOUT 15)
    set_tests_properties(store_contract PROPERTIES TIMEOUT 30)
    set_tests_properties(rpc_integration PROPERTIES TIMEOUT 90)
endif()
