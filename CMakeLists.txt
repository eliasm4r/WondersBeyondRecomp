cmake_minimum_required(VERSION 3.20)
project(wii_recomp CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WP_BUILD "2611-004" CACHE STRING "Build number shown in the window title")
set(WP_VERSION "0.2.0-alpha" CACHE STRING "Release version compared with the latest GitHub release")
option(WP_TRACE_CALLS "Record every guest function entry for diagnostics (slower)" OFF)
option(WP_CONSOLE "Build the game as a console program" OFF)
set(WP_CODE_OPT "-O1" CACHE STRING "Optimization flag for the translated game code")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

enable_testing()

add_subdirectory(third_party)
add_subdirectory(engine)
add_subdirectory(tests)
add_subdirectory(game)
