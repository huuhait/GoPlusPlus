//! `gopp test-run`: builds many programs side by side and runs each,
//! and says in one line of JSON how each ended (test-runner.md).
//!
//! The fast way, on Linux: a program is a shared module that a
//! `gopp test-exec` process loads, and a process runs one program after
//! another. The other way, `-real`: a program is an executable of its
//! own.

#[cfg(target_os = "linux")]
pub mod exec;
#[cfg(target_os = "linux")]
mod pool;
mod protocol;

use std::fs::{self, File};
use std::io::{self, BufRead, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::sync::Mutex;
use std::sync::mpsc::{self, Sender};
use std::time::{Duration, Instant};

use gopp_codegen_c::driver::{ClangDriver, OptLevel, Output};
use gopp_codegen_c::runtime::{self, Form};
use gopp_codegen_c::toolchain::HostToolchain;
use gopp_codegen_c::{EmitOptions, Entry, emit_c23};
use gopp_diag::Code;
use gopp_target::TargetSpec;
use rayon::prelude::*;

use self::protocol::{JobResult, JobSpec, Outcome, VERSION, base64, job_of};
use crate::build::{cache_dir, imported_units, lowered};

// Only the fast way, which is on Linux, reads all of it.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
pub struct Options {
    /// Programs: a file or a directory each.
    pub paths: Vec<PathBuf>,
    /// A file of jobs, one a line; `-` is the standard input.
    pub manifest: Option<PathBuf>,
    /// Where the results go, in the place of the standard output.
    pub output: Option<PathBuf>,
    pub jobs: Option<usize>,
    pub executors: Option<usize>,
    pub timeout_ms: u64,
    pub max_output: u64,
    pub work_dir: Option<PathBuf>,
    pub merge_output: bool,
    pub summary: bool,
    pub real: bool,
    pub opt: OptLevel,
    pub executor: Option<PathBuf>,
    pub startup_timeout_ms: u64,
    pub schedule: Option<String>,
}

/// What every job of a run is built and run with.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
struct Run<'a> {
    driver: ClangDriver<'a>,
    /// What a link is given of the runtime: nothing for a module, which
    /// finds the runtime in its process.
    link_with: Vec<PathBuf>,
    /// A program is an executable of its own.
    real: bool,
    work: &'a Path,
    timeout: Duration,
    max_output: u64,
    merge_output: bool,
    #[cfg(target_os = "linux")]
    pool: pool::Settings,
}

/// A job that is built, with what runs it.
struct Built {
    at: usize,
    program: PathBuf,
    compile_ms: f64,
}

fn millis(since: Instant) -> f64 {
    since.elapsed().as_secs_f64() * 1e3
}

/// The jobs of the command line and of the manifest, in their order.
fn jobs(options: &Options) -> Result<Vec<JobSpec>, String> {
    let named = options.paths.iter().map(|path| {
        let stem = path.file_stem().unwrap_or(path.as_os_str());
        JobSpec {
            v: VERSION,
            id: stem.to_string_lossy().into_owned(),
            inputs: vec![path.clone()],
            target: "host".to_owned(),
            timeout_ms: 0,
            merge_output: false,
        }
    });
    let mut jobs: Vec<JobSpec> = named.collect();
    let Some(manifest) = &options.manifest else {
        return Ok(jobs);
    };
    let name = manifest.display();
    let failed = |error: io::Error| format!("{name}: {error}");
    let reader: Box<dyn BufRead> = match manifest.as_os_str() == "-" {
        true => Box::new(io::stdin().lock()),
        false => Box::new(io::BufReader::new(File::open(manifest).map_err(failed)?)),
    };
    for (at, line) in reader.lines().enumerate() {
        let line = line.map_err(failed)?;
        if line.trim().is_empty() {
            continue;
        }
        let job = job_of(&line).map_err(|error| format!("{name}:{}: {error}", at + 1))?;
        jobs.push(job);
    }
    Ok(jobs)
}

