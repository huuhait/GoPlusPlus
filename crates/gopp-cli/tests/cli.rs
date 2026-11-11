#![allow(clippy::unwrap_used, reason = "test helpers outside #[test] functions")]

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::{Mutex, PoisonError};

use tempfile::TempDir;

/// Held while a script is written and while a process is started. A process
/// started while another thread has a script open for writing inherits that
/// handle for a moment, and executing the script then fails as "text file busy".
static SPAWN: Mutex<()> = Mutex::new(());

fn gopp() -> Command {
    Command::new(env!("CARGO_BIN_EXE_gopp"))
}

fn run(command: &mut Command) -> Output {
    let _guard = SPAWN.lock().unwrap_or_else(PoisonError::into_inner);
    command.output().unwrap()
}

fn stdout(output: &Output) -> String {
    String::from_utf8_lossy(&output.stdout).into_owned()
}

fn stderr(output: &Output) -> String {
    String::from_utf8_lossy(&output.stderr).into_owned()
}

fn script(dir: &Path, name: &str, body: &str) -> PathBuf {
    let path = dir.join(name);
    let _guard = SPAWN.lock().unwrap_or_else(PoisonError::into_inner);
    fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    path
}

/// A `clang` that reports `clang_version` and points at an `ld.lld` reporting `lld_version`.
fn fake_toolchain(dir: &TempDir, clang_version: &str, lld_version: &str) -> PathBuf {
    let lld = script(
        dir.path(),
        "ld.lld",
        &format!("echo 'LLD {lld_version} (compatible with GNU linkers)'"),
    );
    script(
        dir.path(),
        "clang",
        &format!(
            "case $1 in\n\
             --version) echo 'clang version {clang_version}';;\n\
             -print-prog-name=ld.lld) echo '{}';;\n\
             esac",
            lld.display()
        ),
    )
}

fn host_toolchain(clang: &Path) -> Output {
    run(gopp().arg("host-toolchain").env("GOPP_CLANG", clang))
}

#[test]
fn version_names_the_compiler_and_the_pinned_clang() {
    let output = run(gopp().arg("version"));
    assert_eq!(
        stdout(&output),
        format!("gopp version {} (clang 23)\n", env!("CARGO_PKG_VERSION"))
    );
    assert_eq!(output.status.code(), Some(0));
}

#[test]
fn unknown_command_is_a_usage_error() {
    let output = run(gopp().arg("no-such-command"));
    assert_eq!(output.status.code(), Some(2));
}

#[test]
fn long_flags_take_one_dash() {
    let output = run(gopp().arg("-help"));
    assert_eq!(output.status.code(), Some(0));
    assert!(stdout(&output).contains("host-toolchain"));
}

#[test]
fn host_toolchain_prints_the_pinned_tools() {
    let dir = TempDir::new().unwrap();
    let clang = fake_toolchain(&dir, "23.1.0", "23.1.0");
    let output = host_toolchain(&clang);
    assert_eq!(
        stdout(&output),
        format!(
            "clang: {} (23.1.0)\nld.lld: {} (23.1.0)\n",
            clang.display(),
            dir.path().join("ld.lld").display()
        )
    );
    assert_eq!(output.status.code(), Some(0));
}

#[test]
fn host_toolchain_rejects_another_clang_major() {
    let dir = TempDir::new().unwrap();
    let clang = fake_toolchain(&dir, "22.1.8", "23.1.0");
    let output = host_toolchain(&clang);
    assert_eq!(
        stderr(&output),
        format!(
            "error: {} is clang 22.1.8, but gopp needs clang 23\n",
            clang.display()
        )
    );
    assert_eq!(output.status.code(), Some(1));
}

#[test]
fn host_toolchain_rejects_another_lld_major() {
    let dir = TempDir::new().unwrap();
    let clang = fake_toolchain(&dir, "23.1.0", "21.1.8");
    let output = host_toolchain(&clang);
    assert!(stderr(&output).ends_with("is LLD 21.1.8, but gopp needs LLD 23\n"));
    assert_eq!(output.status.code(), Some(1));
}

