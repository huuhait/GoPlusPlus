//! The `gopp` command.

mod args;
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
}

#[derive(Clone, Copy, PartialEq, ValueEnum)]
enum Emit {
    /// The syntax tree, one node per line
    Ast,
    /// The syntax tree as JSON, one file per line
    Json,
}

fn main() -> ExitCode {
    panic::catch_unwind(run).unwrap_or(ExitCode::from(EXIT_INTERNAL))
}

fn run() -> ExitCode {
    let cli = Cli::parse_from(args::normalize(env::args_os()));
    match cli.command {
        Command::Version => version(),
        Command::HostToolchain => host_toolchain(),
        Command::Parse {
            emit, batch: true, ..
        } => parse::run_batch(emit),
        Command::Parse { emit, files, .. } => parse::run(&files, emit),
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
