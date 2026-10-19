cmake_minimum_required(VERSION 3.10)
project(vhuge C)

set(CMAKE_C_STANDARD 11)
# -O1 keeps the timed accesses in the probes as written.
string(APPEND CMAKE_C_FLAGS " -O1 -Wall")
find_package(Threads REQUIRED)

include_directories(${PROJECT_SOURCE_DIR}/include)
# Third-party header; SYSTEM hides warnings about its unused helpers.
include_directories(SYSTEM ${PROJECT_SOURCE_DIR}/misc/PTEditor)

# Standalone probe: TLB method and prefetcher method.
add_executable(vhuge src/vhuge.c src/utils.c src/pref.c src/gpa_hpa.c)
# Pool daemon for the vHuge kernel.
add_executable(vhuge-poold src/vhuge_poold.c src/utils.c src/pref.c)
target_link_libraries(vhuge Threads::Threads)
target_link_libraries(vhuge-poold Threads::Threads)
