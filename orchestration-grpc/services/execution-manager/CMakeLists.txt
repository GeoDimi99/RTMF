cmake_minimum_required(VERSION 3.20)
project(task_service C CXX)

# Set C and C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compilation flags
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")

# Initialize PkgConfig to find system dependencies
find_package(PkgConfig REQUIRED)

# Check for required modules and create imported targets
# This automatically handles include paths and library linking
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# Find gRPC and Protobuf
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# Define the executable and its source files
add_executable(execution-manager
    src/main.c
    src/grpc_client.cpp
    src/realtime_scheduler.c
    proto/proto/task_service.pb.cc
    proto/proto/task_service.grpc.pb.cc
    src/schedule.c
    ../common/src/task.c
    ../common/src/logger.c
)

# Set include directories for the target
# Using PRIVATE scope to avoid leaking these paths to other potential targets
target_include_directories(execution-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${GLIB2_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
)

# Find Threading support (pthread)
find_package(Threads REQUIRED)

# Link libraries to the executable
# Using PkgConfig:: targets is the modern approach to ensure all flags are passed correctly
target_link_libraries(execution-manager 
    PRIVATE 
    Threads::Threads 
    PkgConfig::GLIB2
    PkgConfig::GIO
    PkgConfig::HIREDIS
    protobuf::libprotobuf
    gRPC::grpc++
    gRPC::grpc++_reflection
)

# Apply additional compiler definitions from PkgConfig (if any)
add_definitions(${GLIB2_CFLAGS_OTHER} ${GIO_CFLAGS_OTHER})

# Required for pthread_setattr_default_np (GNU extension)
target_compile_definitions(execution-manager PRIVATE _GNU_SOURCE)