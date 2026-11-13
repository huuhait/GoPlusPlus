//! The packages that a package needs: found from its imports, parsed, and
//! put in an order in which each comes after those that it imports.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{SourceFile, SourceMap, Span, is_source_path};
use gopp_syntax::ast::{DeclKind, ExprKind, File};
use gopp_syntax::constraint::{self, BuildContext, is_test_file};
use gopp_syntax::parser::parse;
use lasso::ThreadedRodeo;
use rayon::prelude::*;
use rustc_hash::FxHashMap;

/// Where the packages of an import path are looked for.
pub struct Roots {
    /// The directory of the standard library: `fmt` is `<std>/fmt`.
    pub std: PathBuf,
}

impl Roots {
    /// The directory of the package at the import path, if a root has
    /// one. A header is no package.
    fn dir(&self, path: &str) -> Option<PathBuf> {
        let header = [".h", ".hh", ".hpp", ".hxx"];
        let plain = |part: &str| !part.is_empty() && part != "." && part != "..";
        if path.starts_with('<')
            || header.iter().any(|extension| path.ends_with(extension))
            || !path.split('/').all(plain)
        {
            return None;
        }
        let dir = self.std.join(path);
        dir.is_dir().then_some(dir)
    }

    /// The import path of the package in the directory, if it is under a
    /// root.
    pub fn path_of(&self, dir: &Path) -> Option<String> {
        let (dir, std) = (
            fs::canonicalize(dir).ok()?,
            fs::canonicalize(&self.std).ok()?,
        );
        let inside = dir.strip_prefix(std).ok()?;
        let parts: Option<Vec<&str>> = inside.iter().map(|part| part.to_str()).collect();
        parts
            .filter(|parts| !parts.is_empty())
            .map(|parts| parts.join("/"))
    }
}

/// The files of one package, parsed.
pub struct Parsed {
    /// The import path.
    pub path: String,
    pub files: Vec<(File, Arc<SourceFile>)>,
    /// The packages that the files import and that a root has, each
    /// once, in the order of the source.
    pub imports: Vec<String>,
    /// Where each of those is first imported.
    spans: Vec<Span>,
}

/// A package with every package that it needs.
pub struct Graph {
    /// Each package after those that it imports; the last is the one
    /// that was asked for.
    pub packages: Vec<Parsed>,
}

/// The import paths that the file writes, without their quotes.
fn imports(file: &File, source: &SourceFile) -> Vec<(String, Span)> {
    let written = file.decls.iter().filter_map(|&decl| {
        let DeclKind::Import { path, .. } = &file.decl_nodes[decl].kind else {
            return None;
        };
        let ExprKind::BasicLit { .. } = file.exprs[*path].kind else {
            return None;
        };
        let span = file.exprs[*path].span;
        let literal = &source.text()[span.lo as usize..span.hi as usize];
        Some((literal.trim_matches(['"', '`']).to_owned(), span))
    });
    written.collect()
}

/// The source files of the package in the directory that the build
/// takes by their names, sorted.
fn files_in(dir: &Path, cx: &BuildContext) -> Result<Vec<PathBuf>, String> {
    let failed = |error: std::io::Error| format!("{}: {error}", dir.display());
    let mut files = Vec::new();
    for entry in fs::read_dir(dir).map_err(failed)? {
        let path = entry.map_err(failed)?.path();
        let Some(name) = path.file_name().and_then(|name| name.to_str()) else {
            continue;
        };
        let hidden = name.starts_with(['.', '_']);
        if path.is_file()
            && is_source_path(&path)
            && !hidden
            && !is_test_file(name)
            && cx.takes_file_name(name)
        {
            files.push(path);
        }
    }
    files.sort();
    Ok(files)
}

/// What one load shares.
struct Loader<'a> {
    roots: &'a Roots,
    cx: &'a BuildContext,
    sources: &'a SourceMap,
    names: &'a ThreadedRodeo,
    sink: &'a Sink,
}

