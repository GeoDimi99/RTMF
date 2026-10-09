cmake_minimum_required(VERSION 3.20)
project(task_service C)

# Set C standard and requirements
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compilation flags
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")

# Initialize PkgConfig to find system dependencies
find_package(PkgConfig REQUIRED)

# Check for required modules and create imported targets
# This automatically handles include paths and library linking
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# Define the executable and its source files
add_executable(execution-manager
    src/main.c
    src/execution_manager.c
    src/schedule.c
    ../common/src/logger.c
)

# Set include directories for the target
# Using PRIVATE scope to avoid leaking these paths to other potential targets
target_include_directories(execution-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../common/include
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
)

# Apply additional compiler definitions from PkgConfig (if any)
target_compile_options(execution-manager PRIVATE 
    ${GLIB2_CFLAGS_OTHER} 
    ${GIO_CFLAGS_OTHER}
    ${HIREDIS_CFLAGS_OTHER}
)