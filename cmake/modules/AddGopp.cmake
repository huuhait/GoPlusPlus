# Helpers modelled on LLVM's add_llvm_library / add_llvm_tool.
#
#   add_gopp_library(<Name> [sources...]
#                    [LLVM_COMPONENTS <components...>]
#                    [CLANG_LIBS <clang libraries...>]
#                    [LLD_LIBS <lld libraries...>]
#                    [PCH <headers...>]
#                    [LINK_LIBS <targets...>])
#   add_gopp_tool(<name> [sources...]
#                 [LLVM_COMPONENTS <components...>]
#                 [LINK_LIBS <targets...>])
#
# PCH: precompiled headers, only for libraries that include heavy LLVM/Clang
# headers (CodeGen, Import). Front-end libraries must not use it (D0.13).
# Libraries are named goppXxx (like clangXxx). A library with no sources yet
# becomes an INTERFACE target so the dependency graph exists from day one.

function(add_gopp_library name)
  cmake_parse_arguments(ARG "" "" "LLVM_COMPONENTS;CLANG_LIBS;LLD_LIBS;PCH;LINK_LIBS" ${ARGN})
  set(target gopp${name})
  if(ARG_UNPARSED_ARGUMENTS)
    add_library(${target} STATIC ${ARG_UNPARSED_ARGUMENTS})
    set(scope PUBLIC)
  else()
    add_library(${target} INTERFACE)
    set(scope INTERFACE)
  endif()
  target_include_directories(${target} ${scope}
    $<BUILD_INTERFACE:${GOPP_SOURCE_DIR}/compiler/include>)
  if(ARG_LLVM_COMPONENTS)
    gopp_llvm_libs(llvm_libs ${ARG_LLVM_COMPONENTS})
    target_link_libraries(${target} ${scope} ${llvm_libs})
  endif()
  if(ARG_CLANG_LIBS)
    gopp_clang_libs(clang_libs ${ARG_CLANG_LIBS})
    target_link_libraries(${target} ${scope} ${clang_libs})
  endif()
  if(ARG_LLD_LIBS)
    gopp_lld_libs(lld_libs ${ARG_LLD_LIBS})
    target_link_libraries(${target} ${scope} ${lld_libs})
  endif()
  if(ARG_LINK_LIBS)
    target_link_libraries(${target} ${scope} ${ARG_LINK_LIBS})
  endif()
  # An INTERFACE library has no objects to precompile; the PCH starts to apply
  # once the library gets its first source file.
  if(ARG_PCH AND ARG_UNPARSED_ARGUMENTS)
    target_precompile_headers(${target} PRIVATE ${ARG_PCH})
  endif()
endfunction()

function(add_gopp_tool name)
  cmake_parse_arguments(ARG "" "" "LLVM_COMPONENTS;LINK_LIBS" ${ARGN})
  add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
  if(ARG_LLVM_COMPONENTS)
    gopp_llvm_libs(llvm_libs ${ARG_LLVM_COMPONENTS})
    target_link_libraries(${name} PRIVATE ${llvm_libs})
  endif()
  target_link_libraries(${name} PRIVATE ${ARG_LINK_LIBS})
endfunction()
