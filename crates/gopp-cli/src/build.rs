//! `gopp build`.

use std::env;
use std::ffi::OsString;
use std::fs;
use std::io::{self, IsTerminal, Write};
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

use gopp_codegen_c::driver::{ClangDriver, OptLevel, Output};
use gopp_codegen_c::runtime::{self, Form};
use gopp_codegen_c::toolchain::HostToolchain;
use gopp_codegen_c::{EmitOptions, Entry, emit_c23};
use gopp_diag::{Code, Diagnostic, Severity, Sink, render_text};
use gopp_gir::{Checked, Module, Summaries, lower_with, print_module};
use gopp_sema::Package;
use gopp_sema::types::Types;
use gopp_span::{SourceFile, SourceMap};
use gopp_syntax::ast::File;
use gopp_target::{Os, TargetSpec};
use lasso::ThreadedRodeo;
use rayon::prelude::*;

use crate::check::grouped;
use crate::parse::MAX_DIAGNOSTICS;
use crate::{EXIT_ERRORS, EmitBuilt, packages};

pub struct Options {
    pub target: Option<String>,
    /// A panic ends the program at once (`-panic=abort`).
    pub aborts: bool,
    pub emit: EmitBuilt,
    pub output: Option<PathBuf>,
    pub paths: Vec<PathBuf>,
}

/// Says what stopped the build and gives the status of a failure.
fn stopped(message: impl std::fmt::Display) -> ExitCode {
    eprintln!("error: {message}");
    ExitCode::from(EXIT_ERRORS)
}

/// A fault of the code generator, with its code.
fn codegen(message: impl std::fmt::Display) -> ExitCode {
    eprintln!("error[{}]: {message}", Code::Codegen);
    ExitCode::from(EXIT_ERRORS)
}

/// Adds the diagnostics to `said` as they are printed, and says whether
/// one of them is an error.
fn reported(
    diagnostics: Vec<Diagnostic>,
    sources: &SourceMap,
    color: bool,
    said: &mut String,
) -> bool {
    let mut failed = false;
    for diagnostic in diagnostics {
        failed |= diagnostic.code.severity() == Severity::Error;
        said.push_str(&render_text(&diagnostic, sources, color));
    }
    failed
}

/// A package that was checked and lowered, with those that it imports.
pub(crate) struct Lowered {
    pub module: Module,
    /// Whether it is the package `main`.
    pub is_main: bool,
    /// What the package is called for what is built of it: its
    /// directory, or its first file without the extension.
    pub name: OsString,
    /// The packages that it needs, each after those that it imports,
    /// with their import paths.
    pub imports: Vec<(String, Module)>,
}

