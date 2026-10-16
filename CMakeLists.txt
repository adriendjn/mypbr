cmake_minimum_required(VERSION 3.10)
project(mypbr)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build; rendering is far too slow in Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(mypbr main.cpp src/stb_impl.cpp)
target_include_directories(mypbr PRIVATE include)
# Third-party single-header libraries; SYSTEM keeps their warnings out of our build
target_include_directories(mypbr SYSTEM PRIVATE external)
target_compile_options(mypbr PRIVATE -Wall -Wextra)

# Sanity tests: run with "ctest --test-dir build"
enable_testing()
foreach(test furnace_test pdf_test)
    add_executable(${test} tests/${test}.cpp)
    target_include_directories(${test} PRIVATE include)
    target_compile_options(${test} PRIVATE -Wall -Wextra)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
