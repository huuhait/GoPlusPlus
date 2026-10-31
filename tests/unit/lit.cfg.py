import lit.formats

# Each GoogleTest case becomes one lit test, so it runs in parallel and shows
# up in the slowest-tests report. Executables are named *Tests.
config.name = "Go++ unit"
config.test_format = lit.formats.GoogleTest(".", "Tests")
config.test_source_root = config.gopp_bin_dir
config.test_exec_root = config.gopp_bin_dir

# The GIR verifier's V085 reports a use whose check ran out of its search
# budget (gopp/GIR/Verify.h): no module the unit tests verify may reach it.
config.environment["GOPP_VERIFY_BUDGET_STRICT"] = "1"
