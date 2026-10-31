cmake_minimum_required(VERSION 3.20)
# The Dev flags before project(): with -DCMAKE_BUILD_TYPE=Dev given, project()
# makes empty CMAKE_<LANG>_FLAGS_DEV cache entries, and a CACHE default set
# after it would never apply (no -O1 -g). A -DCMAKE_CXX_FLAGS_DEV=... of the
# user's is already in the cache and stays.
set(CMAKE_CXX_FLAGS_DEV "-O1 -g" CACHE STRING "Flags for the Dev build type")
set(CMAKE_C_FLAGS_DEV "-O1 -g" CACHE STRING "Flags for the Dev build type")
project(GoPlusPlus VERSION 0.0.1 LANGUAGES C CXX)

set(GOPP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND CMAKE_MODULE_PATH "${GOPP_SOURCE_DIR}/cmake/modules")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Build types (D0.13): Dev is the default and is tuned for the edit-build loop:
# -O1 -g, assertions on (no NDEBUG), shared LLVM/Clang. Release is -O3 -DNDEBUG
# with LLVM, Clang and LLD linked statically.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Dev CACHE STRING "Build type: Dev, Release, Debug" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(GoppLLVM)
include(GoppBuildOptions)
include(AddGopp)
include(GoppFormat)

add_subdirectory(compiler)
add_subdirectory(tests)
add_subdirectory(bench)
