cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

# Hybrid residual: fork keeps ArduinoJson OTA (no lib/JsonParser). Skip tip
# host suites whose sources/CMakeLists are absent so cmake -S test configures.
foreach(_suite IN ITEMS
    streaming_json_parser
    release_json_parser
    differential_rounding
    hyphenation_eval
    utf8_compose
    opds_filename
    minibidi_arabic
    combining_marks
    font_repository)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_suite}/CMakeLists.txt")
    add_subdirectory(${_suite})
  else()
    message(STATUS "Skipping host test suite ${_suite} (CMakeLists.txt not present)")
  endif()
endforeach()
