cmake_minimum_required(VERSION 3.22)
project(lan_regex_runtime LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# Pin Abseil to RE2's own MODULE.bazel dependency version. Callers can supply
# offline source trees via FETCHCONTENT_SOURCE_DIR_ABSEIL / _RE2.
set(BUILD_SHARED_LIBS OFF)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL OFF)
set(ABSL_BUILD_TESTING OFF)
set(RE2_INSTALL OFF)
set(RE2_BUILD_TESTING OFF)
set(RE2_TEST OFF)
set(RE2_BENCHMARK OFF)
FetchContent_Declare(abseil
  URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20250512.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(re2
  URL https://github.com/google/re2/archive/refs/tags/2025-11-05.tar.gz
  URL_HASH SHA256=87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(abseil re2)

add_library(lan_regex_runtime STATIC regex.cpp)
target_include_directories(lan_regex_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lan_regex_runtime PUBLIC re2::re2)
target_compile_options(lan_regex_runtime PRIVATE
  $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Werror>)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()
  # Deliberately compiled as C to exercise the public ABI, linked as C++.
  add_executable(lan_regex_runtime_test ../../tests/regex_runtime.c)
  target_link_libraries(lan_regex_runtime_test PRIVATE lan_regex_runtime)
  add_test(NAME regex_runtime COMMAND lan_regex_runtime_test)
endif()
