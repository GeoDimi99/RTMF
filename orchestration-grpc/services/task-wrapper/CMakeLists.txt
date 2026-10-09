cmake_minimum_required(VERSION 3.20)
project(task_wrapper C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")

add_compile_definitions(_GNU_SOURCE)

# Find pkg-config
find_package(PkgConfig REQUIRED)

# Find GLib and hiredis
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# Find gRPC and Protobuf
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# Include directories
# (the Dockerfile copies common/include/ and common/src/ into
# ./include and ./src respectively before this runs, so the shared
# logger/task sources and headers are already local paths at build time)
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${GLIB2_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
)

# Add gRPC server executable
add_executable(task-wrapper
    src/grpc_server.cpp
    proto/proto/task_service.pb.cc
    proto/proto/task_service.grpc.pb.cc
    src/logger.c
    src/task.c
)

# Threads
find_package(Threads REQUIRED)

# Link libraries
target_link_libraries(task-wrapper 
    PRIVATE 
    Threads::Threads
    PkgConfig::GLIB2
    PkgConfig::HIREDIS
    protobuf::libprotobuf
    gRPC::grpc++
    gRPC::grpc++_reflection
)


