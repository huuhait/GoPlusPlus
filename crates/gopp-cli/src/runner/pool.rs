//! The processes that run the programs of the fast runner
//! (test-runner.md): each is `gopp test-exec`, started when a program
//! needs one and kept for the next program if the last one left it
//! whole.

use std::fs;
use std::io::{BufRead, BufReader, ErrorKind, Write};
use std::os::fd::OwnedFd;
use std::os::unix::net::UnixStream;
use std::os::unix::process::ExitStatusExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::Duration;

use object::{Object, ObjectSymbol};

use super::exec::{Reply, Run};
use super::protocol::Outcome;

/// What every executor of a run is started with.
pub struct Settings {
    /// The program that is started as `gopp test-exec`.
    pub executor: PathBuf,
    /// The shared object of the runtime, which every module needs.
    pub runtime: PathBuf,
    pub max_output: u64,
    /// How long an executor may take until it is ready.
    pub startup: Duration,
}

/// One executor that waits for a program.
pub struct Worker {
    child: Child,
    asked: UnixStream,
    answers: BufReader<UnixStream>,
}

/// How a program ended in an executor.
#[derive(Debug)]
pub struct Ran {
    pub outcome: Outcome,
    pub exit: i32,
    pub signal: i32,
    pub message: String,
    /// The executor did not outlive the program.
    pub restarted: bool,
}

impl Ran {
    fn failed(outcome: Outcome, message: String) -> Self {
        Self {
            outcome,
            exit: 0,
            signal: 0,
            message,
            restarted: false,
        }
    }
}

/// The functions of the C library that keep nothing between two calls:
/// a module that needs no other leaves its executor as it found it.
const STATE_FREE: [&str; 44] = [
    "write",
    "exit",
    "_exit",
    "abort",
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "bcmp",
    "strlen",
    "malloc",
    "calloc",
    "realloc",
    "free",
    "_Unwind_Resume",
    "_Unwind_Backtrace",
    "_Unwind_GetIP",
    "_Unwind_GetRegionStart",
    "__gcc_personality_v0",
    "sin",
    "cos",
    "tan",
    "asin",
    "acos",
    "atan",
    "atan2",
    "sinh",
    "cosh",
    "tanh",
    "exp",
    "exp2",
    "log",
    "log2",
    "log10",
    "pow",
    "sqrt",
    "cbrt",
    "floor",
    "ceil",
    "trunc",
    "round",
    "fabs",
    "fmod",
    "fma",
];

/// What the module names and does not define. A name that may stay
/// undefined is none of them.
pub fn undefined_symbols(module: &Path) -> Result<Vec<String>, String> {
    let failed = |error: &dyn std::fmt::Display| format!("{}: {error}", module.display());
    let bytes = fs::read(module).map_err(|error| failed(&error))?;
    let file = object::File::parse(&*bytes).map_err(|error| failed(&error))?;
    let mut names: Vec<String> = file
        .dynamic_symbols()
        .filter(|symbol| symbol.is_undefined() && !symbol.is_weak())
        .filter_map(|symbol| symbol.name().ok())
        .filter(|name| !name.is_empty())
        .map(str::to_owned)
        .collect();
    names.sort_unstable();
    names.dedup();
    Ok(names)
}

/// Whether a module that names these may leave state behind in its
/// executor: it names more than the runtime and the functions that keep
/// nothing.
pub fn keeps_state(undefined: &[String]) -> bool {
    let free = |name: &String| name.starts_with("_gopp_") || STATE_FREE.contains(&&**name);
    !undefined.iter().all(free)
}