/// The order in which the jobs are given to the executors, for a run
/// that must be the same each time: as they stand, the other way round,
/// or mixed by a seed.
fn schedule(count: usize, how: &str) -> Result<Vec<usize>, String> {
    let mut order: Vec<usize> = (0..count).collect();
    match how.split_once(':') {
        None if how == "forward" => {}
        None if how == "reverse" => order.reverse(),
        Some(("shuffle", seed)) => {
            let mut state: u64 = seed
                .parse()
                .map_err(|_| format!("-schedule: {seed} is no seed"))?;
            // The steps of splitmix64.
            let mut next = || {
                state = state.wrapping_add(0x9e37_79b9_7f4a_7c15);
                let mut mixed = state;
                mixed = (mixed ^ mixed >> 30).wrapping_mul(0xbf58_476d_1ce4_e5b9);
                mixed = (mixed ^ mixed >> 27).wrapping_mul(0x94d0_49bb_1331_11eb);
                mixed ^ mixed >> 31
            };
            for at in (1..count).rev() {
                order.swap(at, (next() % (at as u64 + 1)) as usize);
            }
        }
        _ => {
            return Err(format!(
                "-schedule: forward, reverse or shuffle:SEED, not {how}"
            ));
        }
    }
    Ok(order)
}

impl Run<'_> {
    fn dir(&self, at: usize) -> PathBuf {
        self.work.join(at.to_string())
    }

    /// The unit of C of a job: its package, lowered, or its one file of
    /// C after the header of the runtime. The units of the packages that
    /// it imports are written to `dir`, and are what it is built with;
    /// so are the files of C that are named beside a package, which the
    /// same Clang compiles with the same flags.
    fn unit(&self, job: &JobSpec, dir: &Path) -> Result<(String, Vec<PathBuf>), String> {
        let target = self.driver.target;
        let is_c = |file: &PathBuf| file.extension().is_some_and(|extension| extension == "c");
        if let [file] = &job.inputs[..]
            && is_c(file)
        {
            let text = fs::read_to_string(file)
                .map_err(|error| format!("error: {}: {error}\n", file.display()))?;
            let header = runtime::header(target.layout());
            // An executable starts where a module is entered.
            let start = match self.real {
                true => "\nint main(void) { return gopp_entry(); }\n",
                false => "",
            };
            return Ok((format!("{header}\n{text}{start}"), Vec::new()));
        }
        let (of_c, sources): (Vec<PathBuf>, Vec<PathBuf>) =
            job.inputs.iter().cloned().partition(is_c);
        let mut said = String::new();
        let Some(package) = lowered(&sources, (target, false), false, &mut said) else {
            return Err(said);
        };
        if !package.is_main {
            return Err(
                "error: the package is not `main`: there is no program to run\n".to_owned(),
            );
        }
        let paths: Vec<String> = (package.imports.iter())
            .map(|(path, _)| path.clone())
            .collect();
        let options = EmitOptions {
            layout: target.layout(),
            entry: match self.real {
                true => Entry::Main,
                false => Entry::Module,
            },
            imports: &paths,
            swiftcall: target.swiftcall,
            aborts: false,
            debug: false,
        };
        let codegen = |error: String| format!("error[{}]: {error}\n", Code::Codegen);
        let unit =
            emit_c23(&package.module, &options).map_err(|error| codegen(error.to_string()))?;
        let mut with = imported_units(&package.imports, &options, dir).map_err(codegen)?;
        for file in of_c {
            let whole = std::path::absolute(&file)
                .map_err(|error| format!("error: {}: {error}\n", file.display()))?;
            with.push(whole);
        }
        Ok((unit, with))
    }

    /// Builds the program of the job: a module, or an executable.
    fn build(&self, at: usize, job: &JobSpec) -> Result<Built, Box<JobResult>> {
        let start = Instant::now();
        let failed = |message: String| {
            let mut result = JobResult::without_run(&job.id, Outcome::CompileFailed, message);
            result.compile_ms = millis(start);
            Box::new(result)
        };
        let host = self.driver.target;
        let named = format!("{}/{}", host.os.name(), host.arch.name());
        if job.target != "host" && job.target != named {
            let target = &job.target;
            let message = format!("error: a program for {target} cannot run on this machine\n");
            return Err(failed(message));
        }
        let dir = self.dir(at);
        fs::create_dir_all(&dir)
            .map_err(|error| failed(format!("error: {}: {error}\n", dir.display())))?;
        let (unit, mut link_with) = self.unit(job, &dir).map_err(failed)?;
        link_with.extend(self.link_with.iter().cloned());
        let (kind, name) = match self.real {
            true => (Output::Executable, "program"),
            false => (Output::Module, "module.so"),
        };
        let program = dir.join(name);
        self.driver
            .build(&unit, &dir, &program, kind, &link_with)
            .map_err(|error| failed(format!("error[{}]: {error}\n", Code::Codegen)))?;
        Ok(Built {
            at,
            program,
            compile_ms: millis(start),
        })
    }

    /// Runs the executable of a job as a process of its own.
    fn run_real(&self, job: &JobSpec, built: &Built, timeout: Duration) -> JobResult {
        let dir = self.dir(built.at);
        let (out, err) = (dir.join("stdout"), dir.join("stderr"));
        let mut result = JobResult::without_run(&job.id, Outcome::Internal, String::new());
        let files = File::create(&out).and_then(|out_file| {
            let err_file = match job.merge_output || self.merge_output {
                true => out_file.try_clone()?,
                false => File::create(&err)?,
            };
            Ok((out_file, err_file))
        });
        let (out_file, err_file) = match files {
            Ok(files) => files,
            Err(error) => {
                result.message = format!("{}: {error}", dir.display());
                return result;
            }
        };
        let mut command = Command::new(&built.program);
        command
            .stdin(Stdio::null())
            .stdout(out_file)
            .stderr(err_file);
        #[cfg(target_os = "linux")]
        exec::limit_output(&mut command, self.max_output);
        let start = Instant::now();
        let mut child = match command.spawn() {
            Ok(child) => child,
            Err(error) => {
                result.message = format!("{}: {error}", built.program.display());
                return result;
            }
        };
        // The process is asked whether it has ended until its time is
        // over.
        let status = loop {
            match child.try_wait() {
                Ok(Some(status)) => break Some(status),
                Ok(None) if start.elapsed() < timeout => {
                    std::thread::sleep(Duration::from_millis(1));
                }
                _ => {
                    let _ = child.kill();
                    let _ = child.wait();
                    break None;
                }
            }
        };
        result.run_ms = millis(start);
        result.restarted = true;
        match status {
            None => result.outcome = Outcome::Timeout,
            Some(status) => match status.code() {
                Some(code) => (result.outcome, result.exit) = (Outcome::Exited, code),
                None => {
                    result.outcome = Outcome::Signaled;
                    #[cfg(unix)]
                    {
                        use std::os::unix::process::ExitStatusExt;
                        result.signal = status.signal().unwrap_or(0);
                    }
                }
            },
        }
        result
    }

    /// Runs the module of a job in an executor.
    #[cfg(target_os = "linux")]
    fn run_fast(
        &self,
        job: &JobSpec,
        built: &Built,
        timeout: Duration,
        kept: &mut Option<pool::Worker>,
    ) -> JobResult {
        let dir = self.dir(built.at);
        let mut result = JobResult::without_run(&job.id, Outcome::Internal, String::new());
        let undefined = match pool::undefined_symbols(&built.program) {
            Ok(undefined) => undefined,
            Err(message) => {
                result.message = message;
                return result;
            }
        };
        let request = exec::Run {
            module: built.program.clone(),
            stdout: dir.join("stdout"),
            stderr: dir.join("stderr"),
            merge_output: job.merge_output || self.merge_output,
            recycle: pool::keeps_state(&undefined),
            undefined,
        };
        let start = Instant::now();
        let ran = self.pool.run(kept, &request, timeout);
        result.run_ms = millis(start);
        result.outcome = ran.outcome;
        (result.exit, result.signal) = (ran.exit, ran.signal);
        (result.message, result.restarted) = (ran.message, ran.restarted);
        result
    }

    /// Runs a job that is built, and adds what it wrote to its result.
    fn run(&self, job: &JobSpec, built: &Built, kept: &mut Kept) -> JobResult {
        let timeout = match job.timeout_ms {
            0 => self.timeout,
            millis => Duration::from_millis(millis),
        };
        #[cfg(target_os = "linux")]
        let mut result = match self.real {
            true => self.run_real(job, built, timeout),
            false => self.run_fast(job, built, timeout, kept),
        };
        #[cfg(not(target_os = "linux"))]
        let mut result = {
            let _ = kept;
            self.run_real(job, built, timeout)
        };
        result.compile_ms = built.compile_ms;
        let dir = self.dir(built.at);
        let written = |name: &str| base64(&fs::read(dir.join(name)).unwrap_or_default());
        (result.stdout, result.stderr) = (written("stdout"), written("stderr"));
        result
    }
}

