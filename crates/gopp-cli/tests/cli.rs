#![allow(clippy::unwrap_used, reason = "test helpers outside #[test] functions")]

use std::fs;
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

/// A program that prints the line that `replies` has for its first
/// argument, and `otherwise` for any other: a shell script, or a batch file
/// on Windows.
fn fake_tool(dir: &Path, name: &str, replies: &[(&str, &str)], otherwise: &str) -> PathBuf {
    let (path, text) = if cfg!(windows) {
        let mut text = "@echo off\r\n".to_owned();
        for (at, (argument, _)) in replies.iter().enumerate() {
            text += &format!("if \"%~1\"==\"{argument}\" goto reply{at}\r\n");
        }
        text += &format!("echo {otherwise}\r\nexit /b 0\r\n");
        for (at, (_, line)) in replies.iter().enumerate() {
            text += &format!(":reply{at}\r\necho {line}\r\nexit /b 0\r\n");
        }
        (dir.join(format!("{name}.cmd")), text)
    } else {
        let mut text = "#!/bin/sh\ncase $1 in\n".to_owned();
        for (argument, line) in replies {
            text += &format!("{argument}) echo '{line}';;\n");
        }
        text += &format!("*) echo '{otherwise}';;\nesac\n");
        (dir.join(name), text)
    };
    let _guard = SPAWN.lock().unwrap_or_else(PoisonError::into_inner);
    fs::write(&path, text).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
    }
    path
}

