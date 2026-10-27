# The parser/formatter fuzzer. Only built with -DGOPP_BUILD_FUZZERS=ON (a
# sanitized build, see cmake/modules/GoppBuildOptions.cmake and README.md).
gopp_llvm_libs(_fuzz_llvm support)
add_executable(gopp-fuzz-parse ParseFuzzer.cpp)
target_compile_options(gopp-fuzz-parse PRIVATE -fsanitize=fuzzer)
target_link_options(gopp-fuzz-parse PRIVATE -fsanitize=fuzzer)
target_link_libraries(gopp-fuzz-parse PRIVATE
  goppFormat goppParse goppAST goppLex goppBasic ${_fuzz_llvm})
