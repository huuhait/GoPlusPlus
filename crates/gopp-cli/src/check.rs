//! `gopp check`.

use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::io::{self, IsTerminal};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::sync::Arc;

use gopp_diag::{Diagnostic, Severity, Sink, render_json, render_text};
use gopp_sema::types::Types;
use gopp_sema::{Package, PackageLoader, Unit, check, info_text};
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_syntax::ast::{DeclKind, ExprKind, File};
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

/// The packages of Go's standard library that are checked, and for each
/// package the paths that its imports stand for.
#[derive(Default)]
pub(crate) struct Standard {
    pub checked: HashMap<String, Arc<Package>>,
}

/// Finds the packages that a package imports. A package of Go's standard
/// library is the one that was checked from its sources; any other is a
/// stand-in whose names are all taken to exist, until packages are built
/// and loaded.
pub(crate) struct Imports<'a> {
    pub standard: &'a HashMap<String, Arc<Package>>,
    /// What an import path stands for in the importing package.
    pub import_map: HashMap<String, String>,
}

impl PackageLoader for Imports<'_> {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        let path = self.import_map.get(path).map_or(path, String::as_str);
        match self.standard.get(path) {
            Some(package) => Ok(Arc::clone(package)),
            None => Ok(Arc::new(Package::fake(path))),
        }
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
    let standard = standard_library(&parsed, &names, &types);
    let results: Vec<(String, Vec<Diagnostic>)> = parsed
        .par_iter()
        .zip(sinks)
        .map(|(package, sink)| {
            let units: Vec<Unit> = package
                .files
                .iter()
                .map(|(file, source)| Unit { file, source })
                .collect();
            let loader = Imports {
                standard: &standard.checked,
                import_map: HashMap::new(),
            };
            let types = types.fork();
            let checked = check(&package.path, &units, &loader, &names, &types, &sink);
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
        let is_source = |file: &PathBuf| {
            let test = file.to_string_lossy().ends_with("_test.go");
            file.is_file() && Dialect::from_path(file).is_some() && !test
        };
        let files: BTreeSet<PathBuf> = entries
            .filter_map(|entry| entry.ok().map(|entry| entry.path()))
            .filter(is_source)
            .collect();
        if files.is_empty() {
            groups.push(Err(format!("{}: no .gopp or .go files", path.display())));
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
    let Some(dialect) = Dialect::from_path(path) else {
        return Err(format!("{}: not a .gopp or .go file", path.display()));
    };
    let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
    SourceFile::from_bytes(path.to_owned(), &bytes, dialect).map_err(|error| error.to_string())
}

/// The import paths that the files write, without their quotes.
fn imports(package: &Parsed) -> impl Iterator<Item = String> + '_ {
    package.files.iter().flat_map(|(file, source)| {
        file.decls.iter().filter_map(move |&decl| {
            let DeclKind::Import { path, .. } = &file.decl_nodes[decl].kind else {
                return None;
            };
            let ExprKind::BasicLit { .. } = file.exprs[*path].kind else {
                return None;
            };
            let span = file.exprs[*path].span;
            let literal = &source.text()[span.lo as usize..span.hi as usize];
            Some(literal.trim_matches(['"', '`']).to_owned())
        })
    })
}

/// Checks the packages of Go's standard library that the packages
/// import, with what those import, from the sources that `go list`
/// names. Without `go`, none is checked.
pub(crate) fn standard_library(
    packages: &[Parsed],
    names: &ThreadedRodeo,
    types: &Types,
) -> Standard {
    let mut standard = Standard::default();
    // A path whose first element has a dot is a module, one in angle
    // brackets or with a header extension is a header.
    let is_standard = |path: &String| {
        let first = path.split('/').next().unwrap_or_default();
        let header = [".h", ".hh", ".hpp", ".hxx"];
        !path.is_empty()
            && !first.contains('.')
            && !path.starts_with('<')
            && !header.iter().any(|extension| path.ends_with(extension))
    };
    let wanted: BTreeSet<String> = packages
        .iter()
        .flat_map(imports)
        .filter(is_standard)
        .collect();
    if wanted.is_empty() {
        return standard;
    }
    let format = "{{.ImportPath}}\t{{.Dir}}\t{{join .GoFiles \",\"}}\t\
                  {{range $from, $to := .ImportMap}}{{$from}}={{$to}},{{end}}";
    let listed = Command::new("go")
        .args(["list", "-e", "-deps", "-f", format])
        .args(&wanted)
        .env("CGO_ENABLED", "0")
        .output();
    let Ok(listed) = listed else {
        return standard;
    };
    // The packages come after those that they import.
    for line in String::from_utf8_lossy(&listed.stdout).lines() {
        let fields: Vec<&str> = line.split('\t').collect();
        let [path, dir, files, mapped] = fields[..] else {
            continue;
        };
        if dir.is_empty() || files.is_empty() {
            continue;
        }
        let import_map = mapped
            .split_terminator(',')
            .filter_map(|pair| pair.split_once('='))
            .map(|(from, to)| (from.to_owned(), to.to_owned()))
            .collect();
        let sources = SourceMap::new();
        // What Go's own packages are told is not shown.
        let sink = Sink::new(MAX_DIAGNOSTICS);
        let mut parsed = Vec::new();
        for name in files.split_terminator(',') {
            let Ok(source) = load(&Path::new(dir).join(name)) else {
                continue;
            };
            let id = sources.add(source);
            let source = sources.file(id);
            parsed.push((parse(id, &source, names, &sink), source));
        }
        let units: Vec<Unit> = parsed
            .iter()
            .map(|(file, source)| Unit { file, source })
            .collect();
        let loader = Imports {
            standard: &standard.checked,
            import_map,
        };
        let package = check(path, &units, &loader, names, types, &sink);
        standard.checked.insert(path.to_owned(), Arc::new(package));
    }
    standard
}
