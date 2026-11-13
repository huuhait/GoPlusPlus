//! `gopp parse`.

use std::fs;
use std::io::{self, IsTerminal, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use gopp_diag::{Severity, Sink, render_text};
use gopp_span::{SourceFile, SourceMap, is_source_path};
use gopp_syntax::ast::{dump, dump_json};
use gopp_syntax::parser::parse;
use lasso::ThreadedRodeo;
use rayon::prelude::*;

use crate::{EXIT_ERRORS, Emit, exit_code};

/// The errors after the tenth are rarely worth reading.
pub const MAX_DIAGNOSTICS: usize = 10;

/// What parsing one file printed, and whether it failed.
struct Parsed {
    failed: bool,
    stdout: String,
    stderr: String,
}

pub fn run(files: &[PathBuf], emit: Option<Emit>) -> ExitCode {
    let color = io::stderr().is_terminal();
    let mut failed = false;
    for path in files {
        let parsed = parse_file(path, emit, color);
        print!("{}", parsed.stdout);
        eprint!("{}", parsed.stderr);
        failed |= parsed.failed;
    }
    exit_code(failed)
}

/// Parses the files named on standard input, one path per line, on all
/// cores. For each, in input order, it writes one record to standard
/// output: the line `gopp-batch <exit> <stdout_len> <stderr_len> <path>`,
/// then that many bytes of what `gopp parse <path>` prints to standard
/// output, then of what it prints to standard error.
pub fn run_batch(emit: Option<Emit>) -> ExitCode {
    let paths: Vec<String> = match io::stdin().lines().collect() {
        Ok(paths) => paths,
        Err(error) => {
            eprintln!("error: reading the list of files: {error}");
            return ExitCode::from(EXIT_ERRORS);
        }
    };
    let paths: Vec<&String> = paths.iter().filter(|path| !path.is_empty()).collect();
    let results: Vec<Parsed> = paths
        .par_iter()
        .map(|path| parse_file(Path::new(path), emit, false))
        .collect();
    let mut out = io::stdout().lock();
    let mut failed = false;
    for (path, parsed) in paths.iter().zip(&results) {
        failed |= parsed.failed;
        let exit = if parsed.failed { EXIT_ERRORS } else { 0 };
        let (stdout, stderr) = (&parsed.stdout, &parsed.stderr);
        let header = format!(
            "gopp-batch {exit} {} {} {path}\n",
            stdout.len(),
            stderr.len()
        );
        let record = [header.as_bytes(), stdout.as_bytes(), stderr.as_bytes()].concat();
        if let Err(error) = out.write_all(&record) {
            eprintln!("error: writing the records: {error}");
            return ExitCode::from(EXIT_ERRORS);
        }
    }
    exit_code(failed)
}

fn parse_file(path: &Path, emit: Option<Emit>, color: bool) -> Parsed {
    let source = match load(path) {
        Ok(source) => source,
        Err(message) => {
            return Parsed {
                failed: true,
                stdout: String::new(),
                stderr: format!("error: {message}\n"),
            };
        }
    };
    let sources = SourceMap::new();
    let names = ThreadedRodeo::new();
    let id = sources.add(source);
    let source = sources.file(id);
    let sink = Sink::new(MAX_DIAGNOSTICS);
    let file = parse(id, &source, &names, &sink);
    let mut parsed = Parsed {
        failed: false,
        stdout: String::new(),
        stderr: String::new(),
    };
    for diagnostic in sink.finish() {
        parsed.failed |= diagnostic.code.severity() == Severity::Error;
        parsed.stderr += &render_text(&diagnostic, &sources, color);
    }
    match emit {
        Some(Emit::Ast) => parsed.stdout = dump(&file, source.text(), &names),
        Some(Emit::Json) => parsed.stdout = dump_json(&file, source.text(), &names),
        None => {}
    }
    parsed
}

fn load(path: &Path) -> Result<SourceFile, String> {
    if !is_source_path(path) {
        return Err(format!("{}: not a .gopp file", path.display()));
    }
    let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
    SourceFile::from_bytes(path.to_owned(), &bytes).map_err(|error| error.to_string())
}
