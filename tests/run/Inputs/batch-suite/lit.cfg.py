# A small lit suite for tests/run/runner-batch.test and
# runner-batch-fallback.test (P04.19, manager decision M3): the batch of the
# run tests (tests/lit_gopp_format.py) on two tests, in a nested lit run.
# Parameters: gopp, gopp_source_dir, exec_root (and fastrun_batch_gopp, which
# the format reads; fastrun_binary_dir, where the batch keeps its results). The tests run in a copy of this directory.
import os
import sys

src = lit_config.params["gopp_source_dir"]
gopp = lit_config.params["gopp"]
sys.path.insert(0, os.path.join(src, "tests"))
import lit_gopp_format  # noqa: E402

config.name = "batch-suite"
config.suffixes = [".test"]
config.excludes = ["prog"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))
config.test_exec_root = lit_config.params["exec_root"]
config.environment["GOPP"] = gopp
config.environment["GOPP_RUNLIB_TRACE"] = "1"
config.substitutions.append(("%python", sys.executable))
config.substitutions.append(("%inputs", os.path.join(src, "tests", "run", "Inputs")))
config.test_format = lit_gopp_format.GoppTestFormat(
    source_dir=src,
    binary_dir=lit_config.params.get("fastrun_binary_dir", lit_config.params["exec_root"]),
    tools_dir=os.path.dirname(gopp),
    run_long=False,
)
