find_package(GTest REQUIRED)

add_executable(GoppUnitTests
  Basic/VersionTest.cpp
  AST/ASTTest.cpp
  Basic/DiagnosticsTest.cpp
  Basic/ExplainTest.cpp
  Basic/SourceManagerTest.cpp
  Basic/UnicodeTest.cpp
  Format/FormatTest.cpp
  Format/FuzzCorpusTest.cpp
  Lex/GoScannerPortTest.cpp
  Lex/LexerExtrasTest.cpp
  Mangle/SemaSymbolsTest.cpp
  Mangle/SymbolNameTest.cpp
  Parse/GoParserPortTest.cpp
  Parse/ParserTest.cpp
  Sema/ConformanceTest.cpp
  Sema/ConstantTest.cpp
  Sema/CorpusTest.cpp
  Sema/DeepTest.cpp
  Sema/FlowTest.cpp
  Sema/ForeignTest.cpp
  Sema/SerializeTest.cpp
  Sema/LoaderTest.cpp
  Sema/ParallelTest.cpp
  Build/HostToolchainTest.cpp
  GIR/GIRTest.cpp
  GIR/VerifierTest.cpp
  GIR/StressTest.cpp
  GIR/OwnershipTest.cpp
  GIR/DeepTest.cpp
  GIR/AllocatorTest.cpp
  GIR/GlueTest.cpp
  GIR/UnitRuntimeTest.cpp
  GIR/InlineTest.cpp
  GIR/ScopeTest.cpp
  GIR/InstancesTest.cpp
  CodeGen/CABITest.cpp
  CodeGen/CodeGenTest.cpp
  CodeGen/DebugInfoTest.cpp
  CodeGen/GoABITest.cpp
  CodeGen/LayoutTest.cpp
  CodeGen/OptionHashTest.cpp
  CodeGen/PipelineTest.cpp
  TestRun/TestRunTest.cpp)
# The glue, Option hash and allocator tests run GIR in LLJIT.
gopp_llvm_libs(_unit_llvm orcjit native support core target)
target_link_libraries(GoppUnitTests PRIVATE
  ${_unit_llvm}
  goppBasic goppLex goppAST goppParse goppSema goppMangle goppFormat goppBuild goppGIR goppCodeGen goppImport goppTestRun
  GTest::gtest_main)
target_compile_definitions(GoppUnitTests PRIVATE
  GOPP_TEST_GOPP_BIN="$<TARGET_FILE:gopp>"
  GOPP_TEST_INPUTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Build/Inputs"
  GOPP_EXAMPLES_DIR="${GOPP_SOURCE_DIR}/docs/spec/examples"
  GOPP_CONFORMANCE_DIR="${GOPP_SOURCE_DIR}/tests/sema/conformance"
  GOPP_FUZZ_CORPUS_DIR="${GOPP_SOURCE_DIR}/tests/fuzz/corpus"
  GOPP_SPEC_DIR="${GOPP_SOURCE_DIR}/docs/spec"
  GOPP_FUZZ_SYMBOL_DIR="${GOPP_SOURCE_DIR}/tests/fuzz/symbol-corpus"
  GOPP_FUZZ_REGRESSIONS_DIR="${GOPP_SOURCE_DIR}/tests/fuzz/regressions")

# The runner tests start `gopp test-exec`.
add_dependencies(GoppUnitTests gopp)

configure_file(lit.site.cfg.py.in lit.site.cfg.py @ONLY)

add_custom_target(check-unit
  COMMAND ${GOPP_LIT} ${GOPP_LIT_ARGS} ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS GoppUnitTests
  USES_TERMINAL)
