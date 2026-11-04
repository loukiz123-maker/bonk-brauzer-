# Bonk Brauzer — CMake build (alternative to the canonical Makefile).
#
#   cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-cmake -j
#   ctest --test-dir build-cmake --output-on-failure
#
# The source list is globbed with CONFIGURE_DEPENDS so that adding a module
# does not require touching this file; the Makefile does the same with wildcard.

cmake_minimum_required(VERSION 3.16)
project(bonk_brauzer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_library(bonk_compile_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bonk_compile_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
target_include_directories(bonk_compile_options INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Engine library: everything under src/ except the executables in src/apps.
file(GLOB_RECURSE BONK_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp)
list(FILTER BONK_ENGINE_SOURCES EXCLUDE REGEX "/src/apps/")

add_library(bonk_engine STATIC ${BONK_ENGINE_SOURCES})
target_link_libraries(bonk_engine PUBLIC bonk_compile_options)

add_executable(bonk ${CMAKE_CURRENT_SOURCE_DIR}/src/apps/main.cpp)
target_link_libraries(bonk PRIVATE bonk_engine)

enable_testing()

file(GLOB_RECURSE BONK_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
list(FILTER BONK_TEST_SOURCES EXCLUDE REGEX "test_main\\.cpp$")

add_executable(bonk_tests ${BONK_TEST_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(bonk_tests PRIVATE bonk_engine)
target_include_directories(bonk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_test(NAME unit COMMAND bonk_tests)
add_test(NAME selftest COMMAND bonk selftest)
