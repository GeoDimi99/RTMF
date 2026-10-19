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

# Generate the schedule plan (schedule_plan.h) from the test manifest: it is regenerated
# whenever the manifest (or the generator) changes. Override with -DPLAN_MANIFEST=<path>.
set(PLAN_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test_0_code/manifest.yaml"
    CACHE FILEPATH "Manifest the schedule plan is generated from")
set(PLAN_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/manifest_to_plan.py")
set(PLAN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/schedule_plan.h")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${PLAN_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE} ${PLAN_GENERATOR} ${PLAN_MANIFEST} ${PLAN_HEADER}
    DEPENDS ${PLAN_MANIFEST} ${PLAN_GENERATOR}
    COMMENT "Generating schedule_plan.h from ${PLAN_MANIFEST}"
)

# Define the executable and its source files
add_executable(execution-manager
    src/main.c
    src/execution_manager.c
    src/schedule.c
    src/app_task.c
    ../../common/src/logger.c
    ${PLAN_HEADER}
)

# Set include directories for the target
# Using PRIVATE scope to avoid leaking these paths to other potential targets
# (common/ lives at the repo root, two levels up from this CMakeLists.txt:
#  services/execution-manager -> services -> repo root -> common)
target_include_directories(execution-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
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
    m
)

# Apply additional compiler definitions from PkgConfig (if any)
add_definitions(${GLIB2_CFLAGS_OTHER} ${GIO_CFLAGS_OTHER})