impl Settings {
    /// A new executor, which has said that it is ready.
    fn start(&self) -> Result<Worker, String> {
        let name = self.executor.display();
        let (ours, theirs) = UnixStream::pair().map_err(|error| error.to_string())?;
        let mut command = Command::new(&self.executor);
        command.arg("test-exec");
        command.arg(format!("--runtime={}", self.runtime.display()));
        command.arg(format!("--max-output={}", self.max_output));
        // The executor finds the socket where its input would be.
        command.stdin(Stdio::from(OwnedFd::from(theirs)));
        command.stdout(Stdio::null()).stderr(Stdio::inherit());
        let child = command
            .spawn()
            .map_err(|error| format!("cannot start the executor {name}: {error}"))?;
        // The command holds the other end of the socket: while it does,
        // an executor that ends is not seen to end.
        drop(command);
        let answers = ours.try_clone().map_err(|error| error.to_string())?;
        let mut worker = Worker {
            child,
            asked: ours,
            answers: BufReader::new(answers),
        };
        match worker.answer(self.startup) {
            Answer::Said(Reply::Ready) => Ok(worker),
            Answer::Late => {
                worker.end();
                Err(format!("the executor {name} was not ready in time"))
            }
            _ => {
                worker.end();
                Err(format!("the executor {name} ended before it was ready"))
            }
        }
    }

    /// Runs the program of the request in the executor of `kept`, or in
    /// a new one. `kept` has an executor afterwards if it is good for
    /// another program.
    pub fn run(&self, kept: &mut Option<Worker>, run: &Run, timeout: Duration) -> Ran {
        let mut worker = match kept.take() {
            Some(worker) => worker,
            None => match self.start() {
                Ok(worker) => worker,
                Err(message) => return Ran::failed(Outcome::Internal, message),
            },
        };
        let line = serde_json::to_string(run).unwrap_or_default();
        if writeln!(worker.asked, "{line}").is_err() {
            worker.end();
            let message = "the executor did not take the program".to_owned();
            return Ran::failed(Outcome::Internal, message);
        }
        match worker.answer(timeout) {
            Answer::Said(Reply::Done { exit, recycle }) => {
                match recycle {
                    true => worker.end(),
                    false => *kept = Some(worker),
                }
                Ran {
                    outcome: Outcome::Exited,
                    exit,
                    signal: 0,
                    message: String::new(),
                    restarted: recycle,
                }
            }
            Answer::Said(Reply::LoadFailed { message }) => {
                *kept = Some(worker);
                Ran::failed(Outcome::LoadFailed, message)
            }
            Answer::Said(Reply::Failed { message }) => {
                worker.end();
                Ran::failed(Outcome::Internal, message)
            }
            Answer::Said(Reply::Ready) | Answer::Broken => {
                worker.end();
                let message = "the executor answered what was not asked".to_owned();
                Ran::failed(Outcome::Internal, message)
            }
            Answer::Late => {
                worker.end();
                Ran {
                    restarted: true,
                    ..Ran::failed(Outcome::Timeout, String::new())
                }
            }
            // The program ended the process: it left with a status, or
            // a signal ended it.
            Answer::Gone => {
                let status = worker.child.wait().ok();
                let exit = status.and_then(|status| status.code());
                let signal = status.and_then(|status| status.signal());
                Ran {
                    outcome: match exit {
                        Some(_) => Outcome::Exited,
                        None => Outcome::Signaled,
                    },
                    exit: exit.unwrap_or(0),
                    signal: signal.unwrap_or(0),
                    message: String::new(),
                    restarted: true,
                }
            }
        }
    }
}

/// What came back from an executor.
enum Answer {
    Said(Reply),
    /// Nothing in the time that it had.
    Late,
    /// Its end of the socket is closed: the process is gone.
    Gone,
    /// What is no answer.
    Broken,
}

impl Worker {
    fn answer(&mut self, within: Duration) -> Answer {
        // A wait of no time at all would be a wait without an end.
        let within = within.max(Duration::from_millis(1));
        if self
            .answers
            .get_ref()
            .set_read_timeout(Some(within))
            .is_err()
        {
            return Answer::Broken;
        }
        let mut line = String::new();
        match self.answers.read_line(&mut line) {
            Ok(0) => Answer::Gone,
            Ok(_) => serde_json::from_str(&line).map_or(Answer::Broken, Answer::Said),
            Err(error) if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {
                Answer::Late
            }
            // The process was ended while it wrote, or before.
            Err(_) => Answer::Gone,
        }
    }

    /// Ends the process, whatever it does.
    pub fn end(mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
