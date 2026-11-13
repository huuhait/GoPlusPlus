//! `gopp check`.

use std::collections::BTreeSet;
use std::fs;
use std::io::{self, IsTerminal};
use std::path::PathBuf;
use std::process::ExitCode;

use gopp_diag::{Diagnostic, Severity, Sink, render_json, render_text};
use gopp_sema::info_text;
use gopp_sema::types::Types;
use gopp_span::{SourceFile, SourceMap, is_source_path};
use gopp_syntax::ast::File;
use lasso::ThreadedRodeo;
use rayon::prelude::*;

use crate::parse::MAX_DIAGNOSTICS;
use crate::{EXIT_ERRORS, exit_code, packages};

/// With `--json` every diagnostic is wanted.
const MAX_JSON_DIAGNOSTICS: usize = 10_000;

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
    let target = match packages::target(None) {
        Ok(target) => target,
        Err(error) => {
            eprintln!("error: {error}");
            return ExitCode::from(EXIT_ERRORS);
        }
    };
    let (roots, cx) = (packages::roots(), packages::build_context(target));
    // Each package is loaded and checked on its own, with the packages
    // that it imports and a copy of the type table to itself; what it
    // says is printed in the order of the arguments.
    let types = Types::new(&names);
    let checked = |(name, files): &(String, Vec<PathBuf>)| {
        let sink = Sink::new(cap);
        let path = packages::import_path(name, files, &roots);
        let graph = gopp_build::load((&path, files), &roots, &cx, &sources, &names, &sink)?;
        let types = types.fork();
        let all = packages::checked(graph, &names, &types, &sink);
        let mut text = String::new();
        let last = all.len() - 1;
        let pairs: Vec<Vec<(&File, &SourceFile)>> = (all.iter())
            .map(|(parsed, _)| (parsed.files.iter().map(|(f, s)| (f, &**s))).collect())
            .collect();
        let needs = packages::imported(&all);
        for (at, (_, package)) in all.iter().enumerate() {
            // The instances of the generic code of a package without an
            // error are held to their bounds, as a build holds them:
            // those of the generic code of the packages that it imports
            // too.
            if sink.is_empty() {
                let imports: Vec<gopp_gir::Imported> = (needs[at].iter())
                    .map(|&import| gopp_gir::Imported {
                        package: &all[import].1,
                        files: &pairs[import],
                    })
                    .collect();
                let accepted = gopp_gir::Checked {
                    package,
                    files: &pairs[at],
                    imports: &imports,
                    types: &types,
                    names: &names,
                    aborts: false,
                };
                gopp_gir::check_instances(&accepted, &sink);
            }
            if info && at == last {
                text = info_text(package, &pairs[at], &types, &names);
            }
        }
        Ok((text, sink.finish()))
    };
    let results: Result<Vec<(String, Vec<Diagnostic>)>, String> =
        groups.par_iter().map(checked).collect();
    let results = match results {
        Ok(results) => results,
        Err(message) => {
            eprintln!("error: {message}");
            return ExitCode::from(EXIT_ERRORS);
        }
    };
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