/// Checks and lowers the one package that `paths` name, and the
/// packages that it imports, for the target. What is said about them is
/// added to `said` as it is printed; there is no package if an error is
/// among it.
pub(crate) fn lowered(
    paths: &[PathBuf],
    (target, aborts): (&'static TargetSpec, bool),
    color: bool,
    said: &mut String,
) -> Option<Lowered> {
    let mut stopped = |message: &dyn std::fmt::Display| {
        said.push_str(&format!("error: {message}\n"));
        None
    };
    let mut groups = grouped(paths);
    let (name, files) = match (groups.pop(), groups.is_empty()) {
        (Some(Ok(group)), true) => group,
        (Some(Err(message)), true) => return stopped(&message),
        _ => return stopped(&"gopp build takes one package: a directory, or files"),
    };
    let sources = SourceMap::new();
    let names = ThreadedRodeo::new();
    let sink = Sink::new(MAX_DIAGNOSTICS);
    let (roots, cx) = (packages::roots(), packages::build_context(target));
    let path = packages::import_path(&name, &files, &roots);
    let graph = match gopp_build::load((&path, &files), &roots, &cx, &sources, &names, &sink) {
        Ok(graph) => graph,
        Err(message) => return stopped(&message),
    };
    let types = Types::new(&names);
    let all = packages::checked(graph, &names, &types, &sink);
    if reported(sink.finish(), &sources, color, said) {
        return None;
    }
    // A package is lowered with what is known of the functions of those
    // that it imports, so the packages that import nothing come first,
    // side by side, then those that import only them, and so on. What is
    // said about each is printed in the order of the packages.
    let lower = |(parsed, package): &(gopp_build::Parsed, Arc<Package>), known: &Summaries| {
        let pairs: Vec<(&File, &SourceFile)> = parsed
            .files
            .iter()
            .map(|(file, source)| (file, &**source))
            .collect();
        let checked = Checked {
            package,
            files: &pairs,
            types: &types,
            names: &names,
            aborts,
        };
        let sink = Sink::new(MAX_DIAGNOSTICS);
        let (module, own) = lower_with(&checked, known, target.layout(), &sink);
        (module, own, sink.finish())
    };
    let mut level = vec![0; all.len()];
    for at in 0..all.len() {
        let imported = all[at].0.imports.iter();
        let before =
            imported.filter_map(|path| all[..at].iter().position(|(p, _)| p.path == *path));
        level[at] = before.map(|import| level[import] + 1).max().unwrap_or(0);
    }
    let mut made: Vec<Option<(Module, Vec<Diagnostic>)>> = all.iter().map(|_| None).collect();
    let mut known = Summaries::default();
    for now in 0..=level.iter().copied().max().unwrap_or(0) {
        let of_level: Vec<usize> = (0..all.len()).filter(|&at| level[at] == now).collect();
        let lowered: Vec<_> = of_level
            .par_iter()
            .map(|&at| lower(&all[at], &known))
            .collect();
        for (at, (module, own, diagnostics)) in of_level.into_iter().zip(lowered) {
            known.extend(&own);
            made[at] = Some((module, diagnostics));
        }
    }
    let mut failed = false;
    let mut modules = Vec::with_capacity(made.len());
    for ((module, diagnostics), (parsed, _)) in made.into_iter().flatten().zip(&all) {
        failed |= reported(diagnostics, &sources, color, said);
        modules.push((parsed.path.clone(), module));
    }
    let (_, module) = modules.pop()?;
    if failed {
        return None;
    }
    let first = files.first().and_then(|file| file.file_stem());
    let name = match name.as_str() {
        "command-line-arguments" => first.map_or("a".into(), |stem| stem.to_os_string()),
        name => name.into(),
    };
    Some(Lowered {
        module,
        is_main: all
            .last()
            .is_some_and(|(_, package)| package.name == "main"),
        name,
        imports: modules,
    })
}

pub fn run(options: &Options) -> ExitCode {
    let selected = packages::target(options.target.as_deref());
    let target = match selected.and_then(TargetSpec::in_this_version) {
        Ok(target) => target,
        Err(error) => return stopped(error),
    };
    let mut said = String::new();
    let lowered = lowered(
        &options.paths,
        (target, options.aborts),
        io::stderr().is_terminal(),
        &mut said,
    );
    eprint!("{said}");
    let Some(Lowered {
        module,
        is_main,
        name,
        imports,
    }) = lowered
    else {
        return ExitCode::from(EXIT_ERRORS);
    };
    if options.emit == EmitBuilt::Gir {
        return written(options.output.as_ref(), &print_module(&module));
    }
    let paths: Vec<String> = imports.iter().map(|(path, _)| path.clone()).collect();
    let emit = EmitOptions {
        layout: target.layout(),
        // A package that is no program is built as a program takes it.
        entry: match is_main {
            true => Entry::Main,
            false => Entry::Package,
        },
        imports: &paths,
        swiftcall: target.swiftcall,
        aborts: options.aborts,
    };
    let unit = match emit_c23(&module, &emit) {
        Ok(unit) => unit,
        Err(error) => return codegen(error),
    };
    let (kind, extension) = match (options.emit, target.os) {
        (EmitBuilt::Gir | EmitBuilt::C, _) => return written(options.output.as_ref(), &unit),
        (EmitBuilt::Obj, Os::Windows) => (Output::Object, "obj"),
        (EmitBuilt::Obj, _) => (Output::Object, "o"),
        (EmitBuilt::Asm, _) => (Output::Assembly, "s"),
        (EmitBuilt::Exe, Os::Windows) => (Output::Executable, "exe"),
        (EmitBuilt::Exe, _) => (Output::Executable, ""),
    };
    // Without `-o`: named after the directory, or after the first file.
    let output = options
        .output
        .clone()
        .unwrap_or_else(|| PathBuf::from(name).with_extension(extension));
    let toolchain = match HostToolchain::discover() {
        Ok(toolchain) => toolchain,
        Err(error) => return stopped(error),
    };
    let dir = match tempfile::tempdir() {
        Ok(dir) => dir,
        Err(error) => return stopped(format!("no directory for the unit: {error}")),
    };
    let driver = ClangDriver {
        toolchain: &toolchain,
        target,
        opt: OptLevel::default(),
        strict: false,
    };
    // An executable is linked with the runtime, which is built once for
    // a target and its flags.
    let runtime = match kind {
        Output::Executable => {
            let cache = match cache_dir() {
                Ok(cache) => cache,
                Err(message) => return stopped(message),
            };
            let checked = driver.check_link_inputs();
            match checked.and_then(|()| runtime::build(&driver, Form::Objects, &cache)) {
                Ok(runtime) => runtime,
                Err(error) => return codegen(error),
            }
        }
        _ => Vec::new(),
    };
    // An executable is linked with the packages that it imports too,
    // each a unit of its own.
    let mut link_with = Vec::new();
    if kind == Output::Executable {
        let units = match imported_units(&imports, &emit, dir.path()) {
            Ok(units) => units,
            Err(message) => return codegen(message),
        };
        link_with = units;
    }
    link_with.extend(runtime);
    match driver.build(&unit, dir.path(), &output, kind, &link_with) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => codegen(error),
    }
}