impl Loader<'_> {
    /// Reads the files side by side, adds them to the sources in their
    /// order, and parses them side by side: a file has the same number
    /// however many threads there are. A file whose build condition does
    /// not hold is left out, and so are the declarations of the others
    /// whose conditions do not hold.
    fn parsed(&self, path: &str, files: &[PathBuf]) -> Result<Parsed, String> {
        let read = |file: &PathBuf| {
            if !is_source_path(file) {
                return Err(format!("{}: not a .gopp file", file.display()));
            }
            let bytes = fs::read(file).map_err(|error| format!("{}: {error}", file.display()))?;
            SourceFile::from_bytes(file.clone(), &bytes).map_err(|error| error.to_string())
        };
        let texts: Result<Vec<SourceFile>, String> = files.par_iter().map(read).collect();
        let ids: Vec<_> = texts?
            .into_iter()
            .map(|source| self.sources.add(source))
            .collect();
        let parse_one = |&id| {
            let source = self.sources.file(id);
            let mut file = parse(id, &source, self.names, self.sink);
            constraint::apply(&mut file, &source, self.cx, self.sink).then_some((file, source))
        };
        let files: Vec<(File, Arc<SourceFile>)> = ids.par_iter().filter_map(parse_one).collect();
        let (mut imports, mut spans) = (Vec::new(), Vec::new());
        for (file, source) in &files {
            for (import, span) in self::imports(file, source) {
                if self.roots.dir(&import).is_some() && !imports.contains(&import) {
                    imports.push(import);
                    spans.push(span);
                }
            }
        }
        Ok(Parsed {
            path: path.to_owned(),
            files,
            imports,
            spans,
        })
    }

    /// The package of a root at the import path.
    fn imported(&self, path: &str) -> Result<Parsed, String> {
        let Some(dir) = self.roots.dir(path) else {
            return Err(format!("{path}: no such package"));
        };
        let files = files_in(&dir, self.cx)?;
        self.parsed(path, &files)
    }
}

/// Loads the package of the files, which has the import path `path`,
/// with every package that it needs. An import that no root has is left
/// to the checker. What is wrong with a file or with the imports is
/// reported to `sink`; `Err` is a file that cannot be read.
pub fn load(
    (path, files): (&str, &[PathBuf]),
    roots: &Roots,
    cx: &BuildContext,
    sources: &SourceMap,
    names: &ThreadedRodeo,
    sink: &Sink,
) -> Result<Graph, String> {
    let loader = Loader {
        roots,
        cx,
        sources,
        names,
        sink,
    };
    // The packages are found wave by wave: all that the packages of one
    // wave import and that are not loaded yet are parsed side by side.
    let mut loaded = vec![loader.parsed(path, files)?];
    let mut index: FxHashMap<String, usize> = FxHashMap::default();
    index.insert(path.to_owned(), 0);
    let mut wave = 0..1;
    while !wave.is_empty() {
        let mut wanted: Vec<&String> = loaded[wave.clone()]
            .iter()
            .flat_map(|package| &package.imports)
            .filter(|import| !index.contains_key(*import))
            .collect();
        wanted.sort();
        wanted.dedup();
        let found: Result<Vec<Parsed>, String> = wanted
            .par_iter()
            .map(|import| loader.imported(import))
            .collect();
        wave = loaded.len()..loaded.len();
        for package in found? {
            index.insert(package.path.clone(), loaded.len());
            loaded.push(package);
            wave.end += 1;
        }
    }
    for (package, at) in loaded.iter().zip(0..) {
        if package.files.is_empty() && at != 0 {
            let message = format!("package {}: no source files for this build", package.path);
            let by = loaded.iter().find_map(|other| {
                let import = other
                    .imports
                    .iter()
                    .position(|path| *path == package.path)?;
                Some(other.spans[import])
            });
            if let Some(span) = by {
                sink.push(Diagnostic::new(Code::LoadNoSourceFiles, span, message));
            }
        }
    }
    Ok(Graph {
        packages: ordered(loaded, &index, sink),
    })
}

/// The packages with each after those that it imports, the first of
/// `loaded` last. An import that closes a cycle is reported and not
/// followed.
fn ordered(loaded: Vec<Parsed>, index: &FxHashMap<String, usize>, sink: &Sink) -> Vec<Parsed> {
    #[derive(Clone, Copy, PartialEq)]
    enum State {
        New,
        Open,
        Done,
    }
    let mut state = vec![State::New; loaded.len()];
    let mut order = Vec::with_capacity(loaded.len());
    // The packages on the way from the first to the one that is walked,
    // each with the import that is looked at next.
    let mut way = vec![(0, 0)];
    state[0] = State::Open;
    while let Some(&mut (at, ref mut next)) = way.last_mut() {
        let package = &loaded[at];
        let Some(import) = package.imports.get(*next) else {
            state[at] = State::Done;
            order.push(at);
            way.pop();
            continue;
        };
        let span = package.spans[*next];
        *next += 1;
        let to = index[import];
        match state[to] {
            State::New => {
                state[to] = State::Open;
                way.push((to, 0));
            }
            State::Done => {}
            State::Open => {
                let from = way.iter().position(|&(on_way, _)| on_way == to);
                let cycle: Vec<&str> = way[from.unwrap_or(0)..]
                    .iter()
                    .map(|&(on_way, _)| &*loaded[on_way].path)
                    .chain([&**import])
                    .collect();
                let message = format!("import cycle not allowed: {}", cycle.join(" imports "));
                sink.push(Diagnostic::new(Code::LoadImportCycle, span, message));
            }
        }
    }
    let mut slots: Vec<Option<Parsed>> = loaded.into_iter().map(Some).collect();
    order
        .into_iter()
        .filter_map(|at| slots[at].take())
        .collect()
}

#[cfg(test)]
mod tests;