/// A `clang` that reports `clang_version` and points at an `ld.lld` reporting
/// `lld_version`: the two programs.
fn fake_toolchain(dir: &TempDir, clang_version: &str, lld_version: &str) -> (PathBuf, PathBuf) {
    let version = format!("LLD {lld_version} (compatible with GNU linkers)");
    let lld = fake_tool(dir.path(), "ld.lld", &[], &version);
    let replies = [
        ("--version", format!("clang version {clang_version}")),
        ("-print-prog-name=ld.lld", lld.display().to_string()),
    ];
    let replies = replies
        .each_ref()
        .map(|(argument, line)| (*argument, line.as_str()));
    (fake_tool(dir.path(), "clang", &replies, ""), lld)
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
fn targets_lists_what_gopp_builds_for() {
    let output = run(gopp().arg("targets"));
    let listed = stdout(&output);
    let lines: Vec<&str> = listed.lines().collect();
    assert_eq!(lines.len(), 9, "{listed}");
    assert!(lines[0].starts_with("TARGET") && lines[0].ends_with("VERSION 1"));
    let supported: Vec<&str> = lines
        .iter()
        .filter(|line| line.ends_with("  supported"))
        .filter_map(|line| line.split_whitespace().next())
        .collect();
    assert_eq!(supported, ["linux/amd64", "windows/amd64"]);
    assert_eq!(stderr(&output), "");
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
fn explain_says_what_a_code_means_and_lists_them_all() {
    let output = run(gopp().args(["explain", "E0111"]));
    assert_eq!(output.status.code(), Some(0));
    let text = stdout(&output);
    assert!(
        text.starts_with("E0111 err_undefined\n\nA name is used"),
        "{text}"
    );
    assert!(
        text.contains("\nFix: ") && text.contains("\nExample:\n\n    "),
        "{text}"
    );
    assert_eq!(
        stdout(&run(gopp().args(["explain", "err_undefined"]))),
        text
    );

    let output = run(gopp().args(["explain", "-list"]));
    assert_eq!(output.status.code(), Some(0));
    let listed = stdout(&output);
    assert!(listed.starts_with("E0001 err_too_many_errors\n"));
    assert!(listed.contains("\nE0111 err_undefined\n"));

    let output = run(gopp().args(["explain", "E9999"]));
    assert_eq!(output.status.code(), Some(1));
    assert_eq!(
        stderr(&output),
        "error: E9999 is no error code; gopp explain --list lists them\n"
    );
    assert_eq!(run(gopp().arg("explain")).status.code(), Some(2));
}

#[test]
fn host_toolchain_prints_the_pinned_tools() {
    let dir = TempDir::new().unwrap();
    let (clang, lld) = fake_toolchain(&dir, "23.1.0", "23.1.0");
    let output = host_toolchain(&clang);
    assert_eq!(
        stdout(&output),
        format!(
            "clang: {} (23.1.0)\nld.lld: {} (23.1.0)\n",
            clang.display(),
            lld.display()
        )
    );
    assert_eq!(output.status.code(), Some(0));
}

#[test]
fn host_toolchain_rejects_another_clang_major() {
    let dir = TempDir::new().unwrap();
    let (clang, _) = fake_toolchain(&dir, "22.1.8", "23.1.0");
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
    let (clang, _) = fake_toolchain(&dir, "23.1.0", "21.1.8");
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
    let replies = [
        ("--version", "clang version 23.1.0"),
        ("-print-prog-name=ld.lld", "ld.lld"),
    ];
    let clang = fake_tool(dir.path(), "clang", &replies, "");
    let output = host_toolchain(&clang);
    assert!(stderr(&output).starts_with("error: ld.lld not found: "));
    assert_eq!(output.status.code(), Some(1));
}

fn write_source(dir: &TempDir, name: &str, text: &str) -> PathBuf {
    // The path as the system writes it, whatever separates its parts.
    let path: PathBuf = [dir.path()]
        .into_iter()
        .chain(name.split('/').map(Path::new))
        .collect();
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

/// A package in a directory of its own.
fn write_package(dir: &TempDir, name: &str, files: &[(&str, &str)]) -> PathBuf {
    let package = dir.path().join(name);
    fs::create_dir(&package).unwrap();
    for (file, text) in files {
        fs::write(package.join(file), text).unwrap();
    }
    package
}

#[test]
fn check_reports_what_is_wrong_as_text_or_json() {
    let dir = TempDir::new().unwrap();
    let good = write_source(
        &dir,
        "good.gopp",
        "package p\n\nfunc f() int { return 1 }\n",
    );
    let output = run(gopp().arg("check").arg(&good));
    assert_eq!(
        (stdout(&output), stderr(&output)),
        (String::new(), String::new())
    );
    assert_eq!(output.status.code(), Some(0));

    let bad = write_source(
        &dir,
        "bad.gopp",
        "package p\n\nfunc f() int { return \"one\" }\n",
    );
    let output = run(gopp().arg("check").arg(&bad));
    let header = format!(
        "{}:3:23: error[E0206]: cannot use \"one\" (untyped string constant) as int value in \
         return statement\n",
        bad.display()
    );
    assert!(stderr(&output).starts_with(&header), "{}", stderr(&output));
    assert_eq!(stdout(&output), "");
    assert_eq!(output.status.code(), Some(1));

    let output = run(gopp().args(["check", "-json"]).arg(&bad));
    let json = stdout(&output);
    assert_eq!(json.lines().count(), 1, "{json}");
    assert!(
        json.starts_with('{') && json.contains(r#""code":"E0206""#),
        "{json}"
    );
    assert_eq!(stderr(&output), "");
    assert_eq!(output.status.code(), Some(1));

    let missing = dir.path().join("none.gopp");
    let output = run(gopp().arg("check").arg(&missing));
    assert!(
        stderr(&output).starts_with("error: "),
        "{}",
        stderr(&output)
    );
    assert_eq!(output.status.code(), Some(1));
}

#[test]
fn check_takes_a_directory_as_one_package_and_prints_what_it_found() {
    let dir = TempDir::new().unwrap();
    let files = [
        (
            "a.gopp",
            "package shapes\n\nimport \"example.com/units\"\n\ntype Vec struct{ X, Y int }\n\n@Operator\nfunc (a Vec) Add(b Vec) Vec { return Vec{a.X + b.X, a.Y + b.Y} }\n\nconst Origin = Vec{0, 0}\n\nvar scale = units.Scale\n",
        ),
        (
            "b.gopp",
            "package shapes\n\nfunc Sum(a, b Vec) int {\n\tc := a + b\n\treturn c.X + Origin.Y\n}\n",
        ),
        (
            "a_test.go",
            "package shapes\n\nfunc broken() { undefined() }\n",
        ),
    ];
    let package = write_package(&dir, "shapes", &files);
    let output = run(gopp().args(["check", "-emit=info"]).arg(&package));
    assert_eq!(stderr(&output), "");
    assert_eq!(output.status.code(), Some(0));
    let found = stdout(&output).replace(&package.display().to_string(), "shapes");
    let found = found.replace('\\', "/");
    let expected = "package shapes (shapes)
type Vec shapes.Vec
const Origin shapes.Vec = {0, 0}
var scale invalid type
func Sum func(shapes.Vec, shapes.Vec) int
file shapes/a.gopp
  8:38 Vec{a.X + b.X, a.Y + b.Y}: type shapes.Vec
  8:42 a: type shapes.Vec
  8:42 a.X: selection Field of shapes.Vec in shapes.Vec by [0]
  8:42 a.X: type int
  8:42 a.X + b.X: type int
  8:48 b: type shapes.Vec
  8:48 b.X: selection Field of shapes.Vec in shapes.Vec by [0]
  8:48 b.X: type int
  8:53 a: type shapes.Vec
  8:53 a.Y: selection Field of shapes.Vec in shapes.Vec by [1]
  8:53 a.Y: type int
  8:53 a.Y + b.Y: type int
  8:59 b: type shapes.Vec
  8:59 b.Y: selection Field of shapes.Vec in shapes.Vec by [1]
  8:59 b.Y: type int
file shapes/b.gopp
  4:7 a: type shapes.Vec
  4:7 a + b: operator shapes.Vec.Add func(shapes.Vec) shapes.Vec
  4:7 a + b: type shapes.Vec
  4:11 b: type shapes.Vec
  5:9 c: type shapes.Vec
  5:9 c.X: selection Field of shapes.Vec in shapes.Vec by [0]
  5:9 c.X: type int
  5:9 c.X + Origin.Y: type int
  5:15 Origin: type shapes.Vec = {0, 0}
  5:15 Origin.Y: selection Field of shapes.Vec in shapes.Vec by [1]
  5:15 Origin.Y: type int = 0
";
    assert_eq!(found, expected);
}

#[test]
fn check_prints_the_same_with_one_thread_and_with_many() {
    let dir = TempDir::new().unwrap();
    let mut packages = Vec::new();
    for at in 0..24 {
        let text = format!(
            "package p{at}\n\ntype T{at} struct{{ n int }}\n\n@Operator\nfunc (a T{at}) Add(b T{at}) \
             T{at} {{ return a }}\n\nfunc use(a T{at}, xs []int) int {{\n\tb := a + a\n\tvar wrong \
             string = {at}\n\tfor _, x := range xs {{\n\t\tb.n += x\n\t}}\n\treturn b.n + missing{at}\n}}\n"
        );
        packages.push(write_package(
            &dir,
            &format!("p{at}"),
            &[("main.gopp", &text)],
        ));
    }
    let checked = |threads: &str, json: bool| {
        let mut command = gopp();
        command.arg("check").arg("-emit=info");
        if json {
            command.arg("-json");
        }
        let output = run(command.args(&packages).env("RAYON_NUM_THREADS", threads));
        assert_eq!(output.status.code(), Some(1));
        (stdout(&output), stderr(&output))
    };
    for json in [false, true] {
        let one = checked("1", json);
        assert!(
            one.0.contains("package p23") && one.0.contains("operator"),
            "{}",
            one.0
        );
        for threads in ["2", "8", "32"] {
            assert_eq!(checked(threads, json), one, "{threads} threads");
        }
    }
}

/// The samples that need no header and no macro, each in Go and in Go++.
#[test]
#[ignore = "long: checks the packages of Go's standard library that the samples import"]
fn check_accepts_the_samples_with_go_s_standard_library() {
    let wanted = ["01-", "02-", "03-", "04-", "05-", "09-", "10-"];
    let files: Vec<PathBuf> = samples::files()
        .into_iter()
        .filter(|path| {
            let name = path.to_string_lossy();
            wanted.iter().any(|sample| name.contains(sample))
        })
        .collect();
    assert_eq!(files.len(), 14);
    let checked = |file: &PathBuf, threads: &str| {
        let mut command = gopp();
        command.args(["check", "-emit=info"]).arg(file);
        let output = run(command.env("RAYON_NUM_THREADS", threads));
        assert_eq!(stderr(&output), "", "{}", file.display());
        assert_eq!(output.status.code(), Some(0));
        stdout(&output)
    };
    for file in &files {
        assert_eq!(checked(file, "1"), checked(file, "8"), "{}", file.display());
    }
}

const SUMS: &str = "package main

var total int

func add(a, b int) int { return a + b }

func main() {
	total = add(1, 2)
}
";

#[test]
fn demangle_reads_symbols_as_go_plus_plus() {
    let names = ["_GPF4mainE3Foo", "_GPTo7opr_DivN4mainE4Vec2LdE"];
    let output = run(gopp().arg("demangle").args(names));
    assert_eq!(output.status.code(), Some(0));
    assert_eq!(stdout(&output), "main.Foo\nmain.Vec2.Div(float64) @Right\n");
    // What is no symbol is said with where it stops being one; the
    // others are still printed.
    let output = run(gopp().args(["demangle", "_GPF4mainE03Foo", "printf", "_GPJ4mainE"]));
    assert_eq!(output.status.code(), Some(1));
    assert_eq!(stdout(&output), "main.init\n");
    assert_eq!(
        stderr(&output),
        "error: _GPF4mainE03Foo: a number has a leading zero at offset 10\n\
         error: printf: no Go++ symbol: it does not start with _GP at offset 0\n"
    );
    let output = run(gopp().args(["demangle", "-max-output=6", "_GPF4mainE3Foo"]));
    assert_eq!(stdout(&output), "main.F\n");
}

#[test]
fn demangle_filters_standard_input() {
    let dir = TempDir::new().unwrap();
    let trace = dir.path().join("trace");
    let text = "#0 0x401000 in _GPM3LenN4mainE4Vec2 (v=...)\n#1 _GPF4mainE4main+0x1c\n#2 _Z3foov\n";
    fs::write(&trace, text).unwrap();
    let output = run(gopp()
        .arg("demangle")
        .stdin(fs::File::open(&trace).unwrap()));
    assert_eq!(output.status.code(), Some(0));
    assert_eq!(
        stdout(&output),
        "#0 0x401000 in main.Vec2.Len (v=...)\n#1 main.main+0x1c\n#2 _Z3foov\n"
    );
}

#[test]
fn build_prints_the_module_and_the_unit_of_c() {
    let dir = TempDir::new().unwrap();
    let file = write_source(&dir, "sums.gopp", SUMS);
    let built = |flags: &[&str]| {
        let output = run(gopp().arg("build").args(flags).arg(&file));
        assert_eq!(stderr(&output), "");
        assert_eq!(output.status.code(), Some(0));
        stdout(&output)
    };
    let gir = built(&["-emit=gir"]);
    assert!(gir.contains("fn _GPF28command$2d$line$2d$argumentsE3add(_1: i64, _2: i64) -> i64"));
    assert!(gir.contains("global var @_GPR28command$2d$line$2d$argumentsE5total: i64"));
    for target in ["linux/amd64", "windows/amd64"] {
        let unit = built(&["-emit=c", &format!("-target={target}")]);
        assert!(unit.contains("__asm__(\"_GPF28command$2d$line$2d$argumentsE3add\")"));
        assert!(unit.contains("int main(int argc, char **argv) {"));
        assert_eq!(unit, built(&["-emit=c"]));
    }
    // With `-o` the text goes to the file.
    let out = dir.path().join("sums.c");
    assert_eq!(built(&["-emit=c", "-o", out.to_str().unwrap()]), "");
    assert_eq!(fs::read_to_string(out).unwrap(), built(&["-emit=c"]));
}

#[test]
fn build_says_what_stops_it() {
    let dir = TempDir::new().unwrap();
    let good = write_source(&dir, "sums.gopp", SUMS);
    let failed = |command: &mut Command| {
        let output = run(command);
        assert_eq!(output.status.code(), Some(1));
        assert_eq!(stdout(&output), "");
        stderr(&output)
    };
    let other = failed(
        gopp()
            .args(["build", "-target=linux/arm64", "-emit=c"])
            .arg(&good),
    );
    assert!(
        other.contains("linux/arm64 is not supported in this version"),
        "{other}"
    );
    let none = failed(
        gopp()
            .args(["build", "-target=plan9/mips", "-emit=c"])
            .arg(&good),
    );
    assert!(none.starts_with("error: "), "{none}");
    let wrong = write_source(
        &dir,
        "wrong.gopp",
        "package main\n\nfunc main() { missing() }\n",
    );
    let unchecked = failed(gopp().args(["build", "-emit=c"]).arg(&wrong));
    assert!(unchecked.contains("undefined: missing"), "{unchecked}");
    let ranges = "package main\n\nfunc main() {\n\tfor range 3 {\n\t}\n}\n";
    let later = write_source(&dir, "later.gopp", ranges);
    let unsupported = failed(gopp().args(["build", "-emit=c"]).arg(&later));
    assert!(unsupported.contains("error[E0439]"), "{unsupported}");
    assert!(
        unsupported.contains("not supported yet: a range loop"),
        "{unsupported}"
    );
    fs::create_dir(dir.path().join("p")).unwrap();
    write_source(&dir, "p/p.gopp", "package p\n");
    let two = failed(
        gopp()
            .args(["build", "-emit=c"])
            .arg(&good)
            .arg(dir.path().join("p")),
    );
    assert!(two.contains("gopp build takes one package"), "{two}");
}

#[test]
#[cfg_attr(
    windows,
    ignore = "needs Clang 23, which the Windows test machine does not have yet"
)]
fn build_makes_an_object_for_each_target_and_links_for_its_own() {
    let dir = TempDir::new().unwrap();
    let file = write_source(&dir, "sums.gopp", SUMS);
    let build = |flags: &[&str]| {
        let mut command = gopp();
        command.arg("build").args(flags).arg(&file);
        run(command.current_dir(dir.path()))
    };
    let made = |flags: &[&str], name: &str| {
        let output = build(flags);
        assert_eq!(stderr(&output), "");
        assert_eq!(output.status.code(), Some(0));
        fs::read(dir.path().join(name)).unwrap()
    };
    let elf = made(&["-emit=obj", "-target=linux/amd64"], "sums.o");
    assert_eq!(&elf[..4], b"\x7fELF");
    let coff = made(&["-emit=obj", "-target=windows/amd64"], "sums.obj");
    assert_eq!(&coff[..2], [0x64, 0x86]);
    let assembly = made(&["-emit=asm", "-target=linux/amd64"], "sums.s");
    assert!(String::from_utf8_lossy(&assembly).contains("_GPF28command$2d$line$2d$argumentsE3add"));
    let (own, other, program) = match cfg!(target_os = "linux") {
        true => ("linux/amd64", "windows/amd64", "sums"),
        false => ("windows/amd64", "linux/amd64", "sums.exe"),
    };
    let refused = build(&[&format!("-target={other}")]);
    assert_eq!(refused.status.code(), Some(1));
    assert!(stderr(&refused).starts_with("error[E0441]: cannot link for "));
    if cfg!(target_os = "linux") {
        made(&[&format!("-target={own}")], program);
        let ran = run(&mut Command::new(dir.path().join(program)));
        assert_eq!(ran.status.code(), Some(0));
    }
}
