//! The `gopp` command.

mod args;
mod build;
mod check;
mod demangle;
mod diff;
mod fmt;
mod parse;

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
        /// Read standard input as plain Go, not as Go++
        #[arg(long)]
        go: bool,
        /// Files, and directories to format every .go and .gopp file under;
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
        Command::Fmt { l, d, w, go, paths } => {
            let mode = fmt::Mode {
                list: l,
                diff: d,
                write: w,
            };
            fmt::run(&paths, mode, go)
        }
        Command::Check { json, emit, paths } => check::run(&paths, json, emit.is_some()),
        Command::Build {
            target,
            emit,
            o,
            paths,
        } => build::run(&build::Options {
            target,
            emit,
            output: o,
            paths,
        }),
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
