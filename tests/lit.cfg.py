import os
import shutil
import sys

import lit.formats

config.name = "Go++"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".test", ".gopp"]
config.test_source_root = os.path.join(config.gopp_source_dir, "tests")
config.test_exec_root = os.path.join(config.gopp_binary_dir, "tests")
config.excludes = ["unit", "Inputs", "lit.cfg.py", "lit.site.cfg.py.in"]

# `gopp` and FileCheck must be found by name inside tests.
config.environment["PATH"] = os.pathsep.join(
    [config.gopp_tools_dir, config.llvm_tools_dir, config.environment["PATH"]]
)
# Nix wrappers and the host probe read these.
for var in ("NIX_CFLAGS_COMPILE", "NIX_LDFLAGS", "HOME", "TMPDIR"):
    if var in os.environ:
        config.environment[var] = os.environ[var]

# Tests must not depend on the user's locale.
config.environment["LC_ALL"] = "C"

# Slow tests are opt-in: they say "REQUIRES: long" and run via `ninja check-long`.
if lit_config.params.get("run_long"):
    config.available_features.add("long")

# The Go tool chain, for the tests that compare `gopp fmt` with gofmt and run it
# over the standard library (they also need "long").
if shutil.which("go") and shutil.which("gofmt"):
    config.available_features.add("go")

# For test inputs that are generated (too big or too repetitive to check in).
config.substitutions.append(("%python", sys.executable))
config.substitutions.append(("%gopp", os.path.join(config.gopp_tools_dir, "gopp")))
# The spec examples (docs/spec/examples/<name>/main.gopp) must always parse.
config.substitutions.append(
    ("%examples", os.path.join(config.gopp_source_dir, "docs", "spec", "examples"))
)

# Where the shared libLLVM.so / libclang-cpp.so of the configured LLVM live
# (empty for a static build). A test checks that gopp loads exactly these.
if config.llvm_lib_dirs.strip():
    config.available_features.add("shared-llvm")
config.substitutions.append(("%llvm_lib_dirs", config.llvm_lib_dirs))
