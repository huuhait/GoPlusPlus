//! `gopp test-exec`: a process that runs one program after another,
//! each a shared module, and puts itself back as it was in between
//! (test-runner.md). It talks to the runner over a socket, which it
//! is started with as its standard input.
//!
//! This is the one place of `gopp` that asks the system directly: the
//! calls are those of the C library, on the one thread of the process.

#![allow(unsafe_code)]

use std::ffi::{CString, OsString, c_int};
use std::fs::File;
use std::io::{BufRead, BufReader, Write};
use std::os::fd::FromRawFd;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use libloading::os::unix::{Library, RTLD_GLOBAL, RTLD_LOCAL, RTLD_NOW};
use serde::{Deserialize, Serialize};

/// The descriptors of the process itself, out of the way of a program:
/// the socket, `/dev/null`, and the two streams that it was started
/// with. A program finds 0, 1 and 2 open and the next ones free.
const CONTROL: c_int = 900;
const NULL: c_int = 901;
const OUT: c_int = 902;
const ERR: c_int = 903;

/// After so many programs the process ends, whatever they did.
pub const JOBS_AT_MOST: u32 = 64;

/// The function of a module that runs its program.
pub const ENTRY: &str = "gopp_entry";

/// What the runner asks for: one program.
#[derive(Debug, Serialize, Deserialize)]
pub struct Run {
    pub module: PathBuf,
    /// The files that get what the program writes.
    pub stdout: PathBuf,
    pub stderr: PathBuf,
    pub merge_output: bool,
    /// What the module names and does not define, to say what is
    /// missing if it does not load.
    pub undefined: Vec<String>,
    /// The process ends after this program.
    pub recycle: bool,
}

/// What the process answers.
#[derive(Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Reply {
    /// It is set up and waits for a program.
    Ready,
    /// The program returned. With `recycle`, the process ends now.
    Done {
        exit: i32,
        recycle: bool,
    },
    LoadFailed {
        message: String,
    },
    /// The process could not do its own part.
    Failed {
        message: String,
    },
}

/// What a program may change and the process puts back.
struct Saved {
    dir: PathBuf,
    mask: libc::mode_t,
    env: Vec<(OsString, OsString)>,
    limits: Vec<(libc::__rlimit_resource_t, libc::rlimit)>,
}

const LIMITS: [libc::__rlimit_resource_t; 8] = [
    libc::RLIMIT_AS,
    libc::RLIMIT_CPU,
    libc::RLIMIT_DATA,
    libc::RLIMIT_FSIZE,
    libc::RLIMIT_NOFILE,
    libc::RLIMIT_STACK,
    libc::RLIMIT_NPROC,
    libc::RLIMIT_MEMLOCK,
];

fn limit(resource: libc::__rlimit_resource_t) -> libc::rlimit {
    let mut limit = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    // SAFETY: `limit` is a place for what the call writes.
    unsafe { libc::getrlimit(resource, &mut limit) };
    limit
}

impl Saved {
    fn now() -> Self {
        // SAFETY: the mask is read by setting it, and set back at once.
        let mask = unsafe {
            let mask = libc::umask(0);
            libc::umask(mask);
            mask
        };
        Self {
            dir: std::env::current_dir().unwrap_or_else(|_| PathBuf::from("/")),
            mask,
            env: std::env::vars_os().collect(),
            limits: LIMITS
                .iter()
                .map(|&resource| (resource, limit(resource)))
                .collect(),
        }
    }
}

fn path_of_c(path: &Path) -> Result<CString, String> {
    CString::new(path.as_os_str().as_bytes()).map_err(|_| format!("{}: a NUL", path.display()))
}

/// Every signal that a process may take in hand.
fn signals() -> impl Iterator<Item = c_int> {
    (1..=libc::SIGRTMAX()).filter(|&signal| signal != libc::SIGKILL && signal != libc::SIGSTOP)
}

/// Moves the descriptors of the process out of the way and limits what
/// a program can do to the machine. The socket is what the process was
/// started with as its standard input.
fn set_up(max_output: u64) -> Result<File, String> {
    let null = path_of_c(Path::new("/dev/null"))?;
    // SAFETY: descriptors of this process are copied to numbers that
    // nothing else has, on its one thread; the calls after them take
    // values and places that live through them.
    unsafe {
        let opened = libc::open(null.as_ptr(), libc::O_RDWR);
        let moved = [(0, CONTROL), (opened, NULL), (1, OUT), (2, ERR), (NULL, 0)];
        for (from, to) in moved {
            if from < 0 || libc::dup2(from, to) < 0 {
                return Err("the descriptors of the executor cannot be set up".to_owned());
            }
        }
        libc::close(opened);
        // A process of Rust starts with some signals in hand.
        for signal in signals() {
            libc::signal(signal, libc::SIG_DFL);
        }
        let none = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        libc::setrlimit(libc::RLIMIT_CORE, &none);
        // More output than this ends a program with SIGXFSZ.
        let output = libc::rlimit {
            rlim_cur: max_output,
            rlim_max: max_output,
        };
        libc::setrlimit(libc::RLIMIT_FSIZE, &output);
        // No executor outlives the runner.
        libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL);
        Ok(File::from_raw_fd(CONTROL))
    }
}

