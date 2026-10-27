# Run with: cmake -DMODE=check|apply -DCLANG_FORMAT=... -DSOURCE_DIR=... -P this
# Globbing happens at build time so new files are never missed.
file(GLOB_RECURSE files
  "${SOURCE_DIR}/compiler/*.cpp" "${SOURCE_DIR}/compiler/*.h"
  "${SOURCE_DIR}/tests/*.cpp" "${SOURCE_DIR}/tests/*.h"
  "${SOURCE_DIR}/runtime/*.cpp" "${SOURCE_DIR}/runtime/*.h"
  "${SOURCE_DIR}/bench/*.cpp" "${SOURCE_DIR}/bench/*.h")
if(NOT files)
  return()
endif()
if(MODE STREQUAL "check")
  set(args --dry-run -Werror)
else()
  set(args -i)
endif()
execute_process(COMMAND ${CLANG_FORMAT} ${args} ${files} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "clang-format ${MODE} failed (run `ninja format` to fix)")
endif()
