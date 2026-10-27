find_package(GTest REQUIRED)

add_executable(GoppUnitTests
  Basic/VersionTest.cpp
  AST/ASTTest.cpp
  Basic/DiagnosticsTest.cpp
  Basic/SourceManagerTest.cpp
  Basic/UnicodeTest.cpp
  Lex/GoScannerPortTest.cpp
  Lex/LexerExtrasTest.cpp
  Parse/GoParserPortTest.cpp
  Parse/ParserTest.cpp
  Build/HostToolchainTest.cpp)
target_link_libraries(GoppUnitTests PRIVATE
  goppBasic goppLex goppAST goppParse goppBuild goppCodeGen goppImport
  GTest::gtest_main)
target_compile_definitions(GoppUnitTests PRIVATE
  GOPP_TEST_INPUTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Build/Inputs"
  GOPP_EXAMPLES_DIR="${GOPP_SOURCE_DIR}/docs/spec/examples")

configure_file(lit.site.cfg.py.in lit.site.cfg.py @ONLY)

add_custom_target(check-unit
  COMMAND ${GOPP_LIT} ${GOPP_LIT_ARGS} ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS GoppUnitTests
  USES_TERMINAL)