/// The executor that a thread keeps from one job to the next.
#[cfg(target_os = "linux")]
type Kept = Option<pool::Worker>;
#[cfg(not(target_os = "linux"))]
type Kept = ();

fn end(kept: Kept) {
    #[cfg(target_os = "linux")]
    if let Some(worker) = kept {
        worker.end();
    }
    #[cfg(not(target_os = "linux"))]
    let () = kept;
}

fn stopped(message: impl std::fmt::Display) -> ExitCode {
    eprintln!("error: {message}");
    ExitCode::from(crate::EXIT_ERRORS)
}

pub fn run(options: &Options) -> ExitCode {
    let jobs = match jobs(options) {
        Ok(jobs) => jobs,
        Err(message) => return stopped(message),
    };
    let order = match options
        .schedule
        .as_deref()
        .map(|how| schedule(jobs.len(), how))
    {
        Some(Ok(order)) => Some(order),
        Some(Err(message)) => return stopped(message),
        None => None,
    };
    // Only Linux has the fast way.
    let fast_off = std::env::var("GOPP_FAST_RUN").is_ok_and(|value| value == "0");
    let real = options.real || fast_off || !cfg!(target_os = "linux");
    let toolchain = match HostToolchain::discover() {
        Ok(toolchain) => toolchain,
        Err(error) => return stopped(error),
    };
    let target = match TargetSpec::host().and_then(TargetSpec::in_this_version) {
        Ok(target) => target,
        Err(error) => return stopped(error),
    };
    let driver = ClangDriver {
        toolchain: &toolchain,
        target,
        opt: options.opt,
        strict: false,
        debug: false,
    };
    let form = match real {
        true => Form::Objects,
        false => Form::Shared,
    };
    let built = cache_dir().and_then(|cache| {
        let checked = driver.check_link_inputs();
        let built = checked.and_then(|()| runtime::build(&driver, form, &cache));
        built.map_err(|error| error.to_string())
    });
    let runtime = match built {
        Ok(runtime) => runtime,
        Err(message) => return stopped(message),
    };
    let kept_dir;
    let work = match &options.work_dir {
        Some(dir) => dir.clone(),
        None => match tempfile::tempdir() {
            Ok(dir) => {
                kept_dir = dir;
                kept_dir.path().to_owned()
            }
            Err(error) => return stopped(format!("no directory to work in: {error}")),
        },
    };
    let work = match fs::create_dir_all(&work).and_then(|()| std::path::absolute(&work)) {
        Ok(work) => work,
        Err(error) => return stopped(format!("{}: {error}", work.display())),
    };
    let run = Run {
        driver,
        link_with: match real {
            true => runtime.clone(),
            false => Vec::new(),
        },
        real,
        work: &work,
        timeout: Duration::from_millis(options.timeout_ms),
        max_output: options.max_output,
        merge_output: options.merge_output,
        #[cfg(target_os = "linux")]
        pool: pool::Settings {
            executor: match &options.executor {
                Some(executor) => executor.clone(),
                None => std::env::current_exe().unwrap_or_else(|_| "gopp".into()),
            },
            runtime: runtime.first().cloned().unwrap_or_default(),
            max_output: options.max_output,
            startup: Duration::from_millis(options.startup_timeout_ms),
        },
    };
    let mut output: Box<dyn Write> = match &options.output {
        None => Box::new(io::stdout().lock()),
        Some(path) => match File::create(path) {
            Ok(file) => Box::new(file),
            Err(error) => return stopped(format!("{}: {error}", path.display())),
        },
    };
    let cores = std::thread::available_parallelism().map_or(1, usize::from);
    let executors = options
        .executors
        .unwrap_or(cores)
        .clamp(1, jobs.len().max(1));
    let builders = match rayon::ThreadPoolBuilder::new()
        .num_threads(options.jobs.unwrap_or(0))
        .build()
    {
        Ok(builders) => builders,
        Err(error) => return stopped(error),
    };
    let start = Instant::now();
    let (built_to, built_from) = mpsc::channel::<Built>();
    let built_from = Mutex::new(built_from);
    let (done_to, done_from) = mpsc::channel::<JobResult>();
    let (run, jobs) = (&run, &jobs);
    // A job that does not build has its result at once; one that does
    // goes to the executors.
    let build = |at: usize, built_to: &Sender<Built>, done_to: &Sender<JobResult>| {
        let _ = match run.build(at, &jobs[at]) {
            Ok(built) => built_to.send(built).is_ok(),
            Err(result) => done_to.send(*result).is_ok(),
        };
    };
    let mut results: Vec<JobResult> = Vec::with_capacity(jobs.len());
    let mut written = Ok(());
    std::thread::scope(|scope| {
        for _ in 0..executors {
            let (done_to, built_from) = (done_to.clone(), &built_from);
            scope.spawn(move || {
                let mut kept = Kept::default();
                loop {
                    // The next job, or the end of them.
                    let next = built_from.lock().ok().and_then(|from| from.recv().ok());
                    let Some(built) = next else {
                        break;
                    };
                    let result = run.run(&jobs[built.at], &built, &mut kept);
                    if done_to.send(result).is_err() {
                        break;
                    }
                }
                end(kept);
            });
        }
        scope.spawn(move || {
            let senders = (built_to, done_to);
            builders.install(|| match &order {
                // In an order that is asked for, every job is built
                // before the first one runs.
                Some(order) => {
                    let all: Vec<Result<Built, Box<JobResult>>> = (0..jobs.len())
                        .into_par_iter()
                        .map(|at| run.build(at, &jobs[at]))
                        .collect();
                    let mut all: Vec<Option<Result<Built, Box<JobResult>>>> =
                        all.into_iter().map(Some).collect();
                    for &at in order {
                        let _ = match all[at].take() {
                            Some(Ok(built)) => senders.0.send(built).is_ok(),
                            Some(Err(result)) => senders.1.send(*result).is_ok(),
                            None => false,
                        };
                    }
                }
                None => (0..jobs.len()).into_par_iter().for_each_with(
                    senders.clone(),
                    |(built_to, done_to), at| {
                        build(at, built_to, done_to);
                    },
                ),
            });
        });
        for result in done_from {
            let line = serde_json::to_string(&result).unwrap_or_default();
            if written.is_ok() {
                written = writeln!(output, "{line}").and_then(|()| output.flush());
            }
            results.push(result);
        }
    });
    if let Err(error) = written {
        return stopped(format!("the results cannot be written: {error}"));
    }
    if options.summary {
        let count = |outcome: Outcome| {
            results
                .iter()
                .filter(|result| result.outcome == outcome)
                .count()
        };
        let sum = |of: fn(&JobResult) -> f64| results.iter().map(of).sum::<f64>();
        eprintln!(
            "{} jobs in {:.0} ms: {} exited, {} signaled, {} timeout, {} compile-failed, \
             {} load-failed, {} internal; compile {:.0} ms, run {:.0} ms, {} restarted",
            results.len(),
            millis(start),
            count(Outcome::Exited),
            count(Outcome::Signaled),
            count(Outcome::Timeout),
            count(Outcome::CompileFailed),
            count(Outcome::LoadFailed),
            count(Outcome::Internal),
            sum(|result| result.compile_ms),
            sum(|result| result.run_ms),
            results.iter().filter(|result| result.restarted).count(),
        );
    }
    let internal = results
        .iter()
        .any(|result| result.outcome == Outcome::Internal);
    crate::exit_code(internal || results.len() != jobs.len())
}
