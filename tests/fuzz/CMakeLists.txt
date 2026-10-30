# The parser/formatter fuzzer. Only built with -DGOPP_BUILD_FUZZERS=ON (a
# sanitized build, see cmake/modules/GoppBuildOptions.cmake and README.md).
gopp_llvm_libs(_fuzz_llvm support)
add_executable(gopp-fuzz-parse ParseFuzzer.cpp)
target_compile_options(gopp-fuzz-parse PRIVATE -fsanitize=fuzzer)
target_link_options(gopp-fuzz-parse PRIVATE -fsanitize=fuzzer)
target_link_libraries(gopp-fuzz-parse PRIVATE
  goppFormat goppParse goppAST goppLex goppBasic ${_fuzz_llvm})

# The symbol demangler (P04.17): only the mangler's own source and the LLVM
# support library, so the instrumented build is small.
add_executable(gopp-fuzz-symbol SymbolFuzzer.cpp
  ${GOPP_SOURCE_DIR}/compiler/lib/Mangle/SymbolName.cpp)
target_compile_options(gopp-fuzz-symbol PRIVATE -fsanitize=fuzzer)
target_link_options(gopp-fuzz-symbol PRIVATE -fsanitize=fuzzer)
target_include_directories(gopp-fuzz-symbol PRIVATE ${GOPP_SOURCE_DIR}/compiler/include)
target_link_libraries(gopp-fuzz-symbol PRIVATE ${_fuzz_llvm})
