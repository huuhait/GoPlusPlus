# Compiler flags, launcher, linker and sanitizers shared by all targets.

option(GOPP_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
set(GOPP_USE_SANITIZER "" CACHE STRING
  "Sanitizers for the compiler itself, e.g. \"Address;Undefined\"")

add_compile_options(-Wall -Wextra -Wpedantic ${GOPP_RTTI_FLAG} -fno-exceptions)
if(GOPP_WARNINGS_AS_ERRORS)
  add_compile_options(-Werror)
endif()

# With an assertions-enabled LLVM, keep assertions on in our code too: LLVM's
# inline header code (asserts, dump helpers) must agree with the library, so
# drop -DNDEBUG for such builds.
if(LLVM_ENABLE_ASSERTIONS)
  foreach(cfg RELEASE MINSIZEREL RELWITHDEBINFO)
    string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_${cfg} "${CMAKE_CXX_FLAGS_${cfg}}")
    string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_${cfg} "${CMAKE_C_FLAGS_${cfg}}")
  endforeach()
endif()

if(GOPP_USE_SANITIZER)
  string(REPLACE ";" "," _san "${GOPP_USE_SANITIZER}")
  string(TOLOWER "${_san}" _san)
  # -fno-sanitize-recover: a UBSan report aborts, so the test fails.
  add_compile_options(-fsanitize=${_san} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${_san})
endif()

# Compiler cache: sccache, else ccache, else none. Automatic, no flag needed.
# This deliberately overrides a CMAKE_<LANG>_COMPILER_LAUNCHER from the
# environment (some host profiles export ccache globally).
find_program(GOPP_COMPILER_CACHE NAMES sccache ccache)
if(GOPP_COMPILER_CACHE)
  # FORCE into the cache: a plain variable would leave CMakeCache.txt showing
  # the environment's value (ccache) while build.ninja uses ours.
  set(CMAKE_C_COMPILER_LAUNCHER ${GOPP_COMPILER_CACHE} CACHE STRING "C compiler launcher" FORCE)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${GOPP_COMPILER_CACHE} CACHE STRING "C++ compiler launcher" FORCE)
  message(STATUS "compiler cache: ${GOPP_COMPILER_CACHE}")
else()
  message(STATUS "compiler cache: none (no sccache or ccache found)")
endif()

# Linker: mold, else lld, else the compiler's default. Each candidate is
# tried for real (the compiler must accept -fuse-ld=<name>).
include(CheckLinkerFlag)
set(GOPP_LINKER "")
foreach(_ld mold lld)
  check_linker_flag(CXX "-fuse-ld=${_ld}" GOPP_HAVE_LD_${_ld})
  if(GOPP_HAVE_LD_${_ld})
    set(GOPP_LINKER ${_ld})
    break()
  endif()
endforeach()
if(GOPP_LINKER)
  add_link_options(-fuse-ld=${GOPP_LINKER})
  message(STATUS "linker: ${GOPP_LINKER}")
else()
  message(STATUS "linker: the compiler's default (neither mold nor lld works)")
endif()
