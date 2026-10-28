# Benchmarks (not built by default): `ninja bench-parse` builds and runs the
# parse throughput benchmark. Measure in a Release build (a separate build
# directory): cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
add_gopp_tool(gopp-bench-parse ParseThroughput.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppParse goppAST goppLex goppBasic)
set_target_properties(gopp-bench-parse PROPERTIES EXCLUDE_FROM_ALL ON)
target_include_directories(gopp-bench-parse PRIVATE ${GOPP_SOURCE_DIR}/compiler/include)

add_custom_target(bench-parse
  COMMAND gopp-bench-parse
  DEPENDS gopp-bench-parse
  USES_TERMINAL)

# Type-check throughput (P03, D3.10: at least 250k lines/s per core):
# `ninja bench-check`, in a Release build.
add_gopp_tool(gopp-bench-check TypeCheckThroughput.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppSema goppParse goppAST goppLex goppBasic)
set_target_properties(gopp-bench-check PROPERTIES EXCLUDE_FROM_ALL ON)
target_include_directories(gopp-bench-check PRIVATE ${GOPP_SOURCE_DIR}/compiler/include)

add_custom_target(bench-check
  COMMAND gopp-bench-check
  DEPENDS gopp-bench-check
  USES_TERMINAL)
