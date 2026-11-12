//! The fast test runner: each way that a program can end, what becomes
//! of the process that ran it, and that the order of the programs
//! changes nothing (test-runner.md).

#![cfg(all(target_os = "linux", target_arch = "x86_64"))]
#![allow(clippy::unwrap_used, reason = "test helpers outside #[test] functions")]

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use serde_json::{Value, json};
use tempfile::TempDir;

fn decoded(base64: &str) -> String {
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
    String::from_utf8_lossy(&bytes).into_owned()
}

/// How one job ended, as its line says.
#[derive(Clone, Debug, PartialEq)]
struct Ended {
    outcome: String,
    exit: i64,
    signal: i64,
    stdout: String,
    stderr: String,
    message: String,
    restarted: bool,
}

/// A run of `gopp test-run` over programs that the test writes.
struct Suite {
    dir: TempDir,
    jobs: Vec<Value>,
}

impl Suite {
    fn new() -> Self {
        Self {
            dir: TempDir::new().unwrap(),
            jobs: Vec::new(),
        }
    }

    /// A job of one file with the text; `more` are other fields of it.
    fn job(&mut self, id: &str, file: &str, text: &str, more: Value) -> &mut Self {
        let path = self.dir.path().join(format!("{}-{file}", self.jobs.len()));
        fs::write(&path, text).unwrap();
        let mut job = json!({"v": 1, "id": id, "inputs": [path]});
        for (name, value) in more.as_object().unwrap() {
            job[name] = value.clone();
        }
        self.jobs.push(job);
        self
    }

    fn c(&mut self, id: &str, text: &str) -> &mut Self {
        self.job(id, "job.c", text, json!({}))
    }

    fn gopp(&mut self, id: &str, text: &str) -> &mut Self {
        self.job(id, "job.gopp", text, json!({}))
    }

    fn command(&self, flags: &[&str]) -> Command {
        let manifest: String = self.jobs.iter().map(|job| format!("{job}\n")).collect();
        let jobs = self.dir.path().join("jobs");
        fs::write(&jobs, manifest).unwrap();
        let mut command = Command::new(env!("CARGO_BIN_EXE_gopp"));
        command
            .args(["test-run", "-manifest"])
            .arg(jobs)
            .args(flags);
        let cache = Path::new(env!("CARGO_TARGET_TMPDIR")).join("gopp-cache");
        command.env("GOPPCACHE", cache).env_remove("GOPP_FAST_RUN");
        command
    }

    /// The results by the names of the jobs, with what the runner said
    /// and its status.
    fn run(&self, flags: &[&str]) -> (BTreeMap<String, Ended>, Output) {
        let output = self.command(flags).output().unwrap();
        (results(&String::from_utf8_lossy(&output.stdout)), output)
    }

    /// The results of a run that itself did not fail.
    fn ran(&self, flags: &[&str]) -> BTreeMap<String, Ended> {
        let (results, output) = self.run(flags);
        let said = String::from_utf8_lossy(&output.stderr);
        assert_eq!(output.status.code(), Some(0), "{said}");
        assert_eq!(results.len(), self.jobs.len(), "{said}");
        results
    }
}

fn results(lines: &str) -> BTreeMap<String, Ended> {
    let ended = |line: &str| {
        let result: Value = serde_json::from_str(line).unwrap();
        assert_eq!(result["v"], 1);
        let text = |name: &str| result[name].as_str().unwrap().to_owned();
        let ended = Ended {
            outcome: text("outcome"),
            exit: result["exit"].as_i64().unwrap(),
            signal: result["signal"].as_i64().unwrap(),
            stdout: decoded(&text("stdout")),
            stderr: decoded(&text("stderr")),
            message: text("message"),
            restarted: result["restarted"].as_bool().unwrap(),
        };
        assert!(result["compile_ms"].as_f64().unwrap() >= 0.0);
        assert!(result["run_ms"].as_f64().unwrap() >= 0.0);
        (text("id"), ended)
    };
    lines.lines().map(ended).collect()
}