#[test]
fn host_toolchain_reports_a_clang_that_cannot_run() {
    let output = host_toolchain(Path::new("/nonexistent/clang"));
    assert!(stderr(&output).starts_with("error: cannot run /nonexistent/clang: "));
    assert_eq!(output.status.code(), Some(1));
}

#[test]
fn host_toolchain_reports_a_missing_lld() {
    let dir = TempDir::new().unwrap();
    let clang = script(
        dir.path(),
        "clang",
        "case $1 in\n\
         --version) echo 'clang version 23.1.0';;\n\
         -print-prog-name=ld.lld) echo ld.lld;;\n\
         esac",
    );
    let output = host_toolchain(&clang);
    assert!(stderr(&output).starts_with("error: ld.lld not found: "));
    assert_eq!(output.status.code(), Some(1));
}

fn write_source(dir: &TempDir, name: &str, text: &str) -> PathBuf {
    let path = dir.path().join(name);
    fs::write(&path, text).unwrap();
    path
}

#[test]
fn parse_prints_the_tree_on_request() {
    let dir = TempDir::new().unwrap();
    let file = write_source(
        &dir,
        "main.gopp",
        "package main\n\nfunc main() { run!() }\n",
    );
    let quiet = run(gopp().arg("parse").arg(&file));
    assert_eq!(
        (stdout(&quiet), stderr(&quiet)),
        (String::new(), String::new())
    );
    assert_eq!(quiet.status.code(), Some(0));

    let output = run(gopp().args(["parse", "-emit=ast"]).arg(&file));
    assert!(stdout(&output).starts_with("File package=main\n  Func main @14..36\n"));
    assert!(stdout(&output).contains("Call unwrap"));
    assert_eq!(output.status.code(), Some(0));
}

#[test]
fn parse_reports_syntax_errors_and_fails() {
    let dir = TempDir::new().unwrap();
    let good = write_source(&dir, "good.go", "package p\n");
    let bad = write_source(&dir, "bad.gopp", "package p\n\nfunc f() { g(1 2) }\n");
    let output = run(gopp().arg("parse").arg(&good).arg(&bad));
    let header = format!(
        "{}:3:16: error[E0028]: syntax error: unexpected literal 2 in argument list; possibly \
         missing comma or )\n",
        bad.display()
    );
    assert!(stderr(&output).starts_with(&header), "{}", stderr(&output));
    assert!(stderr(&output).contains("func f() { g(1 2) }"));
    assert_eq!(output.status.code(), Some(1));
}

#[test]
fn parse_stops_reporting_after_ten_errors() {
    let dir = TempDir::new().unwrap();
    let bad = write_source(
        &dir,
        "bad.gopp",
        &format!("package p\n{}", "func f() { g(1 2) }\n".repeat(30)),
    );
    let output = run(gopp().arg("parse").arg(&bad));
    assert_eq!(stderr(&output).matches("error[E0028]").count(), 10);
    assert_eq!(
        stderr(&output)
            .matches("error[E0001]: too many errors")
            .count(),
        1
    );
}

#[test]
fn parse_refuses_files_it_cannot_read() {
    let dir = TempDir::new().unwrap();
    let other = write_source(&dir, "main.c", "int main;\n");
    let output = run(gopp().arg("parse").arg(&other));
    assert!(stderr(&output).ends_with("main.c: not a .gopp or .go file\n"));
    assert_eq!(output.status.code(), Some(1));

    let missing = dir.path().join("missing.gopp");
    let output = run(gopp().arg("parse").arg(&missing));
    assert!(stderr(&output).starts_with("error: "));
    assert!(stderr(&output).contains("missing.gopp: "));
    assert_eq!(output.status.code(), Some(1));

    let none = run(gopp().arg("parse"));
    assert_eq!(none.status.code(), Some(2));
}
