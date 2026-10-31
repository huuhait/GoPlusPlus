find_program(GOPP_LIT NAMES lit llvm-lit REQUIRED)
find_program(GOPP_FILECHECK FileCheck HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
get_filename_component(GOPP_FILECHECK_DIR ${GOPP_FILECHECK} DIRECTORY)
find_program(GOPP_PYTHON NAMES python3 python REQUIRED)

# lit runs one worker per two logical cores (at least 8, at most one per core):
# the tests start processes and threads of their own, so a worker a core
# oversubscribes the machine. Measured on a 192-thread host (the whole suite,
# 1792 tests, three runs each): -j 192 took 2.1-2.3 s, 300 s of summed test
# time, 55 tests over 1.3 s and the slowest test at 2.0 s (the 2 s limit);
# -j 96 took 1.7 s, 150 s, none over 1.3 s but one, the slowest 1.4 s; -j 64
# took 2.0 s, 115 s, the slowest 1.0 s (D0.13; the same cost per process that
# tests/procslots.py bounds). Override with -DGOPP_LIT_JOBS=N.
cmake_host_system_information(RESULT GOPP_LOGICAL_CORES QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR GOPP_DEFAULT_LIT_JOBS "${GOPP_LOGICAL_CORES} / 2")
if(GOPP_DEFAULT_LIT_JOBS LESS 8)
  set(GOPP_DEFAULT_LIT_JOBS ${GOPP_LOGICAL_CORES})
  if(GOPP_DEFAULT_LIT_JOBS GREATER 8)
    set(GOPP_DEFAULT_LIT_JOBS 8)
  endif()
endif()
set(GOPP_LIT_JOBS ${GOPP_DEFAULT_LIT_JOBS} CACHE STRING "Parallel lit workers for the check targets")
# A normal test gets at most 2 s (D0.13); a slower one must be marked long.
# Sanitizer builds raise the limit (CI passes -DGOPP_TEST_TIMEOUT=20).
set(GOPP_TEST_TIMEOUT 2 CACHE STRING "Per-test time limit in seconds")
set(GOPP_LONG_TEST_TIMEOUT 300 CACHE STRING "Per-test time limit in seconds for check-long")
# Every run prints its 20 slowest tests (--time-tests); -sv keeps output short.
set(GOPP_LIT_ARGS -j ${GOPP_LIT_JOBS} --time-tests -sv --timeout ${GOPP_TEST_TIMEOUT})

# Directories of the shared LLVM and Clang libraries we link (space separated,
# one -e option each), for tests/driver/loaded-llvm.test.
set(GOPP_SHARED_LLVM_LIB_DIRS "")
if(GOPP_LLVM_SHARED)
  foreach(_t LLVM clang-cpp)
    get_target_property(_loc ${_t} LOCATION)
    get_filename_component(_dir "${_loc}" DIRECTORY)
    string(APPEND GOPP_SHARED_LLVM_LIB_DIRS " -e ${_dir}")
  endforeach()
endif()

configure_file(lit.site.cfg.py.in lit.site.cfg.py @ONLY)

# One check-<stage> target per test directory. To add a stage (lex, parse,
# sema, gir, codegen, ...) put tests in tests/<stage>/ and add one line:
#   gopp_add_stage_check(<stage>)
set(GOPP_CHECK_TARGETS)
set(GOPP_STAGE_DIRS)
function(gopp_add_stage_check stage)
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${stage})
  add_custom_target(check-${stage}
    COMMAND ${GOPP_LIT} ${GOPP_LIT_ARGS} ${CMAKE_CURRENT_BINARY_DIR}/${stage}
    DEPENDS gopp
    USES_TERMINAL)
  set(GOPP_CHECK_TARGETS ${GOPP_CHECK_TARGETS} check-${stage} PARENT_SCOPE)
  set(GOPP_STAGE_DIRS ${GOPP_STAGE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}/${stage} PARENT_SCOPE)
endfunction()

gopp_add_stage_check(driver)
gopp_add_stage_check(lex)
gopp_add_stage_check(parse)
gopp_add_stage_check(fmt)
gopp_add_stage_check(sema)
gopp_add_stage_check(gir)
gopp_add_stage_check(codegen)
gopp_add_stage_check(debug)
gopp_add_stage_check(run)

# GoogleTest binaries run through lit too (same parallelism and timing report).
add_subdirectory(unit)
set(GOPP_CHECK_TARGETS ${GOPP_CHECK_TARGETS} check-unit)
set(GOPP_STAGE_DIRS ${GOPP_STAGE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}/unit)

