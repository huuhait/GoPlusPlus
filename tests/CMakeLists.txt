find_program(GOPP_LIT NAMES lit llvm-lit REQUIRED)
find_program(GOPP_FILECHECK FileCheck HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
get_filename_component(GOPP_FILECHECK_DIR ${GOPP_FILECHECK} DIRECTORY)
find_program(GOPP_PYTHON NAMES python3 python REQUIRED)

# lit runs one worker per core (D0.13).
cmake_host_system_information(RESULT GOPP_LIT_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
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

# GoogleTest binaries run through lit too (same parallelism and timing report).
add_subdirectory(unit)
set(GOPP_CHECK_TARGETS ${GOPP_CHECK_TARGETS} check-unit)
set(GOPP_STAGE_DIRS ${GOPP_STAGE_DIRS} ${CMAKE_CURRENT_BINARY_DIR}/unit)

add_custom_target(check-includes
  COMMAND ${GOPP_SOURCE_DIR}/tools/check-frontend-includes.sh ${GOPP_SOURCE_DIR}
  USES_TERMINAL)

# check runs every stage target.
add_custom_target(check DEPENDS ${GOPP_CHECK_TARGETS} check-includes)

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
