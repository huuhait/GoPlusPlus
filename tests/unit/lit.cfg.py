import lit.formats

# Each GoogleTest case becomes one lit test, so it runs in parallel and shows
# up in the slowest-tests report. Executables are named *Tests.
config.name = "Go++ unit"
config.test_format = lit.formats.GoogleTest(".", "Tests")
config.test_source_root = config.gopp_bin_dir
config.test_exec_root = config.gopp_bin_dir
