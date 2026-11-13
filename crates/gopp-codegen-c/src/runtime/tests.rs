use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_target::{TargetLayout, TargetSpec};
use rayon::prelude::*;

use super::{FUNCS, Form, build, header};
use crate::driver::{ClangDriver, DriverError, OptLevel, Output};
use crate::toolchain::HostToolchain;

fn driver<'t>(toolchain: &'t HostToolchain, target: &str, opt: OptLevel) -> ClangDriver<'t> {
    ClangDriver {
        toolchain,
        target: TargetSpec::named(target).unwrap(),
        opt,
        strict: true,
    }
}

#[test]
fn the_header_says_what_both_sides_share() {
    insta::assert_snapshot!(header(TargetLayout::new(64)));
}

/// A unit that is the header and a table of every function of it.
fn names_all(layout: TargetLayout) -> String {
    let names: Vec<String> = (FUNCS.iter())
        .map(|func| format!("(void *){}", func.name))
        .collect();
    format!(
        "{}\nvoid *const all[] = {{{}}};\n\nint main(void) {{ return all[0] == 0; }}\n",
        header(layout),
        names.join(", ")
    )
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn the_header_compiles_for_both_targets_and_its_checks_bite() {
    let toolchain = HostToolchain::discover().unwrap();
    for target in ["linux/amd64", "windows/amd64"] {
        let driver = driver(&toolchain, target, OptLevel::O0);
        let compiled = |layout: TargetLayout| {
            let dir = tempfile::tempdir().unwrap();
            let object = dir.path().join("unit.o");
            driver.build(&names_all(layout), dir.path(), &object, Output::Object, &[])
        };
        compiled(driver.target.layout()).unwrap_or_else(|error| panic!("{target}: {error}"));
        // The header of a target with pointers of another width.
        let refused = compiled(TargetLayout::new(32)).unwrap_err().to_string();
        assert!(refused.contains("the width of a pointer"), "{refused}");
        assert!(refused.contains("the layout of gopp_iface"), "{refused}");
    }
}

fn magic(path: &Path) -> Vec<u8> {
    let bytes = std::fs::read(path).unwrap();
    bytes[..4].to_vec()
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn the_objects_are_made_for_both_targets_without_a_c_library() {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let made = |target: &str| {
        let driver = driver(&toolchain, target, OptLevel::O2);
        build(&driver, Form::Objects, cache.path()).unwrap()
    };
    let (elf, coff) = (made("linux/amd64"), made("windows/amd64"));
    assert_eq!((elf.len(), coff.len()), (8, 8));
    for object in &elf {
        assert_eq!(magic(object), b"\x7fELF");
    }
    for object in &coff {
        assert_eq!(magic(object)[..2], [0x64, 0x86]);
    }
    // The shared form is for the test runner, which is written for
    // Linux.
    let windows = driver(&toolchain, "windows/amd64", OptLevel::O2);
    let refused = build(&windows, Form::Shared, cache.path()).unwrap_err();
    assert!(
        matches!(refused, DriverError::NoSharedRuntime { .. }),
        "{refused}"
    );
}

/// The directories of the builds that a cache holds.
fn kept_in(cache: &Path) -> Vec<PathBuf> {
    let entries = std::fs::read_dir(cache.join("runtime")).unwrap();
    let mut kept: Vec<PathBuf> = entries.map(|entry| entry.unwrap().path()).collect();
    kept.sort();
    kept
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn the_runtime_is_built_once_for_a_target_and_its_flags() {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let plain = driver(&toolchain, "linux/amd64", OptLevel::O0);
    // Many builds at once of what is not there yet.
    let all: Vec<Vec<PathBuf>> = (0..16)
        .into_par_iter()
        .map(|_| build(&plain, Form::Objects, cache.path()).unwrap())
        .collect();
    assert!(all.iter().all(|objects| *objects == all[0]));
    let kept = kept_in(cache.path());
    assert_eq!(kept.len(), 1, "{kept:?}");
    // What is kept is not made again.
    let written = |path: &Path| std::fs::metadata(path).unwrap().modified().unwrap();
    let first = written(&all[0][0]);
    assert_eq!(build(&plain, Form::Objects, cache.path()).unwrap(), all[0]);
    assert_eq!(written(&all[0][0]), first);
    // The driver of a program, which lets warnings be, has the same.
    let loose = ClangDriver {
        strict: false,
        ..driver(&toolchain, "linux/amd64", OptLevel::O0)
    };
    assert_eq!(build(&loose, Form::Objects, cache.path()).unwrap(), all[0]);
    // Other flags, another target and another form are other builds.
    let optimized = driver(&toolchain, "linux/amd64", OptLevel::O2);
    let windows = driver(&toolchain, "windows/amd64", OptLevel::O0);
    assert_ne!(
        build(&optimized, Form::Objects, cache.path()).unwrap(),
        all[0]
    );
    assert_ne!(
        build(&windows, Form::Objects, cache.path()).unwrap(),
        all[0]
    );
    assert_eq!(kept_in(cache.path()).len(), 3);
    // A directory that has the name and is not the build makes way.
    std::fs::write(kept[0].join("key"), "another").unwrap();
    assert_eq!(build(&plain, Form::Objects, cache.path()).unwrap(), all[0]);
    assert_eq!(kept_in(cache.path()).len(), 3);
    assert_ne!(std::fs::read(kept[0].join("key")).unwrap(), b"another");
}

/// What a program of C that is linked with the runtime writes to the
/// standard error stream for each of `runs`, which are its argument,
/// and the status that it ends with. The program is the header and
/// `body`.
fn ran(opt: OptLevel, body: &str, runs: &[&str]) -> Vec<(String, Option<i32>)> {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let driver = driver(&toolchain, "linux/amd64", opt);
    let runtime = build(&driver, Form::Objects, cache.path()).unwrap();
    let unit = format!("{}\n{body}", header(driver.target.layout()));
    let dir = tempfile::tempdir().unwrap();
    let program = dir.path().join("program");
    driver
        .build(&unit, dir.path(), &program, Output::Executable, &runtime)
        .unwrap_or_else(|error| panic!("{error}"));
    let one = |run: &&str| {
        let output = Command::new(&program).arg(run).output().unwrap();
        assert_eq!(output.stdout, b"");
        let said = String::from_utf8_lossy(&output.stderr).into_owned();
        (said, output.status.code())
    };
    runs.iter().map(one).collect()
}

/// Every failure with what Go says for it; the texts are those of
/// `runtime/error.go` and `runtime/panic.go` of Go.
const FAILURES: [(&str, &str); 22] = [
    (
        "_gopp_panic_bounds(5, 3, true)",
        "runtime error: index out of range [5] with length 3",
    ),
    (
        "_gopp_panic_bounds((uint64_t)-1, 3, true)",
        "runtime error: index out of range [-1]",
    ),
    (
        "_gopp_panic_bounds((uint64_t)-1, 3, false)",
        "runtime error: index out of range [18446744073709551615] with length 3",
    ),
    (
        "_gopp_panic_slice_alen(5, 3, true)",
        "runtime error: slice bounds out of range [:5] with length 3",
    ),
    (
        "_gopp_panic_slice_alen((uint64_t)-7, 3, true)",
        "runtime error: slice bounds out of range [:-7]",
    ),
    (
        "_gopp_panic_slice_acap(5, 3, false)",
        "runtime error: slice bounds out of range [:5] with capacity 3",
    ),
    (
        "_gopp_panic_slice_acap((uint64_t)-1, 3, true)",
        "runtime error: slice bounds out of range [:-1]",
    ),
    (
        "_gopp_panic_slice_b(5, 3, true)",
        "runtime error: slice bounds out of range [5:3]",
    ),
    (
        "_gopp_panic_slice_b((uint64_t)-2, 3, true)",
        "runtime error: slice bounds out of range [-2:]",
    ),
    (
        "_gopp_panic_slice3_alen(5, 3, true)",
        "runtime error: slice bounds out of range [::5] with length 3",
    ),
    (
        "_gopp_panic_slice3_alen((uint64_t)-1, 3, true)",
        "runtime error: slice bounds out of range [::-1]",
    ),
    (
        "_gopp_panic_slice3_acap(5, 3, true)",
        "runtime error: slice bounds out of range [::5] with capacity 3",
    ),
    (
        "_gopp_panic_slice3_b(5, 3, true)",
        "runtime error: slice bounds out of range [:5:3]",
    ),
    (
        "_gopp_panic_slice3_b((uint64_t)-1, 3, true)",
        "runtime error: slice bounds out of range [:-1:]",
    ),
    (
        "_gopp_panic_slice3_c(5, 3, true)",
        "runtime error: slice bounds out of range [5:3:]",
    ),
    (
        "_gopp_panic_slice3_c((uint64_t)-1, 3, true)",
        "runtime error: slice bounds out of range [-1::]",
    ),
    (
        "_gopp_panic_slice_convert(4, 2, true)",
        "runtime error: cannot convert slice with length 2 to array or pointer to array with length 4",
    ),
    (
        "_gopp_panic_nil()",
        "runtime error: invalid memory address or nil pointer dereference",
    ),
    (
        "_gopp_panic_divide()",
        "runtime error: integer divide by zero",
    ),
    (
        "_gopp_panic_shift()",
        "runtime error: negative shift amount",
    ),
    (
        "_gopp_panic_must(text(\"os.Open(path)\"), text(\"main.gopp:12:9\"), text(\"open /nope: no such file or directory\"))",
        "must: os.Open(path) at main.gopp:12:9: open /nope: no such file or directory",
    ),
    (
        "_gopp_panic_bounds(9223372036854775807, (uint64_t)1 << 63, true)",
        "runtime error: index out of range [9223372036854775807] with length -9223372036854775808",
    ),
];

/// A string of Go++ for a string of C, and the first letter of the
/// argument as a number from 0.
const HELPERS: &str = "
static gopp_string text(const char *of) {
  uint64_t len = 0;
  while (of[len]) len++;
  return (gopp_string){(uint8_t *)of, len, 0};
}
";

#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn a_failure_says_what_go_says_and_ends_with_status_2() {
    let mut body = format!(
        "{HELPERS}\nint main(int argc, char **argv) {{\n  _gopp_rt_init(argc, argv);\n  \
         switch (argv[1][0] - 'A') {{\n"
    );
    for (at, (call, _)) in FAILURES.iter().enumerate() {
        body += &format!("  case {at}: {call};\n");
    }
    body += "  }\n  return 0;\n}\n";
    let letters: Vec<String> = (0..FAILURES.len())
        .map(|at| char::from(b'A' + at as u8).to_string())
        .collect();
    let runs: Vec<&str> = letters.iter().map(|letter| &**letter).collect();
    for opt in [OptLevel::O0, OptLevel::O2] {
        let results = ran(opt, &body, &runs);
        for ((call, message), (said, status)) in FAILURES.iter().zip(results) {
            // No function of Go++ is on the stack of this program.
            let expected = format!("panic: {message}\n\ngoroutine 1 [running]:\n");
            assert_eq!(said, expected, "{call}");
            assert_eq!(status, Some(2), "{call}");
        }
    }
}

#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn print_writes_each_value_as_go_does() {
    // Each value on a line of its own, with what a program of Go
    // prints for it.
    let values = [
        ("_gopp_print_bool(true)", "true"),
        ("_gopp_print_bool(false)", "false"),
        ("_gopp_print_int(0)", "0"),
        ("_gopp_print_int(-42)", "-42"),
        (
            "_gopp_print_int(9223372036854775807)",
            "9223372036854775807",
        ),
        (
            "_gopp_print_int(-9223372036854775807 - 1)",
            "-9223372036854775808",
        ),
        ("_gopp_print_uint(0)", "0"),
        (
            "_gopp_print_uint(18446744073709551615u)",
            "18446744073709551615",
        ),
        ("_gopp_print_float(0.0)", "+0.000000e+000"),
        ("_gopp_print_float(-0.0)", "-0.000000e+000"),
        ("_gopp_print_float(1.5)", "+1.500000e+000"),
        ("_gopp_print_float(-2.75)", "-2.750000e+000"),
        ("_gopp_print_float(0.1 + 0.2)", "+3.000000e-001"),
        ("_gopp_print_float(1e100)", "+1.000000e+100"),
        ("_gopp_print_float(123456789.0)", "+1.234568e+008"),
        ("_gopp_print_float(9.9999999)", "+1.000000e+001"),
        ("_gopp_print_float(5e-324)", "+4.940656e-324"),
        (
            "_gopp_print_float(1.7976931348623157e308)",
            "+1.797693e+308",
        ),
        ("_gopp_print_float((double)0.1f)", "+1.000000e-001"),
        ("_gopp_print_float((double)16777217.0f)", "+1.677722e+007"),
        ("_gopp_print_float(zero / zero)", "NaN"),
        ("_gopp_print_float(1 / zero)", "+Inf"),
        ("_gopp_print_float(-1 / zero)", "-Inf"),
        ("_gopp_print_string(text(\"\"))", ""),
        ("_gopp_print_string(text(\"a b\\tc\"))", "a b\tc"),
        ("_gopp_print_pointer(0)", "0x0"),
        ("_gopp_print_pointer((uint8_t *)0xdeadbeef)", "0xdeadbeef"),
        ("_gopp_print_space()", " "),
    ];
    let mut body = format!(
        "{HELPERS}\nint main(int argc, char **argv) {{\n  _gopp_rt_init(argc, argv);\n  \
         volatile double zero = 0;\n"
    );
    let mut expected = String::new();
    for (call, written) in values {
        body += &format!("  {call};\n  _gopp_print_newline();\n");
        expected += &format!("{written}\n");
    }
    body += "  return 0;\n}\n";
    for opt in [OptLevel::O0, OptLevel::O2] {
        assert_eq!(ran(opt, &body, &[""]), [(expected.clone(), Some(0))]);
    }
}

/// The offsets of a trace, which every build has its own.
fn without_offsets(trace: &str) -> String {
    let lines = trace.lines().map(|line| match line.starts_with("\t+0x") {
        true => "\t+0x",
        false => line,
    });
    lines.collect::<Vec<_>>().join("\n")
}

#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn a_trace_names_the_functions_that_a_unit_gave_and_no_other() {
    let body = "
#define KEPT __attribute__((noinline))
KEPT static int leaf(int at) {
  if (at > 2) _gopp_panic_divide();
  return at;
}
KEPT static int unnamed(int at) { return leaf(at + 1) + 1; }
KEPT static int middle(int at) { return unnamed(at + 1) + 1; }
KEPT static int outer(int at) { return middle(at + 1) + 1; }

static const gopp_func_name names[] = {
  {(void *)outer, \"main.outer\"},
  {(void *)leaf, \"main.Vec2.Add(main.Vec2)\"},
};
static gopp_func_table table = {names, 2, 0};
static const gopp_func_name more[] = {{(void *)middle, \"other/pkg.middle\"}};
static gopp_func_table second = {more, 1, 0};

int main(int argc, char **argv) {
  _gopp_rt_init(argc, argv);
  _gopp_rt_funcs(&table);
  _gopp_rt_funcs(&second);
  return outer(argc - 2);
}
";
    let expected = "panic: runtime error: integer divide by zero\n\ngoroutine 1 [running]:\n\
                    main.Vec2.Add(main.Vec2)()\n\t+0x\nother/pkg.middle()\n\t+0x\n\
                    main.outer()\n\t+0x";
    for opt in [OptLevel::O0, OptLevel::O2] {
        let results = ran(opt, body, &["fails", ""]);
        // With one argument the count is 2, and `leaf` gets 3.
        let (said, status) = &results[0];
        assert_eq!(without_offsets(said), expected, "{opt:?}");
        assert_eq!(*status, Some(2));
        assert!(
            said.lines()
                .all(|line| !line.starts_with("\t+0x") || line.len() > 4)
        );
    }
}

#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn the_runtime_defines_every_function_in_both_forms() {
    let toolchain = HostToolchain::discover().unwrap();
    let cache = tempfile::tempdir().unwrap();
    let driver = driver(&toolchain, "linux/amd64", OptLevel::O0);
    let layout = driver.target.layout();
    // A program that names them all, and one that fails a check.
    let fails = format!(
        "{}\nint main(int argc, char **argv) {{\n  _gopp_rt_init(argc, argv);\n  _gopp_panic_nil();\n}}\n",
        header(layout)
    );
    for form in [Form::Objects, Form::Shared] {
        let runtime = build(&driver, form, cache.path()).unwrap();
        let folder = runtime[0].parent().unwrap();
        for (unit, status) in [(names_all(layout), 0), (fails.clone(), 2)] {
            let dir = tempfile::tempdir().unwrap();
            let program = dir.path().join("program");
            driver
                .build(&unit, dir.path(), &program, Output::Executable, &runtime)
                .unwrap_or_else(|error| panic!("{form:?}: {error}"));
            let mut command = Command::new(&program);
            command.env("LD_LIBRARY_PATH", folder);
            let ran = command.output().unwrap();
            assert_eq!(ran.status.code(), Some(status), "{form:?}");
        }
    }
}
