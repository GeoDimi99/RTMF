cmake_minimum_required(VERSION 3.20)
project(common C)

# Optional: create a static library for common code
add_library(common STATIC)

# Include directories for headers
target_include_directories(common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Add source files if you have .c in common/src
target_sources(common PRIVATE   src/logger.c 
                                src/task_ipc.c
                                src/task.c
                                )

# If you only have headers, the library can be empty