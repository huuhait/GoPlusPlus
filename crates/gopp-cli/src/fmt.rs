//! `gopp fmt`.

use std::fs;
use std::io::{self, IsTerminal, Read, Write};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use gopp_diag::{Severity, Sink, render_text};
use gopp_span::{SourceFile, SourceMap, is_source_path};
use gopp_syntax::fmt::format;
use gopp_syntax::parser::parse;
use lasso::ThreadedRodeo;
use rayon::prelude::*;

use crate::diff;
use crate::parse::MAX_DIAGNOSTICS;
use crate::{EXIT_ERRORS, exit_code};

/// What to do with a formatted file; with none set, it is printed.
#[derive(Clone, Copy)]
pub struct Mode {
    pub list: bool,
    pub diff: bool,
    pub write: bool,
}

/// What formatting one file printed, and whether it failed.
#[derive(Default)]
struct Formatted {
    failed: bool,
    stdout: String,
    stderr: String,
}

impl Formatted {
    fn error(message: impl std::fmt::Display) -> Self {
        Self {
            failed: true,
            stdout: String::new(),
            stderr: format!("error: {message}\n"),
        }
    }
}

pub fn run(paths: &[PathBuf], mode: Mode) -> ExitCode {
    let color = io::stderr().is_terminal();
    if paths.is_empty() {
        if mode.write {
            eprintln!("error: cannot use -w with standard input");
            return ExitCode::from(EXIT_ERRORS);
        }
        let mut bytes = Vec::new();
        if let Err(error) = io::stdin().read_to_end(&mut bytes) {
            eprintln!("error: reading standard input: {error}");
            return ExitCode::from(EXIT_ERRORS);
        }
        let formatted = format_bytes(Path::new("<standard input>"), &bytes, mode, color);
        return report(&[formatted]);
    }

    let mut files = Vec::new();
    let mut results = Vec::new();
    for path in paths {
        if path.is_dir() {
            if let Err(error) = source_files_under(path, &mut files) {
                results.push(Formatted::error(format_args!(
                    "{}: {error}",
                    path.display()
                )));
            }
        } else {
            files.push(path.clone());
        }
    }
    let formatted = files.par_iter().map(|path| format_file(path, mode, color));
    results.extend(formatted.collect::<Vec<_>>());
    report(&results)
}

fn report(results: &[Formatted]) -> ExitCode {
    let mut out = io::stdout().lock();
    let mut failed = false;
    for formatted in results {
        failed |= formatted.failed;
        if let Err(error) = out.write_all(formatted.stdout.as_bytes()) {
            eprintln!("error: writing the result: {error}");
            return ExitCode::from(EXIT_ERRORS);
        }
        eprint!("{}", formatted.stderr);
    }
    exit_code(failed)
}

/// The `.gopp` files under `dir`, sorted by name within each
/// directory. Only files whose name starts with a dot are left out.
fn source_files_under(dir: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    let mut entries: Vec<PathBuf> = fs::read_dir(dir)?
        .map(|entry| entry.map(|entry| entry.path()))
        .collect::<io::Result<_>>()?;
    entries.sort();
    for path in entries {
        let hidden = path
            .file_name()
            .is_some_and(|name| name.as_encoded_bytes().starts_with(b"."));
        if path.is_dir() {
            source_files_under(&path, files)?;
        } else if !hidden && is_source_path(&path) {
            files.push(path);
        }
    }
    Ok(())
}

fn format_file(path: &Path, mode: Mode, color: bool) -> Formatted {
    if !is_source_path(path) {
        return Formatted::error(format_args!("{}: not a .gopp file", path.display()));
    }
    match fs::read(path) {
        Ok(bytes) => format_bytes(path, &bytes, mode, color),
        Err(error) => Formatted::error(format_args!("{}: {error}", path.display())),
    }
}

fn format_bytes(path: &Path, bytes: &[u8], mode: Mode, color: bool) -> Formatted {
    let source = match SourceFile::from_bytes(path.to_owned(), bytes) {
        Ok(source) => source,
        Err(error) => return Formatted::error(error),
    };
    let sources = SourceMap::new();
    let names = ThreadedRodeo::new();
    let id = sources.add(source);
    let source = sources.file(id);
    let sink = Sink::new(MAX_DIAGNOSTICS);
    let file = parse(id, &source, &names, &sink);
    let mut formatted = Formatted::default();
    for diagnostic in sink.finish() {
        formatted.failed |= diagnostic.code.severity() == Severity::Error;
        formatted.stderr += &render_text(&diagnostic, &sources, color);
    }
    // A file with a syntax error is left as it is.
    if formatted.failed {
        return formatted;
    }

    let text = format(&file, &source, &names);
    let name = path.display().to_string();
    if text.as_bytes() != bytes {
        if mode.list {
            formatted.stdout += &format!("{name}\n");
        }
        if mode.write
            && let Err(error) = fs::write(path, &text)
        {
            return Formatted::error(format_args!("{name}: {error}"));
        }
        if mode.diff {
            let old = String::from_utf8_lossy(bytes);
            formatted.stdout += &diff::unified(&format!("{name}.orig"), &old, &name, &text);
        }
    }
    if !mode.list && !mode.write && !mode.diff {
        formatted.stdout = text;
    }
    formatted
}
