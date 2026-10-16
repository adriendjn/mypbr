cmake_minimum_required(VERSION 3.10)
project(mypbr)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build; rendering is far too slow in Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Tune for the machine doing the build; turn off to make portable binaries
option(MYPBR_NATIVE "Compile with -march=native" ON)

find_package(Threads REQUIRED)

# Third-party single-header libraries (stb), compiled once. SYSTEM keeps
# their warnings out of our build.
add_library(stb STATIC src/stb_impl.cpp)
target_include_directories(stb SYSTEM PUBLIC external)

add_executable(mypbr main.cpp)
target_link_libraries(mypbr PRIVATE stb Threads::Threads)
target_include_directories(mypbr PRIVATE include)
target_compile_options(mypbr PRIVATE -Wall -Wextra)
if(MYPBR_NATIVE)
    target_compile_options(mypbr PRIVATE $<$<CONFIG:Release>:-march=native>)
endif()

# Sanity tests: run with "ctest --test-dir build"
enable_testing()
foreach(test furnace_test pdf_test)
    add_executable(${test} tests/${test}.cpp)
    target_include_directories(${test} PRIVATE include)
    target_link_libraries(${test} PRIVATE stb Threads::Threads)
    target_compile_options(${test} PRIVATE -Wall -Wextra)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
