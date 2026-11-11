//! The `gopp` command.

mod args;

use std::env;
use std::panic;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
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
}

fn main() -> ExitCode {
    panic::catch_unwind(run).unwrap_or(ExitCode::from(EXIT_INTERNAL))
}

fn run() -> ExitCode {
    let cli = Cli::parse_from(args::normalize(env::args_os()));
    match cli.command {
        Command::Version => version(),
        Command::HostToolchain => host_toolchain(),
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