/// What a program of C needs to ask the system without a function of
/// the C library: a module that names none leaves its process as it
/// found it, as far as the runner can tell.
const SYSTEM: &str = "
static long ask(long number, long a, long b, long c) {
  long result;
  __asm__ volatile(\"syscall\" : \"=a\"(result) : \"a\"(number), \"D\"(a), \"S\"(b), \"d\"(c)
                   : \"rcx\", \"r11\", \"memory\");
  return result;
}
enum { WRITE = 1, GETPID = 39, FCNTL = 72, GETCWD = 79, CHDIR = 80, UMASK = 95, DUP = 32 };
static void say(const char *text) {
  long count = 0;
  while (text[count]) count++;
  ask(WRITE, 1, (long)text, count);
}
static void number(long value) {
  char digits[24];
  int at = sizeof digits;
  digits[--at] = '\\n';
  do { digits[--at] = (char)('0' + value % 10); value /= 10; } while (value > 0);
  ask(WRITE, 1, (long)(digits + at), (long)(sizeof digits - at));
}
";

/// Says which process runs it.
fn says_pid() -> String {
    format!("{SYSTEM}\nint gopp_entry(void) {{ number(ask(GETPID, 0, 0, 0)); return 0; }}\n")
}

const HELLO: &str = "package main\n\nfunc main() { println(\"hello\") }\n";
const PANICS: &str = "package main\n\nfunc zero() int { return 0 }\n\n\
                      func main() { println(\"before\"); println(1 / zero()) }\n";
const OVERFLOWS: &str = "package main\n\nfunc down(n int) int { return down(n+1) + 1 }\n\n\
                         func main() { println(down(0)) }\n";
const SPINS: &str = "package main\n\nfunc main() {\n\tfor {\n\t}\n}\n";
const FLOODS: &str =
    "package main\n\nfunc main() {\n\tfor {\n\t\tprintln(\"more and more\")\n\t}\n}\n";

#[test]
fn each_way_that_a_program_ends_has_its_outcome() {
    let mut suite = Suite::new();
    suite
        .gopp("returns", HELLO)
        .gopp("panics", PANICS)
        .gopp("overflows", OVERFLOWS)
        .gopp("floods", FLOODS)
        .gopp("no-program", "package other\n\nfunc F() {}\n")
        .gopp(
            "wrong",
            "package main\n\nfunc main() { var x int = \"a\" }\n",
        )
        .c(
            "exits",
            "[[noreturn]] void exit(int);\nint gopp_entry(void) { exit(7); }\n",
        )
        .c(
            "aborts",
            "[[noreturn]] void abort(void);\nint gopp_entry(void) { abort(); }\n",
        )
        .c("status", "int gopp_entry(void) { return 3; }\n")
        .c("not-c", "int gopp_entry(void) { return }\n")
        .c(
            "unresolved",
            "void missing_b(void);\nvoid missing_a(void);\n\
             int gopp_entry(void) { missing_b(); missing_a(); return 0; }\n",
        )
        .c("no-entry", "int other(void) { return 0; }\n")
        .job("spins", "job.gopp", SPINS, json!({"timeout_ms": 150}))
        .job(
            "elsewhere",
            "job.gopp",
            HELLO,
            json!({"target": "windows/amd64"}),
        )
        .job("named", "job.gopp", HELLO, json!({"target": "linux/amd64"}));
    let results = suite.ran(&["-max-output", "65536"]);
    let of = |id: &str| &results[id];
    let ended = |id: &str| {
        let result = of(id);
        (
            &*result.outcome,
            result.exit,
            result.signal,
            result.restarted,
        )
    };
    // The process of a program that returns is kept; one that a program
    // ended, a signal ended or the runner killed is not.
    assert_eq!(ended("returns"), ("exited", 0, 0, false));
    assert_eq!(of("returns").stderr, "hello\n");
    assert_eq!(ended("named"), ("exited", 0, 0, false));
    assert_eq!(ended("status"), ("exited", 3, 0, false));
    assert_eq!(ended("panics"), ("exited", 2, 0, true));
    let said = &of("panics").stderr;
    assert!(said.starts_with("before\npanic: runtime error: integer divide by zero\n\n"));
    assert!(said.contains("\nmain.main()\n\t+0x"), "{said}");
    assert_eq!(ended("exits"), ("exited", 7, 0, true));
    assert_eq!(ended("overflows"), ("signaled", 0, 11, true));
    assert_eq!(ended("aborts"), ("signaled", 0, 6, true));
    // SIGXFSZ: more output than a program may write.
    assert_eq!(ended("floods"), ("signaled", 0, 25, true));
    assert_eq!(of("floods").stderr.len(), 65536);
    assert_eq!(ended("spins"), ("timeout", 0, 0, true));
    // What does not build or load did not run.
    assert_eq!(ended("wrong"), ("compile-failed", 0, 0, false));
    let message = &of("wrong").message;
    assert!(
        message.contains("error[E") && message.contains("job.gopp:3:"),
        "{message}"
    );
    assert_eq!(ended("not-c"), ("compile-failed", 0, 0, false));
    assert!(
        of("not-c")
            .message
            .starts_with("error[E0441]: clang failed:")
    );
    assert_eq!(
        of("no-program").message,
        "error: the package is not `main`: there is no program to run\n"
    );
    assert_eq!(
        of("elsewhere").message,
        "error: a program for windows/amd64 cannot run on this machine\n"
    );
    assert_eq!(ended("unresolved"), ("load-failed", 0, 0, false));
    assert_eq!(
        of("unresolved").message,
        "undefined symbols: missing_a missing_b"
    );
    assert_eq!(ended("no-entry"), ("load-failed", 0, 0, false));
    assert_eq!(of("no-entry").message, "the module has no `gopp_entry`");
}

#[test]
fn an_executor_is_kept_until_a_program_may_have_left_something_behind() {
    let mut suite = Suite::new();
    let changes = format!(
        "{SYSTEM}\nint gopp_entry(void) {{\n  number(ask(GETPID, 0, 0, 0));\n  \
         ask(CHDIR, (long)\"/\", 0, 0);\n  ask(UMASK, 0777, 0, 0);\n  \
         number(ask(DUP, 1, 0, 0));\n  return 0;\n}}\n"
    );
    let reports = format!(
        "{SYSTEM}\nint gopp_entry(void) {{\n  char dir[512];\n  \
         number(ask(GETPID, 0, 0, 0));\n  ask(GETCWD, (long)dir, sizeof dir, 0);\n  \
         say(dir);\n  say(\"\\n\");\n  long mask = ask(UMASK, 0, 0, 0);\n  \
         ask(UMASK, mask, 0, 0);\n  number(mask);\n  \
         number(ask(FCNTL, 3, 1, 0) < 0);\n  return 0;\n}}\n"
    );
    let random = "int rand(void);\nint gopp_entry(void) { return rand() < 0; }\n";
    let unresolved = "void missing(void);\nint gopp_entry(void) { missing(); return 0; }\n";
    suite
        .c("1-before", &reports)
        .c("2-changes", &changes)
        .c("3-after", &reports)
        .c("4-unresolved", unresolved)
        .c("5-same", &says_pid())
        .c("6-random", random)
        .c("7-new", &says_pid())
        .gopp("8-panics", PANICS)
        .c("9-newer", &says_pid());
    let results = suite.ran(&["-executors", "1", "-schedule", "forward"]);
    let lines =
        |id: &str| -> Vec<String> { results[id].stdout.lines().map(str::to_owned).collect() };
    let pid = |id: &str| lines(id)[0].clone();
    // One process ran the first five, and each found what the first
    // found: the directory, the mask, and no descriptor of another.
    let before = lines("1-before");
    assert_eq!(lines("2-changes"), [&*before[0], "3"]);
    assert_eq!(lines("3-after"), before);
    assert_eq!(before[3], "1");
    assert_ne!(before[1], "/");
    assert_eq!(pid("5-same"), before[0]);
    for id in ["1-before", "2-changes", "3-after", "4-unresolved", "5-same"] {
        assert!(!results[id].restarted, "{id}");
    }
    // A function that keeps state between calls: the program runs, and
    // its process ends with it.
    assert_eq!(
        (&*results["6-random"].outcome, results["6-random"].exit),
        ("exited", 0)
    );
    assert!(results["6-random"].restarted);
    assert_ne!(pid("7-new"), before[0]);
    assert!(!results["7-new"].restarted);
    assert!(results["8-panics"].restarted);
    assert_ne!(pid("9-newer"), pid("7-new"));
}

#[test]
fn an_executor_runs_64_programs_and_no_more() {
    let mut suite = Suite::new();
    let program = says_pid();
    for at in 0..70 {
        suite.c(&format!("{at:02}"), &program);
    }
    let results = suite.ran(&["-executors", "1", "-schedule", "forward"]);
    let ran: Vec<(&str, bool)> = results
        .values()
        .map(|result| (result.stdout.trim(), result.restarted))
        .collect();
    assert!(ran[..64].iter().all(|run| run.0 == ran[0].0));
    assert!(ran[64..].iter().all(|run| run.0 == ran[64].0));
    assert_ne!(ran[0].0, ran[64].0);
    let ended: Vec<usize> = (0..70).filter(|&at| ran[at].1).collect();
    assert_eq!(ended, [63]);
}

/// Programs that end in different ways, for runs in different orders.
fn mixed() -> Suite {
    let mut suite = Suite::new();
    let writes = "long write(int, const void *, unsigned long);\n\
                  int gopp_entry(void) { write(1, \"out\\n\", 4); write(2, \"err\\n\", 4); \
                  write(1, \"more\\n\", 5); return 0; }\n";
    suite
        .gopp("a", HELLO)
        .gopp("b", PANICS)
        .gopp("c", HELLO)
        .c(
            "d",
            "[[noreturn]] void abort(void);\nint gopp_entry(void) { abort(); }\n",
        )
        .c("e", writes)
        .job("f", "job.c", writes, json!({"merge_output": true}))
        .gopp("g", "package main\n\nfunc main() { var x int = \"a\" }\n")
        .c("h", "int gopp_entry(void) { return 5; }\n");
    suite
}

/// The results without which process each program ended: after a
/// program that ended its process comes a new one in every order.
fn unordered(mut results: BTreeMap<String, Ended>) -> BTreeMap<String, Ended> {
    for result in results.values_mut() {
        result.restarted = false;
    }
    results
}

#[test]
fn the_order_of_the_programs_changes_no_result() {
    let suite = mixed();
    let first = suite.ran(&["-executors", "1", "-schedule", "forward"]);
    assert_eq!(
        (&*first["e"].stdout, &*first["e"].stderr),
        ("out\nmore\n", "err\n")
    );
    assert_eq!(
        (&*first["f"].stdout, &*first["f"].stderr),
        ("out\nerr\nmore\n", "")
    );
    assert_eq!((&*first["d"].outcome, first["d"].signal), ("signaled", 6));
    let first = unordered(first);
    for schedule in ["reverse", "shuffle:1"] {
        let other = unordered(suite.ran(&["-executors", "1", "-schedule", schedule]));
        assert_eq!(other, first, "{schedule}");
    }
    // Many at a time, as they come.
    assert_eq!(unordered(suite.ran(&[])), first);
    // An order that is none.
    let output = suite.command(&["-schedule", "sideways"]).output().unwrap();
    assert_eq!(output.status.code(), Some(1));
    assert_eq!(
        String::from_utf8_lossy(&output.stderr),
        "error: -schedule: forward, reverse or shuffle:SEED, not sideways\n"
    );
}

#[test]
#[ignore = "long: many orders and numbers of executors"]
fn many_orders_change_no_result() {
    let suite = mixed();
    let first = unordered(suite.ran(&["-executors", "1", "-schedule", "forward"]));
    for seed in 2..40 {
        let schedule = format!("shuffle:{seed}");
        let executors = (seed % 4 + 1).to_string();
        let flags = ["-executors", &executors, "-schedule", &schedule];
        assert_eq!(unordered(suite.ran(&flags)), first, "{schedule}");
    }
    assert_eq!(unordered(suite.ran(&["-executors", "3", "-j", "2"])), first);
}

#[test]
fn real_executables_end_the_same_way() {
    let mut suite = Suite::new();
    suite
        .gopp("returns", HELLO)
        .gopp("panics", PANICS)
        .gopp("overflows", OVERFLOWS)
        .gopp("floods", FLOODS)
        .gopp(
            "wrong",
            "package main\n\nfunc main() { var x int = \"a\" }\n",
        )
        .c("status", "int gopp_entry(void) { return 3; }\n")
        .job("spins", "job.gopp", SPINS, json!({"timeout_ms": 150}));
    let flags = ["-max-output", "65536"];
    let fast = suite.ran(&flags);
    let real = suite.ran(&["-real", "-max-output", "65536"]);
    let mut by_variable = suite.command(&flags);
    let output = by_variable.env("GOPP_FAST_RUN", "0").output().unwrap();
    let by_variable = results(&String::from_utf8_lossy(&output.stdout));
    assert_eq!(by_variable.len(), real.len());
    for (id, fast) in &fast {
        // An executable is a process of its own each time, and its
        // addresses are its own.
        let same = |other: &Ended| {
            let trace = |text: &str| text.split("\ngoroutine ").next().unwrap().to_owned();
            (
                other.outcome.clone(),
                other.exit,
                other.signal,
                trace(&other.stderr),
            )
        };
        assert_eq!(same(&real[id]), same(fast), "{id}");
        assert_eq!(same(&by_variable[id]), same(fast), "{id}");
        assert_eq!(real[id].restarted, real[id].message.is_empty(), "{id}");
    }
    assert_eq!(real["floods"].signal, 25);
}

#[test]
fn a_runner_that_cannot_run_says_so_and_fails() {
    let mut suite = Suite::new();
    suite
        .gopp("a", HELLO)
        .gopp("wrong", "package main\n\nfunc main() { x }\n");
    // No executor at all: what builds has no result of a run, and the
    // runner fails.
    let (results, output) = suite.run(&["-executor", "/nonexistent/gopp"]);
    assert_eq!(output.status.code(), Some(1));
    assert_eq!(results["a"].outcome, "internal");
    assert!(
        results["a"]
            .message
            .starts_with("cannot start the executor /nonexistent/gopp: "),
        "{}",
        results["a"].message
    );
    assert_eq!(results["wrong"].outcome, "compile-failed");
    // An executor that ends at once, and one that never gets ready.
    let script = |name: &str, text: &str| -> PathBuf {
        use std::os::unix::fs::PermissionsExt;
        let path = suite.dir.path().join(name);
        fs::write(&path, text).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        path
    };
    let ends = script("ends", "#!/bin/sh\nexit 0\n");
    let (results, output) = suite.run(&["-executor", ends.to_str().unwrap()]);
    assert_eq!(output.status.code(), Some(1));
    let message = &results["a"].message;
    assert!(message.ends_with("ended before it was ready"), "{message}");
    let sleeps = script("sleeps", "#!/bin/sh\nexec sleep 20\n");
    let flags = [
        "-startup-timeout-ms",
        "100",
        "-executor",
        sleeps.to_str().unwrap(),
    ];
    let (results, output) = suite.run(&flags);
    assert_eq!(output.status.code(), Some(1));
    assert!(results["a"].message.ends_with("was not ready in time"));
    // A line that is no job of this version.
    suite
        .jobs
        .push(json!({"v": 2, "id": "later", "inputs": []}));
    let (results, output) = suite.run(&[]);
    assert_eq!((output.status.code(), results.len()), (Some(1), 0));
    let said = String::from_utf8_lossy(&output.stderr);
    assert!(
        said.ends_with("jobs:3: version 2 of a job, this runner reads 1\n"),
        "{said}"
    );
}

#[test]
fn the_results_go_where_they_are_asked_and_the_work_is_kept_on_request() {
    let dir = TempDir::new().unwrap();
    let file = dir.path().join("hello.gopp");
    fs::write(&file, HELLO).unwrap();
    let (out, work) = (dir.path().join("results"), dir.path().join("work"));
    let cache = Path::new(env!("CARGO_TARGET_TMPDIR")).join("gopp-cache");
    let output = Command::new(env!("CARGO_BIN_EXE_gopp"))
        .args(["test-run", "-summary", "-merge-output", "-o"])
        .arg(&out)
        .arg("-work-dir")
        .arg(&work)
        .arg(&file)
        .env("GOPPCACHE", cache)
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(0));
    assert_eq!(output.stdout, b"");
    // A program of the command line is named after its file.
    let results = results(&fs::read_to_string(&out).unwrap());
    assert_eq!(results["hello"].stdout, "hello\n");
    assert_eq!(results["hello"].stderr, "");
    let said = String::from_utf8_lossy(&output.stderr);
    assert!(said.starts_with("1 jobs in "), "{said}");
    assert!(said.contains(
        ": 1 exited, 0 signaled, 0 timeout, 0 compile-failed, 0 load-failed, 0 internal;"
    ));
    assert!(work.join("0").join("module.so").exists());
    assert_eq!(
        fs::read_to_string(work.join("0").join("stdout")).unwrap(),
        "hello\n"
    );
}
