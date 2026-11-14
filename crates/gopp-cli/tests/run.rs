//! The run tests: every program of `tests/run` is built and run in one
//! `gopp test-run`, and what it writes and how it ends is compared with
//! what its files say.
//!
//! A program is a `.gopp` file; a `.c` file of its name beside it is
//! compiled and linked with it. Its `.out` file beside it has what it
//! writes, the standard error stream in the standard output
//! in the order of the writes; a first line `// exit: N` says the
//! status that it ends with, which is 0 without one. The offsets of a
//! trace are written `+0x?`. `GOPP_RUN_UPDATE=1` writes the `.out`
//! files anew.

#![allow(clippy::unwrap_used, reason = "test helpers outside #[test] functions")]

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::{Value, json};

fn decoded(base64: &str) -> Vec<u8> {
    let digit = |letter: u8| match letter {
        b'A'..=b'Z' => letter - b'A',
        b'a'..=b'z' => letter - b'a' + 26,
        b'0'..=b'9' => letter - b'0' + 52,
        b'+' => 62,
        _ => 63,
    };
    let mut bytes = Vec::new();
    for group in base64.as_bytes().chunks(4) {
        let kept = group.iter().filter(|&&letter| letter != b'=').count();
        let bits = group
            .iter()
            .filter(|&&letter| letter != b'=')
            .fold(0u32, |bits, &letter| bits << 6 | u32::from(digit(letter)));
        let bits = bits << (6 * (4 - kept));
        bytes.extend_from_slice(&bits.to_be_bytes()[1..kept]);
    }
    bytes
}

/// What a program wrote, with the offsets of a trace taken out: every
/// build has its own.
fn written(result: &Value) -> String {
    let bytes = decoded(result["stdout"].as_str().unwrap_or_default());
    let text = String::from_utf8_lossy(&bytes);
    let lines = text.split_inclusive('\n').map(|line| {
        let offset = line.strip_prefix("\t+0x").is_some_and(|digits| {
            let digits = digits.trim_end();
            !digits.is_empty() && digits.bytes().all(|digit| digit.is_ascii_hexdigit())
        });
        match offset {
            true => "\t+0x?\n",
            false => line,
        }
    });
    lines.collect()
}

/// The programs of `tests/run` whose names `wanted` takes.
fn programs(wanted: impl Fn(&str) -> bool) -> Vec<PathBuf> {
    let dir = Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/run"));
    let mut programs: Vec<PathBuf> = fs::read_dir(dir)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| path.extension().is_some_and(|ext| ext == "gopp"))
        .filter(|path| wanted(&id(path)))
        .collect();
    programs.sort();
    programs
}

fn id(program: &Path) -> String {
    program.file_stem().unwrap().to_str().unwrap().to_owned()
}

/// Where the trace of a panic starts in what a program writes.
const TRACE: &str = "\ngoroutine 1 [running]:\n";

/// Whether a program that wrote `written` did what `expected` says, but
/// for the functions that Clang put into their callers: before the
/// trace of a panic the two are the same, and the trace names functions
/// of the expected trace, in their order, and no other.
fn same_but_inlined(written: &str, expected: &str) -> bool {
    let (Some((said, trace)), Some((meant, whole))) =
        (written.split_once(TRACE), expected.split_once(TRACE))
    else {
        return written == expected;
    };
    let mut rest = whole.lines();
    said == meant
        && trace
            .lines()
            .all(|line| rest.any(|expected| expected == line))
}

/// Builds and runs the programs in one `gopp test-run` with the flags,
/// and says which of them did not do what its files say.
fn wrong_among(programs: &[PathBuf], flags: &[&str]) -> Vec<String> {
    wrong_by(programs, flags, |written, expected| written == expected)
}

/// `wrong_among`, with what says whether what a program wrote is what
/// its file has.
fn wrong_by(programs: &[PathBuf], flags: &[&str], same: fn(&str, &str) -> bool) -> Vec<String> {
    let manifest: String = programs
        .iter()
        .map(|program| {
            // A file of C beside a program is the C that it calls.
            let of_c = program.with_extension("c");
            let mut inputs = vec![program.clone()];
            inputs.extend(of_c.exists().then_some(of_c));
            let job = json!({"v": 1, "id": id(program), "inputs": inputs, "merge_output": true});
            format!("{job}\n")
        })
        .collect();
    let work = tempfile::tempdir().unwrap();
    let jobs = work.path().join("jobs");
    fs::write(&jobs, manifest).unwrap();
    let cache = Path::new(env!("CARGO_TARGET_TMPDIR")).join("gopp-cache");
    let output = Command::new(env!("CARGO_BIN_EXE_gopp"))
        .args(["test-run", "-manifest"])
        .arg(&jobs)
        .args(flags)
        .env("GOPPCACHE", cache)
        .output()
        .unwrap();
    let said = String::from_utf8_lossy(&output.stderr);
    assert_eq!(output.status.code(), Some(0), "{said}");
    let results: BTreeMap<String, Value> = String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(|line| serde_json::from_str::<Value>(line).unwrap())
        .map(|result| (result["id"].as_str().unwrap().to_owned(), result))
        .collect();
    assert_eq!(results.len(), programs.len(), "two programs of one name");
    let update = std::env::var_os("GOPP_RUN_UPDATE").is_some();
    let mut wrong = Vec::new();
    for program in programs {
        let result = &results[&id(program)];
        let text = fs::read_to_string(program).unwrap();
        let status = text
            .lines()
            .next()
            .and_then(|line| line.strip_prefix("// exit: "));
        let status: i64 = status.map_or(0, |status| status.trim().parse().unwrap());
        let ended = (result["outcome"].as_str(), result["exit"].as_i64());
        if ended != (Some("exited"), Some(status)) {
            let message = result["message"].as_str().unwrap_or_default();
            wrong.push(format!("{}: ended as {ended:?}\n{message}", id(program)));
            continue;
        }
        // The trace of a program that ends while the stack is unwound
        // has other functions on Windows, where the unwinder runs on
        // top of the stack: such a program has a file for Windows.
        let other = program.with_extension("windows.out");
        let golden = match cfg!(windows) && other.exists() {
            true => other,
            false => program.with_extension("out"),
        };
        let written = written(result);
        if update {
            fs::write(&golden, &written).unwrap();
        }
        let expected = fs::read_to_string(&golden).unwrap_or_default();
        if !same(&written, &expected) {
            wrong.push(format!(
                "{}: wrote\n{written}--- and its file says\n{expected}---",
                id(program)
            ));
        }
    }
    wrong
}

