cmake_minimum_required(VERSION 3.20)
project(task_wrapper C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

add_compile_definitions(_GNU_SOURCE)

# Add executable
add_executable(task-wrapper
    src/main.c
    src/task_wrapper.c
    src/app_task.c
    src/logger.c
)

# Include only your local headers
# (the Dockerfile copies common/include/ and common/src/ into
# ./include and ./src respectively before this runs, so the shared
# logger and its header are already local paths at build time)
target_include_directories(task-wrapper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Threads + pkg libraries
find_package(Threads REQUIRED)
target_link_libraries(task-wrapper PRIVATE
    Threads::Threads
    PkgConfig::GLIB2
    PkgConfig::GIO
    PkgConfig::JSON_GLIB
    m
)