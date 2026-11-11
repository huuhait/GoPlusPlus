#![allow(clippy::unwrap_used, reason = "test helpers outside #[test] functions")]

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use tempfile::TempDir;

fn gopp() -> Command {
    Command::new(env!("CARGO_BIN_EXE_gopp"))
}

fn stdout(output: &Output) -> String {
    String::from_utf8_lossy(&output.stdout).into_owned()
}

fn stderr(output: &Output) -> String {
    String::from_utf8_lossy(&output.stderr).into_owned()
}

fn script(dir: &Path, name: &str, body: &str) -> PathBuf {
    let path = dir.join(name);
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
    gopp()
        .arg("host-toolchain")
        .env("GOPP_CLANG", clang)
        .output()
        .unwrap()
}

#[test]
fn version_names_the_compiler_and_the_pinned_clang() {
    let output = gopp().arg("version").output().unwrap();
    assert_eq!(
        stdout(&output),
        format!("gopp version {} (clang 23)\n", env!("CARGO_PKG_VERSION"))
    );
    assert_eq!(output.status.code(), Some(0));
}

#[test]
fn unknown_command_is_a_usage_error() {
    let output = gopp().arg("no-such-command").output().unwrap();
    assert_eq!(output.status.code(), Some(2));
}

#[test]
fn long_flags_take_one_dash() {
    let output = gopp().arg("-help").output().unwrap();
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