#[test]
fn every_program_of_tests_run_does_what_its_files_say() {
    let programs = programs(|_| true);
    assert!(programs.len() >= 8);
    let wrong = wrong_among(&programs, &[]);
    assert!(wrong.is_empty(), "{}", wrong.join("\n"));
}

/// A program that is built for a debugger has its variables in blocks
/// of C and does the same.
#[test]
#[ignore = "long: builds every run test once more, for a debugger"]
fn every_program_does_the_same_when_it_is_built_for_a_debugger() {
    let programs = programs(|_| true);
    let wrong = wrong_among(&programs, &["--debug"]);
    assert!(wrong.is_empty(), "{}", wrong.join("\n"));
    // And when Clang optimizes what is built so.
    let wrong = wrong_by(&programs, &["--debug", "--opt=2"], same_but_inlined);
    assert!(wrong.is_empty(), "{}", wrong.join("\n"));
}

/// What crosses to C is passed as C passes it at every level of
/// Clang: the C side is compiled with the program. And a product is
/// fused with no sum at any level.
#[test]
#[ignore = "long: builds the runtime and the programs that call C at three levels"]
fn the_programs_that_call_c_do_the_same_at_every_level() {
    let programs = programs(|name| name.starts_with("cabi_") || name.starts_with("float_"));
    assert!(programs.len() >= 4);
    for level in ["--opt=0", "--opt=2", "--opt=3"] {
        for real in [&[][..], &["--real"]] {
            let flags = [&[level], real].concat();
            let wrong = wrong_among(&programs, &flags);
            assert!(wrong.is_empty(), "{flags:?}\n{}", wrong.join("\n"));
        }
    }
}

/// A program means the same at every level of Clang (pipelines.md §1):
/// what it writes, the message of a panic and the status that it ends
/// with. Above the first level Clang puts functions into their callers,
/// and the trace of a panic does not name those.
#[test]
#[ignore = "long: builds the runtime and every run test at six levels"]
fn every_program_does_the_same_at_every_level() {
    let programs = programs(|_| true);
    let modes: &[&[&str]] = match cfg!(target_os = "linux") {
        true => &[&[], &["--real"]],
        false => &[&[]],
    };
    for level in [
        "--opt=0", "--opt=1", "--opt=2", "--opt=3", "--opt=s", "--opt=z",
    ] {
        for mode in modes {
            let flags = [&[level], *mode].concat();
            let same = match level {
                "--opt=0" => |written: &str, expected: &str| written == expected,
                _ => same_but_inlined,
            };
            let wrong = wrong_by(&programs, &flags, same);
            assert!(wrong.is_empty(), "{flags:?}\n{}", wrong.join("\n"));
        }
    }
}

#[test]
fn a_trace_may_lack_the_functions_that_were_put_into_their_callers() {
    let expected =
        "1\npanic: no\n\ngoroutine 1 [running]:\nmain.f()\n\t+0x?\nmain.main()\n\t+0x?\n";
    let head = "1\npanic: no\n\ngoroutine 1 [running]:\n";
    assert!(same_but_inlined(expected, expected));
    assert!(same_but_inlined(
        &format!("{head}main.main()\n\t+0x?\n"),
        expected
    ));
    assert!(same_but_inlined(head, expected));
    // Another function, another order, another message, a trace that
    // is missing.
    assert!(!same_but_inlined(
        &format!("{head}main.g()\n\t+0x?\n"),
        expected
    ));
    let turned = format!("{head}main.main()\n\t+0x?\nmain.f()\n\t+0x?\n");
    assert!(!same_but_inlined(&turned, expected));
    assert!(!same_but_inlined(&expected.replace("no", "yes"), expected));
    assert!(!same_but_inlined("1\npanic: no\n", expected));
    assert!(same_but_inlined("1\n", "1\n") && !same_but_inlined("1\n", "2\n"));
}
