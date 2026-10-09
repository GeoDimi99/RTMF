cmake_minimum_required(VERSION 3.20)
project(task_service C)

# 1. Set C standard and requirements
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 2. Compilation flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g -O0 -Wno-unused-parameter")

# 3. Initialize PkgConfig to find system dependencies
find_package(PkgConfig REQUIRED)

# 4. Check for required modules and create imported targets
# Adding IMPORTED_TARGET for JSON_GLIB for consistency
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# 5. Define the executable and its source files
add_executable(execution-manager
    src/main.c
    src/execution_manager.c
    src/schedule.c
    src/app_task.c
    src/logger.c
)

# 6. Set include directories for the target
target_include_directories(execution-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${JSON_GLIB_INCLUDE_DIRS}  # Ensure JSON-GLIB headers are found
)

# 7. Find Threading support (pthread)
find_package(Threads REQUIRED)

# 8. Link libraries to the executable
# Using modern PkgConfig:: targets for all dependencies
target_link_libraries(execution-manager 
    PRIVATE 
    Threads::Threads 
    PkgConfig::GLIB2
    PkgConfig::GIO
    PkgConfig::JSON_GLIB
    PkgConfig::HIREDIS
    m
)

# 9. Apply additional compiler definitions from PkgConfig
target_compile_options(execution-manager PRIVATE 
    ${GLIB2_CFLAGS_OTHER} 
    ${GIO_CFLAGS_OTHER}
    ${JSON_GLIB_CFLAGS_OTHER}
    ${HIREDIS_CFLAGS_OTHER}
)