# check-run-real: the run tests again, every program built and run as a real
# executable instead of in the fast runner's executors (P04.19, D4.14): the
# same tests and goldens must pass both ways (lit -Dreal_binaries=1).
add_custom_target(check-run-real
  COMMAND ${GOPP_LIT} ${GOPP_LIT_ARGS} -Dreal_binaries=1
          ${CMAKE_CURRENT_BINARY_DIR}/run
  DEPENDS gopp
  USES_TERMINAL)

# check-run-leaks: the run tests again as real executables whose own code is
# instrumented with AddressSanitizer (the LLVM IR of the program, compiled by
# clang -fsanitize=address): a use of freed memory or a double free fails, and
# so does a program that ends normally with a block no global reaches
# (LeakSanitizer; P04.09, gir.md section 7, runlib.leak_check). amd64 hosts.
# (An instrumented program is built by clang and runs several times slower:
# the long tests' limit per test.)
add_custom_target(check-run-leaks
  COMMAND ${GOPP_LIT} -j ${GOPP_LIT_JOBS} --time-tests -sv
          --timeout ${GOPP_LONG_TEST_TIMEOUT} -Dleak_check=1
          ${CMAKE_CURRENT_BINARY_DIR}/run
  DEPENDS gopp
  USES_TERMINAL)

add_custom_target(check-includes
  COMMAND ${GOPP_SOURCE_DIR}/tools/check-frontend-includes.sh ${GOPP_SOURCE_DIR}
  USES_TERMINAL)

# check-diag-codes: the error codes of the diagnostics never change (P03.14):
# the table in DiagnosticIDs.def must agree with docs/errors/codes.lock.
add_custom_target(check-diag-codes
  COMMAND ${GOPP_PYTHON} ${GOPP_SOURCE_DIR}/tools/check-diag-codes.py
          --def ${GOPP_SOURCE_DIR}/compiler/include/gopp/Basic/DiagnosticIDs.def
          --lock ${GOPP_SOURCE_DIR}/docs/errors/codes.lock
  USES_TERMINAL)

# check runs every stage target.
add_custom_target(check DEPENDS ${GOPP_CHECK_TARGETS} check-includes check-diag-codes)

# check-long: opt-in slow tests (named *.long.test, "REQUIRES: long").
add_custom_target(check-long
  COMMAND ${GOPP_LIT} -j ${GOPP_LIT_JOBS} --time-tests -sv
          --timeout ${GOPP_LONG_TEST_TIMEOUT}
          -Drun_long=1 --filter "\\.long\\.test$" ${GOPP_STAGE_DIRS}
  DEPENDS gopp GoppUnitTests
  USES_TERMINAL)

# check-suite-time: fails if the suite is >10% slower than the committed
# baseline, or any normal test takes more than 2 s. Run after `check`.
set(GOPP_SUITE_TIME_KEY dev CACHE STRING "Key in tests/suite-time-baseline.json")
option(GOPP_SUITE_TIME_ALLOW_MISSING "Warn instead of failing when the baseline key is missing" OFF)
set(GOPP_SUITE_TIME_BASELINE ${GOPP_SOURCE_DIR}/tests/suite-time-baseline.json CACHE FILEPATH
  "Baseline file used by check-suite-time and check-suite-time-update")
set(GOPP_SUITE_TIME_ARGS)
if(GOPP_SUITE_TIME_ALLOW_MISSING)
  list(APPEND GOPP_SUITE_TIME_ARGS --allow-missing)
endif()
add_custom_target(check-suite-time
  COMMAND ${GOPP_PYTHON} ${GOPP_SOURCE_DIR}/tools/check-suite-time.py
          --lit ${GOPP_LIT} --jobs ${GOPP_LIT_JOBS}
          --baseline ${GOPP_SUITE_TIME_BASELINE}
          --key ${GOPP_SUITE_TIME_KEY} ${GOPP_SUITE_TIME_ARGS} ${GOPP_STAGE_DIRS}
  DEPENDS gopp GoppUnitTests
  USES_TERMINAL)

# check-suite-time-update: re-measures the suite and rewrites the baseline for
# GOPP_SUITE_TIME_KEY (default "dev"; for another machine class configure with
# -DGOPP_SUITE_TIME_KEY=<key>). Commit the changed baseline file afterwards.
add_custom_target(check-suite-time-update
  COMMAND ${GOPP_PYTHON} ${GOPP_SOURCE_DIR}/tools/check-suite-time.py
          --lit ${GOPP_LIT} --jobs ${GOPP_LIT_JOBS}
          --baseline ${GOPP_SUITE_TIME_BASELINE}
          --key ${GOPP_SUITE_TIME_KEY} --update ${GOPP_STAGE_DIRS}
  DEPENDS gopp GoppUnitTests
  USES_TERMINAL)

# The libFuzzer target (off by default, see GOPP_BUILD_FUZZERS).
if(GOPP_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
