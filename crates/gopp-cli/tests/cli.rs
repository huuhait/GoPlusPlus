#![allow(clippy::unwrap_used, reason = "test helpers outside #[test] functions")]

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::{Mutex, PoisonError};

use gopp_testutil::samples;
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

    let output = run(gopp().args(["parse", "-emit=json"]).arg(&file).arg(&file));
    let json = stdout(&output);
    let start = r#"{"kind":"File","text":"package=main","children":[{"kind":"Func","text":"main","span":[14,36],"#;
    assert!(json.starts_with(start), "{json}");
    assert!(json.contains(r#"{"kind":"Call","text":"unwrap","#));
    assert_eq!(json.lines().count(), 2);
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

/// The records of `gopp parse -batch`: exit status, standard output,
/// standard error and path of each file.
fn batch_records(mut output: &[u8]) -> Vec<(i32, String, String, String)> {
    let mut records = Vec::new();
    while !output.is_empty() {
        let end = output.iter().position(|&byte| byte == b'\n').unwrap();
        let header = str::from_utf8(&output[..end]).unwrap();
        let mut fields = header.strip_prefix("gopp-batch ").unwrap().splitn(4, ' ');
        let mut number = || fields.next().unwrap().parse::<usize>().unwrap();
        let (exit, stdout_len, stderr_len) = (number(), number(), number());
        let path = fields.next().unwrap().to_owned();
        let (stdout, rest) = output[end + 1..].split_at(stdout_len);
        let (stderr, rest) = rest.split_at(stderr_len);
        let text = |bytes: &[u8]| String::from_utf8(bytes.to_vec()).unwrap();
        records.push((exit as i32, text(stdout), text(stderr), path));
        output = rest;
    }
    records
}

#[test]
fn parse_batch_gives_each_file_what_parse_gives_it() {
    let dir = TempDir::new().unwrap();
    let mut files = samples::files();
    files.push(write_source(
        &dir,
        "bad name.gopp",
        "package p\n\nfunc f() { g(1 2) }\n",
    ));
    files.push(dir.path().join("missing.gopp"));
    files.push(write_source(&dir, "notes.txt", "x\n"));
    let list: String = files
        .iter()
        .map(|file| format!("{}\n\n", file.display()))
        .collect();
    let list = write_source(&dir, "list", &list);

    let stdin = fs::File::open(list).unwrap();
    let batch = run(gopp().args(["parse", "-batch", "-emit=ast"]).stdin(stdin));
    assert_eq!(stderr(&batch), "");
    assert_eq!(batch.status.code(), Some(1));
    let records = batch_records(&batch.stdout);
    assert_eq!(records.len(), files.len());
    for (file, record) in files.iter().zip(records) {
        let single = run(gopp().args(["parse", "-emit=ast"]).arg(file));
        let expected = (
            single.status.code().unwrap(),
            stdout(&single),
            stderr(&single),
            file.display().to_string(),
        );
        assert_eq!(record, expected);
    }
}

#[test]
fn parse_batch_of_good_files_succeeds_and_takes_no_file_arguments() {
    let dir = TempDir::new().unwrap();
    let good = write_source(&dir, "good.go", "package p\n");
    let list = write_source(&dir, "list", &format!("{}\n", good.display()));
    let batch = run(gopp()
        .args(["parse", "-batch"])
        .stdin(fs::File::open(&list).unwrap()));
    assert_eq!(
        stdout(&batch),
        format!("gopp-batch 0 0 0 {}\n", good.display())
    );
    assert_eq!(batch.status.code(), Some(0));

    let both = run(gopp().args(["parse", "-batch"]).arg(&good));
    assert_eq!(both.status.code(), Some(2));
}

const UNFORMATTED: &str = "package p\n\nfunc f( ) {x:=load!(1,2)}\n";
const FORMATTED: &str = "package p\n\nfunc f() { x := load!(1, 2) }\n";

#[test]
fn fmt_prints_the_canonical_layout() {
    let dir = TempDir::new().unwrap();
    let file = write_source(&dir, "main.gopp", UNFORMATTED);
    let output = run(gopp().arg("fmt").arg(&file));
    assert_eq!(stdout(&output), FORMATTED);
    assert_eq!(output.status.code(), Some(0));
    assert_eq!(fs::read_to_string(&file).unwrap(), UNFORMATTED);
}

#[test]
fn fmt_lists_and_rewrites_only_files_that_change() {
    let dir = TempDir::new().unwrap();
    fs::create_dir(dir.path().join("sub")).unwrap();
    let changed = write_source(&dir, "sub/main.gopp", UNFORMATTED);
    let same = write_source(&dir, "same.gopp", FORMATTED);
    let plain_go = write_source(&dir, "sub/a.go", "package p\nvar  macro = 1\n");
    write_source(&dir, "sub/.hidden.go", "package  p\n");
    write_source(&dir, "notes.txt", "x  y\n");

    let output = run(gopp().args(["fmt", "-l"]).arg(dir.path()));
    let listed = format!("{}\n{}\n", plain_go.display(), changed.display());
    assert_eq!(
        (stdout(&output), stderr(&output)),
        (listed.clone(), String::new())
    );
    assert_eq!(output.status.code(), Some(0));
    assert_eq!(fs::read_to_string(&changed).unwrap(), UNFORMATTED);

    let before = fs::metadata(&same).unwrap().modified().unwrap();
    let output = run(gopp().args(["fmt", "-l", "-w"]).arg(dir.path()));
    assert_eq!(stdout(&output), listed);
    assert_eq!(fs::read_to_string(&changed).unwrap(), FORMATTED);
    assert_eq!(
        fs::read_to_string(&plain_go).unwrap(),
        "package p\n\nvar macro = 1\n"
    );
    assert_eq!(fs::metadata(&same).unwrap().modified().unwrap(), before);
    assert_eq!(
        fs::read_to_string(dir.path().join("sub/.hidden.go")).unwrap(),
        "package  p\n"
    );

    let output = run(gopp().args(["fmt", "-l"]).arg(dir.path()));
    assert_eq!(stdout(&output), "");
}

#[test]
fn fmt_prints_a_diff() {
    let dir = TempDir::new().unwrap();
    let file = write_source(&dir, "main.gopp", UNFORMATTED);
    let output = run(gopp().args(["fmt", "-d"]).arg(&file));
    let name = file.display();
    let diff = format!(
        "diff {name}.orig {name}\n--- {name}.orig\n+++ {name}\n@@ -1,3 +1,3 @@\n package p\n \n\
         -func f( ) {{x:=load!(1,2)}}\n+func f() {{ x := load!(1, 2) }}\n"
    );
    assert_eq!(stdout(&output), diff);
    assert_eq!(output.status.code(), Some(0));
    assert_eq!(fs::read_to_string(&file).unwrap(), UNFORMATTED);

    let formatted = write_source(&dir, "done.gopp", FORMATTED);
    let output = run(gopp().args(["fmt", "-d"]).arg(&formatted));
    assert_eq!(stdout(&output), "");
}

fn fmt_stdin(flags: &[&str], input: &str) -> Output {
    use std::io::Write;
    use std::process::Stdio;

    let mut command = gopp();
    command.arg("fmt").args(flags);
    command.stdin(Stdio::piped()).stdout(Stdio::piped());
    let mut child = {
        let _guard = SPAWN.lock().unwrap_or_else(PoisonError::into_inner);
        command.stderr(Stdio::piped()).spawn().unwrap()
    };
    let mut stdin = child.stdin.take().unwrap();
    stdin.write_all(input.as_bytes()).unwrap();
    drop(stdin);
    child.wait_with_output().unwrap()
}

#[test]
fn fmt_formats_standard_input_as_gopp_unless_told_it_is_go() {
    let output = fmt_stdin(&[], UNFORMATTED);
    assert_eq!(stdout(&output), FORMATTED);
    assert_eq!(output.status.code(), Some(0));

    let listed = fmt_stdin(&["-l"], UNFORMATTED);
    assert_eq!(stdout(&listed), "<standard input>\n");

    // `macro` is a name in Go, and `f!(x)` is not Go.
    let go = fmt_stdin(&["-go"], "package p\nvar  macro = 1\n");
    assert_eq!(stdout(&go), "package p\n\nvar macro = 1\n");
    assert_eq!(fmt_stdin(&["-go"], UNFORMATTED).status.code(), Some(1));
    assert_eq!(
        fmt_stdin(&[], "package p\nvar  macro = 1\n").status.code(),
        Some(1)
    );

    let refused = fmt_stdin(&["-w"], UNFORMATTED);
    assert_eq!(
        stderr(&refused),
        "error: cannot use -w with standard input\n"
    );
    assert_eq!(refused.status.code(), Some(1));
}

#[test]
fn fmt_leaves_a_file_with_a_syntax_error_alone() {
    let dir = TempDir::new().unwrap();
    let source = "package p\n\nfunc f() { g(1 2) }\n";
    let bad = write_source(&dir, "bad.gopp", source);
    let good = write_source(&dir, "good.gopp", UNFORMATTED);
    let parsed = run(gopp().arg("parse").arg(&bad));
    for flags in [&["-w"][..], &["-l"], &["-d"], &[]] {
        let output = run(gopp().arg("fmt").args(flags).arg(&bad));
        assert_eq!(stderr(&output), stderr(&parsed));
        assert_eq!(stdout(&output), "");
        assert_eq!(output.status.code(), Some(1));
        assert_eq!(fs::read_to_string(&bad).unwrap(), source);
    }
    // The files without an error are still formatted.
    let output = run(gopp().args(["fmt", "-w"]).arg(dir.path()));
    assert_eq!(output.status.code(), Some(1));
    assert_eq!(fs::read_to_string(&good).unwrap(), FORMATTED);

    let missing = run(gopp().arg("fmt").arg(dir.path().join("none.gopp")));
    assert!(
        stderr(&missing).starts_with("error: "),
        "{}",
        stderr(&missing)
    );
    assert_eq!(missing.status.code(), Some(1));
}

#[test]
fn fmt_finds_every_sample_formatted() {
    let examples = samples::files()[0]
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_owned();
    let output = run(gopp().args(["fmt", "-l"]).arg(&examples));
    assert_eq!(
        (stdout(&output), stderr(&output)),
        (String::new(), String::new())
    );
    assert_eq!(output.status.code(), Some(0));
}