/// Closes every descriptor that is not one of the first three or of
/// the process itself.
fn close_others() {
    let last = c_int::MAX as libc::c_uint;
    // SAFETY: the ranges hold no descriptor that the process uses.
    let closed = unsafe {
        libc::syscall(libc::SYS_close_range, 3, CONTROL - 1, 0) == 0
            && libc::syscall(libc::SYS_close_range, ERR + 1, last, 0) == 0
    };
    if closed {
        return;
    }
    // A system without that call: the descriptors are listed.
    let open: Vec<c_int> = std::fs::read_dir("/proc/self/fd")
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok()?.file_name().to_str()?.parse().ok())
        .collect();
    for descriptor in open {
        if descriptor > 2 && !(CONTROL..=ERR).contains(&descriptor) {
            // SAFETY: the descriptor is none that the process uses.
            unsafe { libc::close(descriptor) };
        }
    }
}

/// Puts the process back as it was before a program ran. Whether it
/// could not: the process must end then.
fn reset(saved: &Saved) -> bool {
    // SAFETY: the calls change the state of this process, on its one
    // thread, with values and places that live through them.
    unsafe {
        libc::fflush(std::ptr::null_mut());
        for (from, to) in [(NULL, 0), (OUT, 1), (ERR, 2)] {
            libc::dup2(from, to);
        }
        close_others();
        *libc::__errno_location() = 0;
        libc::alarm(0);
        let never: libc::itimerval = std::mem::zeroed();
        for timer in [libc::ITIMER_REAL, libc::ITIMER_VIRTUAL, libc::ITIMER_PROF] {
            libc::setitimer(timer, &never, std::ptr::null_mut());
        }
        // A signal that waits is dropped when it is ignored.
        for signal in signals() {
            libc::signal(signal, libc::SIG_IGN);
            libc::signal(signal, libc::SIG_DFL);
        }
        let mut none: libc::sigset_t = std::mem::zeroed();
        libc::sigemptyset(&mut none);
        libc::sigprocmask(libc::SIG_SETMASK, &none, std::ptr::null_mut());
        let off = libc::stack_t {
            ss_sp: std::ptr::null_mut(),
            ss_flags: libc::SS_DISABLE,
            ss_size: 0,
        };
        libc::sigaltstack(&off, std::ptr::null_mut());
        libc::umask(saved.mask);
    }
    let mut recycle = std::env::set_current_dir(&saved.dir).is_err();
    let env: Vec<(OsString, OsString)> = std::env::vars_os().collect();
    if env != saved.env {
        // SAFETY: the process has one thread, which is checked below.
        unsafe {
            for (name, _) in &env {
                std::env::remove_var(name);
            }
            for (name, value) in &saved.env {
                std::env::set_var(name, value);
            }
        }
    }
    for (resource, was) in &saved.limits {
        let now = limit(*resource);
        let same = now.rlim_cur == was.rlim_cur && now.rlim_max == was.rlim_max;
        // SAFETY: `was` lives through the call. A limit that was
        // lowered for good cannot be raised again.
        recycle |= !same && unsafe { libc::setrlimit(*resource, was) } != 0;
    }
    let threads = std::fs::read_dir("/proc/self/task").map_or(0, Iterator::count);
    recycle | (threads != 1)
}

/// Gives the program the two files for what it writes.
fn open_output(run: &Run) -> Result<(), String> {
    let open = |path: &Path, to: c_int| {
        let name = path_of_c(path)?;
        let flags = libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC;
        // SAFETY: `name` lives through the call; the descriptor that it
        // gives is copied over a stream of the process and closed.
        let opened = unsafe {
            let file = libc::open(name.as_ptr(), flags, 0o644);
            let opened = file >= 0 && libc::dup2(file, to) >= 0;
            libc::close(file);
            opened
        };
        match opened {
            true => Ok(()),
            false => Err(format!("{}: cannot be written", path.display())),
        }
    };
    open(&run.stdout, 1)?;
    match run.merge_output {
        // SAFETY: both are streams of the process.
        true if unsafe { libc::dup2(1, 2) } < 0 => Err("the streams cannot be merged".to_owned()),
        true => Ok(()),
        false => open(&run.stderr, 2),
    }
}

