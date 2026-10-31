import os
import platform
import shutil
import sys

import lit.formats

# tests/lit_gopp_format.py: the test format class lives in a module, because lit
# pickles it for its worker processes.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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

# The scripts that start a process per file or per program in many threads
# share this many slots (tests/procslots.py): a third of the cores, past which
# the kernel time of starting and ending processes grows several times. That
# point is the machine's, so the slots are the user's, shared by every test run
# of every build directory (several worktrees test at once); without a runtime
# directory, those of this build directory.
config.environment["GOPP_TEST_SLOTS"] = str(max(2, (os.cpu_count() or 6) // 3))
config.environment["GOPP_TEST_SLOTS_DIR"] = (
    os.path.join(os.environ["XDG_RUNTIME_DIR"], "gopp-proc-slots")
    if os.path.isdir(os.environ.get("XDG_RUNTIME_DIR", ""))
    else os.path.join(config.test_exec_root, ".proc-slots"))

# Slow tests are opt-in: they say "REQUIRES: long" and run via `ninja check-long`.
if lit_config.params.get("run_long"):
    config.available_features.add("long")

# A sanitizer build reserves terabytes of address space for its shadow, so a
# test that lowers the address-space limit (`ulimit -v`) needs "no-sanitizer".
if not config.gopp_sanitizer.strip():
    config.available_features.add("no-sanitizer")

# What the tests that run a built program can rely on (P04.02, P04.03): a
# program runs only on the architecture of the machine that builds it.
if platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64"):
    config.available_features.add("host-linux-amd64")
if platform.system() == "Linux" and platform.machine() in ("aarch64", "arm64"):
    config.available_features.add("host-linux-arm64")

# Local arm64 runs (P04.03, owner decision M6): the flake provides user-mode
# QEMU and an aarch64 glibc (GOPP_AARCH64_SYSROOT) on x86-64 hosts. The run
# tests build for linux/arm64 against that sysroot's lib directory and run the
# result with `qemu-aarch64 -L <sysroot>`; they compare with the amd64 run.
_sysroot = os.environ.get("GOPP_AARCH64_SYSROOT", "")
if (
    _sysroot
    and os.path.isfile(os.path.join(_sysroot, "lib", "crt1.o"))
    and shutil.which("qemu-aarch64")
):
    config.available_features.add("qemu-aarch64")
    config.substitutions.append(("%aarch64_sysroot", _sysroot))

# The debugger tests (P04.12) script gdb and lldb in batch mode on a program
# the compiler built; both come from the flake. They run a program, so the
# host must be a target too.
for _dbg in ("gdb", "lldb"):
    if shutil.which(_dbg):
        config.available_features.add(_dbg)

# The C ABI tests (P04.11) compile C with Clang for the target: the flake's
# clang, which compiles for aarch64 against no headers (the generated C
# declares what it uses). Tests that need it say "REQUIRES: clang".
if shutil.which("clang"):
    config.available_features.add("clang")

# M0 on an FHS distro (P04.02 Done-when): run/debian.test runs on a Debian or
# Ubuntu host; run/debian-container.long.test builds and runs hello in Debian
# containers from any host. The second is opt-in (it needs docker or podman
# and the image, or the network for apt). The variable is the build image,
# then optionally the image the program runs in:
#   GOPP_TEST_DEBIAN_IMAGE=debian:stable-slim ninja -C build check-long
#   GOPP_TEST_DEBIAN_IMAGE="golang:1.25 debian:bookworm" ...
if os.path.exists("/etc/debian_version"):
    config.available_features.add("debian")
if (shutil.which("docker") or shutil.which("podman")) and os.environ.get(
    "GOPP_TEST_DEBIAN_IMAGE"
):
    config.available_features.add("debian-container")
    config.substitutions.append(
        ("%debian_image", os.environ["GOPP_TEST_DEBIAN_IMAGE"])
    )

# A time limit for the default suite's stress inputs: 10 s, five times that in
# a sanitizer build, which is about five times slower. The long suite judges
# time by CPU-time ratios (tests/sema/Inputs/check-linear.py), never by it.
config.substitutions.append(
    ("%{timeout10}", "timeout %d" % (50 if config.gopp_sanitizer.strip() else 10)))
# A ceiling on the CPU time of one gopp run, in units of `gopp version`
# (tests/cpu-limit.py): `%{cpu_limit} DEFAULT SANITIZER [--status K] -- GOPP
# ARGS`, the second number in a sanitizer build. For size runs whose input
# cannot grow enough for a ratio check.
config.substitutions.append(
    ("%{cpu_limit}", "%s %s --sanitizer=%d" % (
        sys.executable, os.path.join(config.gopp_source_dir, "tests", "cpu-limit.py"),
        1 if config.gopp_sanitizer.strip() else 0)))
# The Slow factor of the generated sweeps of check-long (the `Slow` constant
# of the C++ stress tests): a sanitizer build runs one in five of their
# builds (`--slow` of run-cc.py and run-cabi.py, P04.18), a normal build all.
config.substitutions.append(
    ("%{slow}", "5" if config.gopp_sanitizer.strip() else "1"))

# The Go tool chain, for the tests that compare `gopp fmt` with gofmt and run it
# over the standard library (they also need "long").
if shutil.which("go") and shutil.which("gofmt"):
    config.available_features.add("go")

# Every link of a run test would start the host C compiler three or four times
# to find crt1.o and the dynamic linker (about 0.1 s of a 0.11 s link). The
# probe runs once here; `%gopp_link_flags` hands its answer to `gopp build`, so
# a run test skips it. run/hello.test and run/debian.test do not use it: they
# keep the real probe path (P00.12) tested. This must come before `%gopp`.
def host_link_flags():
    import subprocess

    try:
        out = subprocess.run(
            [os.path.join(config.gopp_tools_dir, "gopp"), "host-toolchain"],
            capture_output=True, text=True, timeout=60, check=True,
            env=dict(os.environ, **config.environment),
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    found = dict(l.split(": ", 1) for l in out.splitlines() if ": " in l)
    crt1, ld = found.get("crt1"), found.get("dynamic-linker")
    if not crt1 or not ld:
        return ""
    return "-crt-dir=%s -dynamic-linker=%s" % (os.path.dirname(crt1), ld)

_link_flags = host_link_flags()
config.substitutions.append(("%gopp_link_flags", _link_flags))
# For the run scripts (tests/run/Inputs/runlib.py).
config.environment["GOPP_LINK_FLAGS"] = _link_flags
config.environment["GOPP"] = os.path.join(config.gopp_tools_dir, "gopp")

# For test inputs that are generated (too big or too repetitive to check in).
config.substitutions.append(("%python", sys.executable))
# A nested lit, for the tests of this suite's own test format (the batch of
# the run tests: tests/run/runner-batch*.test).
config.substitutions.append(
    ("%lit", '%s -c "from lit.main import main; main()"' % sys.executable))
config.substitutions.append(("%gopp_unit_tests", os.path.join(config.gopp_tools_dir, "GoppUnitTests")))
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

# The fast runner for the run tests (P04.19, D4.14). The run scripts
# (tests/run/Inputs/run-*.py) take their program as `--src` and run it through
# `gopp test-run`: compiled in one process, run in an ORC JIT in an executor
# process, no link. Before the tests of tests/run start, ONE `gopp test-run`
# compiles and runs the programs of all of them on every core; each test then
# reads its program's result (same stdout, stderr and exit status as a real
# binary) from $GOPP_FASTRUN_DIR. A test run alone runs its own program the
# same way, and so does every test when the batch fails (a warning says so).
# Switches back to real binaries (`gopp build` + exec):
#   lit -Dreal_binaries=1 ...     all tests (`ninja check-run-real`)
#   GOPP_FAST_RUN=0               same, from the environment
#   --real in a test's RUN line   that one test
# -Dfastrun_batch_gopp=CMD (TEST ONLY) runs the batch with CMD in place of
# gopp, to make it fail (tests/run/runner-batch-fallback.test).
if lit_config.params.get("real_binaries") or os.environ.get("GOPP_FAST_RUN") == "0":
    config.environment["GOPP_TEST_REAL"] = "1"


import lit_gopp_format  # noqa: E402  (tests/lit_gopp_format.py)

config.test_format = lit_gopp_format.GoppTestFormat(
    source_dir=config.gopp_source_dir,
    binary_dir=config.gopp_binary_dir,
    tools_dir=config.gopp_tools_dir,
    run_long=bool(lit_config.params.get("run_long")),
)
