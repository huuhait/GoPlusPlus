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
  Build/HostToolchainTest.cpp)
target_link_libraries(GoppUnitTests PRIVATE
  goppBasic goppLex goppAST goppParse goppSema goppMangle goppFormat goppBuild goppCodeGen goppImport
  GTest::gtest_main)
target_compile_definitions(GoppUnitTests PRIVATE
  GOPP_TEST_INPUTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Build/Inputs"
  GOPP_EXAMPLES_DIR="${GOPP_SOURCE_DIR}/docs/spec/examples"
  GOPP_CONFORMANCE_DIR="${GOPP_SOURCE_DIR}/tests/sema/conformance"
  GOPP_FUZZ_CORPUS_DIR="${GOPP_SOURCE_DIR}/tests/fuzz/corpus"
  GOPP_SPEC_DIR="${GOPP_SOURCE_DIR}/docs/spec"
  GOPP_FUZZ_SYMBOL_DIR="${GOPP_SOURCE_DIR}/tests/fuzz/symbol-corpus"
  GOPP_FUZZ_REGRESSIONS_DIR="${GOPP_SOURCE_DIR}/tests/fuzz/regressions")

configure_file(lit.site.cfg.py.in lit.site.cfg.py @ONLY)

add_custom_target(check-unit
  COMMAND ${GOPP_LIT} ${GOPP_LIT_ARGS} ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS GoppUnitTests
  USES_TERMINAL)