/// Writes the unit of C of each imported package to a file of `dir`,
/// side by side: `import<n>.c`, for the driver to compile with the unit
/// of the program.
pub(crate) fn imported_units(
    imports: &[(String, Module)],
    of_program: &EmitOptions,
    dir: &std::path::Path,
) -> Result<Vec<PathBuf>, String> {
    let options = EmitOptions {
        entry: Entry::Package,
        imports: &[],
        aborts: false,
        ..*of_program
    };
    let write = |(at, (_, module)): (usize, &(String, Module))| {
        let unit = emit_c23(module, &options).map_err(|error| error.to_string())?;
        let file = dir.join(format!("import{at}.c"));
        fs::write(&file, unit).map_err(|error| format!("{}: {error}", file.display()))?;
        Ok(file)
    };
    imports.par_iter().enumerate().map(write).collect()
}

/// Where what is built once is kept: what `GOPPCACHE` says, or the
/// folder of the user for such things.
pub(crate) fn cache_dir() -> Result<PathBuf, String> {
    let set = |name: &str| {
        let value = env::var_os(name).filter(|value| !value.is_empty());
        value.map(PathBuf::from)
    };
    let of_user = match cfg!(windows) {
        true => set("LOCALAPPDATA"),
        false => set("XDG_CACHE_HOME").or_else(|| set("HOME").map(|home| home.join(".cache"))),
    };
    let dir = set("GOPPCACHE").or_else(|| of_user.map(|dir| dir.join("gopp")));
    let dir = dir.ok_or("no folder for the cache: GOPPCACHE says where it is")?;
    std::path::absolute(&dir).map_err(|error| format!("{}: {error}", dir.display()))
}

/// Writes the text to the file, or to standard output without one.
fn written(output: Option<&PathBuf>, text: &str) -> ExitCode {
    let result = match output {
        Some(path) => fs::write(path, text).map_err(|error| format!("{}: {error}", path.display())),
        None => io::stdout()
            .write_all(text.as_bytes())
            .map_err(|error| error.to_string()),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => stopped(message),
    }
}
