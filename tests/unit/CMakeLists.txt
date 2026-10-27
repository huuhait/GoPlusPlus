find_package(GTest REQUIRED)

add_executable(GoppUnitTests
  Basic/VersionTest.cpp
  Build/HostToolchainTest.cpp)
target_link_libraries(GoppUnitTests PRIVATE
  goppBasic goppBuild goppCodeGen goppImport GTest::gtest_main)
target_compile_definitions(GoppUnitTests PRIVATE
  GOPP_TEST_INPUTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Build/Inputs")

configure_file(lit.site.cfg.py.in lit.site.cfg.py @ONLY)

add_custom_target(check-unit
  COMMAND ${GOPP_LIT} ${GOPP_LIT_ARGS} ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS GoppUnitTests
  USES_TERMINAL)