/// Which of the names nothing that is loaded defines.
fn missing(names: &[String]) -> Vec<&str> {
    let defined = |name: &&String| {
        let Ok(name) = CString::new(name.as_bytes()) else {
            return true;
        };
        // SAFETY: `name` lives through the call.
        !unsafe { libc::dlsym(libc::RTLD_DEFAULT, name.as_ptr()) }.is_null()
    };
    let mut missing: Vec<&str> = names
        .iter()
        .filter(|name| !defined(name))
        .map(|name| &**name)
        .collect();
    missing.sort_unstable();
    missing
}

/// Runs the program of one module.
fn one(run: &Run, saved: &Saved) -> Reply {
    if let Err(message) = open_output(run) {
        reset(saved);
        return Reply::Failed { message };
    }
    // SAFETY: the module is code that the runner built for this; what
    // runs when it is loaded is its own.
    let module = match unsafe { Library::open(Some(&run.module), RTLD_NOW | RTLD_LOCAL) } {
        Ok(module) => module,
        Err(error) => {
            reset(saved);
            let message = match &missing(&run.undefined)[..] {
                [] => error.to_string(),
                missing => format!("undefined symbols: {}", missing.join(" ")),
            };
            return Reply::LoadFailed { message };
        }
    };
    // SAFETY: the entry of a module is a function of C without
    // parameters that gives an `int`.
    let exit = unsafe {
        match module.get::<unsafe extern "C" fn() -> c_int>(ENTRY.as_bytes()) {
            Ok(entry) => Ok(entry()),
            Err(_) => Err(format!("the module has no `{ENTRY}`")),
        }
    };
    let recycle = reset(saved) | run.recycle;
    drop(module);
    match exit {
        Ok(exit) => Reply::Done { exit, recycle },
        Err(message) => Reply::LoadFailed { message },
    }
}

/// Sets `command` up so that the files it writes stay under
/// `max_output` bytes each: more ends it with SIGXFSZ.
pub fn limit_output(command: &mut std::process::Command, max_output: u64) {
    use std::os::unix::process::CommandExt;
    let limit = libc::rlimit {
        rlim_cur: max_output,
        rlim_max: max_output,
    };
    // SAFETY: between the fork and the start of the program only this
    // one call of the system is made, with a value that was copied.
    unsafe {
        command.pre_exec(move || match libc::setrlimit(libc::RLIMIT_FSIZE, &limit) {
            0 => Ok(()),
            _ => Err(std::io::Error::last_os_error()),
        });
    }
}

/// The process: it loads the runtime, says that it is ready, and runs
/// the programs that it is sent until the socket closes.
pub fn run(runtime: &Path, max_output: u64) -> ExitCode {
    let failed = |message: String| {
        eprintln!("error: gopp test-exec: {message}");
        ExitCode::from(crate::EXIT_INTERNAL)
    };
    let control = match set_up(max_output) {
        Ok(control) => control,
        Err(message) => return failed(message),
    };
    // SAFETY: the runtime is code that `gopp` built; every module finds
    // its functions in it, so it stays for the life of the process.
    let _runtime = match unsafe { Library::open(Some(runtime), RTLD_NOW | RTLD_GLOBAL) } {
        Ok(runtime) => runtime,
        Err(error) => return failed(error.to_string()),
    };
    let saved = Saved::now();
    // One descriptor for both ways: a copy of it would get a number
    // that belongs to the programs.
    let mut asked = BufReader::new(&control);
    let answer = |reply: &Reply| {
        let line = serde_json::to_string(reply).unwrap_or_default();
        writeln!(&control, "{line}").is_ok()
    };
    if !answer(&Reply::Ready) {
        return ExitCode::SUCCESS;
    }
    let mut line = String::new();
    let mut ran = 0;
    loop {
        line.clear();
        // The runner is done, or gone.
        if asked.read_line(&mut line).unwrap_or(0) == 0 {
            break;
        }
        let mut reply = match serde_json::from_str::<Run>(&line) {
            Ok(run) => one(&run, &saved),
            Err(error) => Reply::Failed {
                message: error.to_string(),
            },
        };
        if let Reply::Done { recycle, .. } = &mut reply {
            ran += 1;
            *recycle |= ran == JOBS_AT_MOST;
        }
        let last = matches!(reply, Reply::Done { recycle: true, .. });
        if !answer(&reply) || last {
            break;
        }
    }
    ExitCode::SUCCESS
}
