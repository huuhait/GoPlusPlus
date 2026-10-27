# Locate the pinned LLVM, Clang and LLD (ADR-0002). The dev shell exports
# LLVM_DIR / Clang_DIR / LLD_DIR; outside Nix pass them to cmake yourself.

# find_package caches <Pkg>_DIR, and CMake then ignores the shell's value. A
# build dir reused in another shell (default vs assertions) or after an LLVM
# bump would silently keep the old LLVM. Refuse that.
foreach(_pkg LLVM Clang LLD)
  if(DEFINED ENV{${_pkg}_DIR} AND DEFINED CACHE{${_pkg}_DIR}
     AND NOT "$ENV{${_pkg}_DIR}" STREQUAL "${${_pkg}_DIR}")
    message(FATAL_ERROR
      "${_pkg}_DIR is cached as\n  ${${_pkg}_DIR}\nbut this shell sets\n  $ENV{${_pkg}_DIR}\n"
      "This build directory was configured against a different LLVM (another dev "
      "shell, or the flake changed). Use a new build directory, for example "
      "`cmake -S . -B build-<name>`.")
  endif()
endforeach()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR}), assertions: ${LLVM_ENABLE_ASSERTIONS}")

if(LLVM_PACKAGE_VERSION VERSION_LESS 21)
  message(FATAL_ERROR "Go++ needs LLVM >= 21 (ADR-0002), found ${LLVM_PACKAGE_VERSION}")
endif()

list(APPEND CMAKE_MODULE_PATH "${LLVM_DIR}")
include(AddLLVM)
include(HandleLLVMOptions)

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
separate_arguments(GOPP_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_compile_definitions(${GOPP_LLVM_DEFINITIONS})

# RTTI must match the LLVM we link (D0.3): ask llvm-config.
find_program(GOPP_LLVM_CONFIG llvm-config HINTS ${LLVM_TOOLS_BINARY_DIR})
if(GOPP_LLVM_CONFIG)
  execute_process(COMMAND ${GOPP_LLVM_CONFIG} --has-rtti
    OUTPUT_VARIABLE GOPP_LLVM_HAS_RTTI OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
elseif(LLVM_ENABLE_RTTI)
  set(GOPP_LLVM_HAS_RTTI YES)
else()
  set(GOPP_LLVM_HAS_RTTI NO)
endif()
if(GOPP_LLVM_HAS_RTTI STREQUAL "YES")
  set(GOPP_RTTI_FLAG -frtti)
else()
  set(GOPP_RTTI_FLAG -fno-rtti)
endif()
message(STATUS "RTTI: ${GOPP_LLVM_HAS_RTTI} (${GOPP_RTTI_FLAG})")

# Shared vs static LLVM/Clang/LLD (D0.2, D0.13). Dev builds link libLLVM.so
# and libclang-cpp.so (fast relinks); Release builds link LLVM, Clang and LLD
# statically. Never mix: two copies of LLVM in one process register every
# cl::opt twice and abort at startup. LLD has no shared library in nixpkgs, so
# its static archives are used in both modes.
set(GOPP_LINK_LLVM_SHARED AUTO CACHE STRING "Link LLVM/Clang as shared libraries: ON, OFF or AUTO (AUTO: shared unless Release)")
if(GOPP_LINK_LLVM_SHARED STREQUAL "AUTO")
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(GOPP_LLVM_SHARED OFF)
  else()
    set(GOPP_LLVM_SHARED ON)
  endif()
else()
  set(GOPP_LLVM_SHARED ${GOPP_LINK_LLVM_SHARED})
endif()
if(GOPP_LLVM_SHARED AND NOT LLVM_LINK_LLVM_DYLIB)
  message(FATAL_ERROR "This LLVM has no libLLVM shared library; use -DGOPP_LINK_LLVM_SHARED=OFF")
endif()

if(GOPP_LLVM_SHARED)
  message(STATUS "LLVM linkage: shared (libLLVM.so, libclang-cpp.so; LLD static)")
else()
  # nixpkgs builds LLVM with LLVM_LINK_LLVM_DYLIB, so the exported Clang and
  # LLD archives name "LLVM" (libLLVM.so) as their only LLVM dependency. The
  # archives themselves are ordinary object files; point that dependency at
  # LLVM's static component archives instead, so exactly one static copy of
  # LLVM ends up in gopp. The linker only pulls the objects that are used.
  # Left out: tool-only components whose extra system libraries (libpfm,
  # libffi, Polly, libxml2) would otherwise land on gopp's link line, and
  # tool-support archives that were themselves linked against libLLVM.so.
  set(_gopp_llvm_archives ${LLVM_AVAILABLE_LIBS})
  list(FILTER _gopp_llvm_archives INCLUDE REGEX "^LLVM.+")
  list(FILTER _gopp_llvm_archives EXCLUDE REGEX
    "^LLVM(Exegesis.*|Interpreter|Extensions|Polly|WindowsManifest)$")
  foreach(_lib IN LISTS _gopp_llvm_archives)
    get_target_property(_deps ${_lib} INTERFACE_LINK_LIBRARIES)
    if("LLVM" IN_LIST _deps)
      list(REMOVE_ITEM _gopp_llvm_archives ${_lib})
    endif()
  endforeach()
  add_library(GoppLLVMStatic INTERFACE)
  target_link_libraries(GoppLLVMStatic INTERFACE ${_gopp_llvm_archives})
  foreach(_t IN LISTS CLANG_EXPORTED_TARGETS LLD_EXPORTED_TARGETS)
    if(NOT TARGET ${_t})
      continue()
    endif()
    get_target_property(_type ${_t} TYPE)
    get_target_property(_deps ${_t} INTERFACE_LINK_LIBRARIES)
    if(_type STREQUAL "STATIC_LIBRARY" AND _deps)
      list(TRANSFORM _deps REPLACE "^LLVM$" "GoppLLVMStatic")
      set_target_properties(${_t} PROPERTIES INTERFACE_LINK_LIBRARIES "${_deps}")
    endif()
  endforeach()
  message(STATUS "LLVM linkage: static (LLVM, Clang and LLD archives; no libLLVM.so)")
endif()

# gopp_llvm_libs(<out> <components...>)
function(gopp_llvm_libs out)
  if(GOPP_LLVM_SHARED)
    set(${out} LLVM PARENT_SCOPE)
  else()
    llvm_map_components_to_libnames(libs ${ARGN})
    set(${out} ${libs} PARENT_SCOPE)
  endif()
endfunction()

# gopp_clang_libs(<out> <clang libraries...>): libclang-cpp.so when shared,
# the named static archives otherwise.
function(gopp_clang_libs out)
  if(GOPP_LLVM_SHARED)
    set(${out} clang-cpp PARENT_SCOPE)
  else()
    set(${out} ${ARGN} PARENT_SCOPE)
  endif()
endfunction()

# gopp_lld_libs(<out> <lld libraries...>): always the static archives.
function(gopp_lld_libs out)
  set(${out} ${ARGN} PARENT_SCOPE)
endfunction()
