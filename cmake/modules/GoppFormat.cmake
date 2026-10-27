# format-check: fails if any C++ file differs from .clang-format.
# format:       rewrites the files in place.
find_program(GOPP_CLANG_FORMAT clang-format)
if(GOPP_CLANG_FORMAT)
  foreach(mode check apply)
    if(mode STREQUAL "check")
      set(target format-check)
    else()
      set(target format)
    endif()
    add_custom_target(${target}
      COMMAND ${CMAKE_COMMAND}
        -DMODE=${mode}
        -DCLANG_FORMAT=${GOPP_CLANG_FORMAT}
        -DSOURCE_DIR=${GOPP_SOURCE_DIR}
        -P ${GOPP_SOURCE_DIR}/cmake/scripts/RunClangFormat.cmake
      USES_TERMINAL VERBATIM)
  endforeach()
else()
  # Never pass silently: without clang-format the targets fail.
  message(STATUS "clang-format not found: format targets will fail (use nix develop)")
  foreach(target format-check format)
    add_custom_target(${target}
      COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found (use nix develop)"
      COMMAND ${CMAKE_COMMAND} -E false)
  endforeach()
endif()
