//! The run tests: every program of `tests/run` is built and run in one
//! `gopp test-run`, and what it writes and how it ends is compared with
//! what its files say.
//!
//! A program is a `.gopp` or a `.go` file. Its `.out` file beside it
//! has what it writes, the standard error stream in the standard output
//! in the order of the writes; a first line `// exit: N` says the
//! status that it ends with, which is 0 without one. The offsets of a
//! trace are written `+0x?`. `GOPP_RUN_UPDATE=1` writes the `.out`
//! files anew.

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

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn every_program_of_tests_run_does_what_its_files_say() {
    let dir = Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/run"));
    let mut programs: Vec<PathBuf> = fs::read_dir(dir)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            path.extension()
                .is_some_and(|ext| ext == "gopp" || ext == "go")
        })
        .collect();
    programs.sort();
    assert!(programs.len() >= 8);
    let id = |program: &Path| program.file_stem().unwrap().to_str().unwrap().to_owned();
    let manifest: String = programs
        .iter()
        .map(|program| {
            let job = json!({"v": 1, "id": id(program), "inputs": [program], "merge_output": true});
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
    for program in &programs {
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
        let golden = program.with_extension("out");
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
    assert!(wrong.is_empty(), "{}", wrong.join("\n"));
}
