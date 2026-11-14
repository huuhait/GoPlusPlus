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

/// Builds and runs the programs in one `gopp test-run` with the flags,
/// and says which of them did not do what its files say.
fn wrong_among(programs: &[PathBuf], flags: &[&str]) -> Vec<String> {
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
        if written != expected {
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
    let wrong = wrong_among(&programs(|_| true), &["--debug"]);
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
