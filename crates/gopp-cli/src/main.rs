//! The `gopp` command.

mod args;
mod build;
mod check;
mod demangle;
mod diff;
mod fmt;
mod packages;
mod parse;
mod runner;
mod times;

use std::env;
use std::panic;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand, ValueEnum};
use gopp_codegen_c::toolchain::{CLANG_MAJOR, HostToolchain};

const EXIT_ERRORS: u8 = 1;
const EXIT_INTERNAL: u8 = 3;

#[derive(Parser)]
#[command(name = "gopp", about = "The Go++ compiler")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Print the compiler version
    Version,
    /// Locate the host Clang and LLD and check their version
    HostToolchain,
    /// List the targets that gopp knows, and those that it builds for
    Targets,
    /// Parse source files and report their syntax errors
    Parse {
        /// Print each file in this form
        #[arg(long, value_enum)]
        emit: Option<Emit>,
        /// Read the file names from standard input, one per line, and write one
        /// framed record per file
        #[arg(long)]
        batch: bool,
        #[arg(required_unless_present = "batch", conflicts_with = "batch")]
        files: Vec<PathBuf>,
    },
    /// Print source files in canonical layout
    Fmt {
        /// List the files whose layout is not canonical
        #[arg(short)]
        l: bool,
        /// Print the changes as a unified diff
        #[arg(short)]
        d: bool,
        /// Write the result to the file when its layout is not canonical
        #[arg(short)]
        w: bool,
        /// Files, and directories to format every .gopp file under;
        /// with none, standard input is formatted to standard output
        paths: Vec<PathBuf>,
    },
    /// Check packages and report what is wrong with them
    Check {
        /// Print each diagnostic as one line of JSON on standard output
        #[arg(long)]
        json: bool,
        /// Print what was found
        #[arg(long, value_enum)]
        emit: Option<EmitChecked>,
        /// Directories, each a package, and files, which are one package
        /// together
        #[arg(required = true)]
        paths: Vec<PathBuf>,
    },
    /// Compile one package
    Build {
        /// The target, as `gopp targets` names it; the default is this
        /// machine, or what GOPPOS and GOPPARCH say
        #[arg(long)]
        target: Option<String>,
        /// What a panic does: `unwind` makes the deferred calls, which
        /// may stop it; `abort` ends the program at once
        #[arg(long, value_enum, default_value = "unwind")]
        panic: PanicMode,
        /// Leave out what a debugger reads: the lines of the source
        /// and the names of the variables
        #[arg(long)]
        no_debug: bool,
        /// How much Clang optimizes; the program means the same at
        /// every level
        #[arg(long, value_enum, default_value = "0")]
        opt: Opt,
        /// Print how long each stage of the build took, as
        /// `stage=wall/cpu`, to standard error
        #[arg(long)]
        pipeline_times: bool,
        /// Write the stages of the build, and those of Clang, to this
        /// file as a trace that Chrome reads
        #[arg(long)]
        trace: Option<PathBuf>,
        /// The root of the files of the target, for a link for another
        /// system: where its C library and its startup objects are
        #[arg(long)]
        sysroot: Option<PathBuf>,
        /// The directory of the startup objects and of the C library of
        /// the target
        #[arg(long)]
        crt_dir: Option<PathBuf>,
        /// The dynamic loader that the program names
        #[arg(long)]
        dynamic_linker: Option<String>,
        /// What is made of the package
        #[arg(long, value_enum, default_value = "exe")]
        emit: EmitBuilt,
        /// Where the result is written; `-emit=gir` and `-emit=c` print
        /// it without this
        #[arg(short)]
        o: Option<PathBuf>,
        /// A directory, which is the package, or the files of it
        #[arg(required = true)]
        paths: Vec<PathBuf>,
    },
    /// Build many programs side by side, run each, and print how each
    /// ended as one line of JSON
    TestRun {
        /// A file of jobs, one line of JSON each; `-` is standard input
        #[arg(long)]
        manifest: Option<PathBuf>,
        /// Write the results to this file, not to standard output
        #[arg(short)]
        o: Option<PathBuf>,
        /// How many programs are built at a time; the default is one
        /// for each core
        #[arg(short)]
        j: Option<usize>,
        /// How many programs run at a time; the default is one for
        /// each core
        #[arg(long)]
        executors: Option<usize>,
        /// How long a program may run, in milliseconds
        #[arg(long, default_value_t = 60_000)]
        timeout_ms: u64,
        /// How many bytes a program may write to each of its streams
        #[arg(long, default_value_t = 256 << 20)]
        max_output: u64,
        /// Keep what is built and what the programs wrote in this
        /// directory
        #[arg(long)]
        work_dir: Option<PathBuf>,
        /// Put what goes to standard error into standard output, in
        /// the order in which it is written
        #[arg(long)]
        merge_output: bool,
        /// Print the counts and the times of the run to standard error
        #[arg(long)]
        summary: bool,
        /// Build each program as an executable of its own; the
        /// environment variable GOPP_FAST_RUN=0 says the same
        #[arg(long)]
        real: bool,
        /// How much Clang optimizes the programs
        #[arg(long, value_enum, default_value = "0")]
        opt: Opt,
        /// Build the programs with what a debugger reads, as `gopp
        /// build` does
        #[arg(long)]
        debug: bool,
        /// What a panic does in the programs, as for `gopp build`
        #[arg(long, value_enum, default_value = "unwind")]
        panic: PanicMode,
        #[arg(long, hide = true)]
        executor: Option<PathBuf>,
        #[arg(long, hide = true, default_value_t = 10_000)]
        startup_timeout_ms: u64,
        #[arg(long, hide = true)]
        schedule: Option<String>,
        /// Programs: a file or a directory each
        paths: Vec<PathBuf>,
    },
    /// The process that runs the programs of `test-run`
    #[cfg(target_os = "linux")]
    #[command(hide = true)]
    TestExec {
        #[arg(long)]
        runtime: PathBuf,
        #[arg(long)]
        max_output: u64,
    },
    /// Print Go++ symbols as Go++ text
    Demangle {
        /// Cut each name after this many bytes: a type that occurs often
        /// in a symbol is written out each time
        #[arg(long, default_value_t = gopp_mangle::MAX_OUTPUT)]
        max_output: usize,
        /// The symbols; with none, standard input is copied with every
        /// symbol in it replaced
        names: Vec<String>,
    },
    /// Say what an error code means, how to fix it, and show an example
    Explain {
        /// List every code
        #[arg(long)]
        list: bool,
        /// A code such as E0111, or the name of one
        #[arg(required_unless_present = "list", conflicts_with = "list")]
        code: Option<String>,
    },
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum Emit {
    /// The syntax tree, one node per line
    Ast,
    /// The syntax tree as JSON, one file per line
    Json,
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum PanicMode {
    /// A panic makes the deferred calls, and `recover` stops it
    Unwind,
    /// A panic writes its message and ends the program
    Abort,
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum Opt {
    #[value(name = "0")]
    O0,
    #[value(name = "1")]
    O1,
    #[value(name = "2")]
    O2,
    #[value(name = "3")]
    O3,
    #[value(name = "s")]
    Os,
    #[value(name = "z")]
    Oz,
}

impl Opt {
    fn level(self) -> gopp_codegen_c::driver::OptLevel {
        use gopp_codegen_c::driver::OptLevel;
        match self {
            Self::O0 => OptLevel::O0,
            Self::O1 => OptLevel::O1,
            Self::O2 => OptLevel::O2,
            Self::O3 => OptLevel::O3,
            Self::Os => OptLevel::Os,
            Self::Oz => OptLevel::Oz,
        }
    }
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum EmitBuilt {
    /// The module of the package as text
    Gir,
    /// The unit of C that is given to Clang
    C,
    /// An object file
    Obj,
    /// The assembly of the target
    Asm,
    /// An executable
    Exe,
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum EmitChecked {
    /// The tables of the checker, one entry per line
    Info,
}

fn main() -> ExitCode {
    panic::catch_unwind(run).unwrap_or(ExitCode::from(EXIT_INTERNAL))
}

fn run() -> ExitCode {
    let cli = Cli::parse_from(args::normalize(env::args_os()));
    match cli.command {
        Command::Version => version(),
        Command::HostToolchain => host_toolchain(),
        Command::Targets => {
            print!("{}", gopp_target::TargetSpec::table());
            ExitCode::SUCCESS
        }
        Command::Parse {
            emit, batch: true, ..
        } => parse::run_batch(emit),
        Command::Parse { emit, files, .. } => parse::run(&files, emit),
        Command::Fmt { l, d, w, paths } => {
            let mode = fmt::Mode {
                list: l,
                diff: d,
                write: w,
            };
            fmt::run(&paths, mode)
        }
        Command::Check { json, emit, paths } => check::run(&paths, json, emit.is_some()),
        Command::Build {
            target,
            panic,
            no_debug,
            opt,
            pipeline_times,
            trace,
            sysroot,
            crt_dir,
            dynamic_linker,
            emit,
            o,
            paths,
        } => build::run(&build::Options {
            target,
            aborts: panic == PanicMode::Abort,
            debug: !no_debug,
            opt: opt.level(),
            pipeline_times,
            trace,
            sysroot,
            crt_dir,
            dynamic_linker,
            emit,
            output: o,
            paths,
        }),
        Command::TestRun {
            manifest,
            o,
            j,
            executors,
            timeout_ms,
            max_output,
            work_dir,
            merge_output,
            summary,
            real,
            opt,
            debug,
            panic,
            executor,
            startup_timeout_ms,
            schedule,
            paths,
        } => runner::run(&runner::Options {
            paths,
            manifest,
            output: o,
            jobs: j,
            executors,
            timeout_ms,
            max_output,
            work_dir,
            merge_output,
            summary,
            real,
            opt: opt.level(),
            debug,
            aborts: panic == PanicMode::Abort,
            executor,
            startup_timeout_ms,
            schedule,
        }),
        #[cfg(target_os = "linux")]
        Command::TestExec {
            runtime,
            max_output,
        } => runner::exec::run(&runtime, max_output),
        Command::Demangle { max_output, names } => demangle::run(&names, max_output),
        Command::Explain { list: true, .. } => {
            print!("{}", gopp_diag::listed());
            ExitCode::SUCCESS
        }
        Command::Explain { code, .. } => explain(&code.unwrap_or_default()),
    }
}

fn explain(code: &str) -> ExitCode {
    match gopp_diag::explained(code) {
        Ok(text) => {
            print!("{text}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(EXIT_ERRORS)
        }
    }
}

fn exit_code(failed: bool) -> ExitCode {
    if failed {
        ExitCode::from(EXIT_ERRORS)
    } else {
        ExitCode::SUCCESS
    }
}

fn version() -> ExitCode {
    let version = env!("CARGO_PKG_VERSION");
    match option_env!("GOPP_GIT_REVISION") {
        Some(revision) => println!("gopp version {version}+{revision} (clang {CLANG_MAJOR})"),
        None => println!("gopp version {version} (clang {CLANG_MAJOR})"),
    }
    ExitCode::SUCCESS
}

fn host_toolchain() -> ExitCode {
    match HostToolchain::discover() {
        Ok(toolchain) => {
            for (name, tool) in [("clang", toolchain.clang), ("ld.lld", toolchain.lld)] {
                println!("{name}: {} ({})", tool.path.display(), tool.version);
            }
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::from(EXIT_ERRORS)
        }
    }
}
