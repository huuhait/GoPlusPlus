//! `gopp check`.

use std::collections::BTreeSet;
use std::fs;
use std::io::{self, IsTerminal};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;

use gopp_diag::{Diagnostic, Severity, Sink, render_json, render_text};
use gopp_sema::types::Types;
use gopp_sema::{Package, PackageLoader, Unit, check, info_text};
use gopp_span::{SourceFile, SourceMap, is_source_path};
use gopp_syntax::ast::File;
use gopp_syntax::parser::parse;
use lasso::ThreadedRodeo;
use rayon::prelude::*;

use crate::parse::MAX_DIAGNOSTICS;
use crate::{EXIT_ERRORS, exit_code};

/// With `--json` every diagnostic is wanted.
const MAX_JSON_DIAGNOSTICS: usize = 10_000;

/// The files of one package, parsed.
pub(crate) struct Parsed {
    pub path: String,
    pub files: Vec<(File, Arc<SourceFile>)>,
}

/// Finds the packages that a package imports: each is a stand-in whose
/// names are all taken to exist, until packages are built and loaded.
pub(crate) struct Imports;

impl PackageLoader for Imports {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        Ok(Arc::new(Package::fake(path)))
    }
}

pub fn run(paths: &[PathBuf], json: bool, info: bool) -> ExitCode {
    let sources = SourceMap::new();
    let names = ThreadedRodeo::new();
    let cap = if json {
        MAX_JSON_DIAGNOSTICS
    } else {
        MAX_DIAGNOSTICS
    };
    let mut groups = Vec::new();
    for group in grouped(paths) {
        match group {
            Ok(group) => groups.push(group),
            Err(message) => {
                eprintln!("error: {message}");
                return ExitCode::from(EXIT_ERRORS);
            }
        }
    }
    // Each package is parsed and checked on its own, with a copy of the
    // type table to itself; what it says is printed in the order of the
    // arguments.
    let sinks: Vec<Sink> = groups.iter().map(|_| Sink::new(cap)).collect();
    let loaded: Vec<Result<Parsed, String>> = groups
        .par_iter()
        .zip(&sinks)
        .map(|((path, files), sink)| parse_package(path, files, &sources, &names, sink))
        .collect();
    let mut parsed = Vec::new();
    for package in loaded {
        match package {
            Ok(package) => parsed.push(package),
            Err(message) => {
                eprintln!("error: {message}");
                return ExitCode::from(EXIT_ERRORS);
            }
        }
    }
    let types = Types::new(&names);
    let results: Vec<(String, Vec<Diagnostic>)> = parsed
        .par_iter()
        .zip(sinks)
        .map(|(package, sink)| {
            let units: Vec<Unit> = package
                .files
                .iter()
                .map(|(file, source)| Unit { file, source })
                .collect();
            let types = types.fork();
            let checked = check(&package.path, &units, &Imports, &names, &types, &sink);
            let files: Vec<(&File, &SourceFile)> = package
                .files
                .iter()
                .map(|(file, source)| (file, &**source))
                .collect();
            // The instances of the generic code of a package without an
            // error are held to their bounds, as a build holds them.
            if sink.is_empty() {
                let accepted = gopp_gir::Checked {
                    package: &checked,
                    files: &files,
                    types: &types,
                    names: &names,
                    aborts: false,
                };
                gopp_gir::check_instances(&accepted, &sink);
            }
            let text = match info {
                true => info_text(&checked, &files, &types, &names),
                false => String::new(),
            };
            (text, sink.finish())
        })
        .collect();
    let color = io::stderr().is_terminal();
    let mut failed = false;
    for (text, diagnostics) in results {
        print!("{text}");
        for diagnostic in diagnostics {
            failed |= diagnostic.code.severity() == Severity::Error;
            match json {
                true => println!("{}", render_json(&diagnostic, &sources)),
                false => eprint!("{}", render_text(&diagnostic, &sources, color)),
            }
        }
    }
    exit_code(failed)
}

/// The packages that the arguments name: each directory is one, with the
/// source files in it; the files that are named are one together.
pub(crate) fn grouped(paths: &[PathBuf]) -> Vec<Result<(String, Vec<PathBuf>), String>> {
    let mut groups = Vec::new();
    let mut loose = Vec::new();
    for path in paths {
        if !path.is_dir() {
            loose.push(path.clone());
            continue;
        }
        let entries = match fs::read_dir(path) {
            Ok(entries) => entries,
            Err(error) => {
                groups.push(Err(format!("{}: {error}", path.display())));
                continue;
            }
        };
        let is_source = |file: &PathBuf| file.is_file() && is_source_path(file);
        let files: BTreeSet<PathBuf> = entries
            .filter_map(|entry| entry.ok().map(|entry| entry.path()))
            .filter(is_source)
            .collect();
        if files.is_empty() {
            groups.push(Err(format!("{}: no .gopp files", path.display())));
            continue;
        }
        let name = path.file_name().map_or_else(
            || path.display().to_string(),
            |name| name.to_string_lossy().into_owned(),
        );
        groups.push(Ok((name, files.into_iter().collect())));
    }
    if !loose.is_empty() {
        groups.push(Ok(("command-line-arguments".to_owned(), loose)));
    }
    groups
}

pub(crate) fn parse_package(
    path: &str,
    files: &[PathBuf],
    sources: &SourceMap,
    names: &ThreadedRodeo,
    sink: &Sink,
) -> Result<Parsed, String> {
    let mut parsed = Vec::new();
    for file in files {
        let source = load(file)?;
        let id = sources.add(source);
        let source = sources.file(id);
        parsed.push((parse(id, &source, names, sink), source));
    }
    Ok(Parsed {
        path: path.to_owned(),
        files: parsed,
    })
}

fn load(path: &Path) -> Result<SourceFile, String> {
    if !is_source_path(path) {
        return Err(format!("{}: not a .gopp file", path.display()));
    }
    let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
    SourceFile::from_bytes(path.to_owned(), &bytes).map_err(|error| error.to_string())
}
