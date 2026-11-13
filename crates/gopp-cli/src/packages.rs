//! The packages of a program: found from its imports, and checked each
//! after those that it imports.

use std::env;
use std::path::PathBuf;
use std::sync::Arc;

use gopp_build::{Graph, Parsed, Roots};
use gopp_diag::Sink;
use gopp_sema::types::Types;
use gopp_sema::{Package, PackageLoader, Unit, check};
use gopp_syntax::constraint::BuildContext;
use gopp_target::{TargetError, TargetSpec};
use lasso::ThreadedRodeo;
use rustc_hash::FxHashMap;

/// Where imported packages are looked for: the standard library is the
/// folder `std` of what `GOPPROOT` names, or of the sources that the
/// compiler was built from.
pub(crate) fn roots() -> Roots {
    let set = env::var_os("GOPPROOT").filter(|root| !root.is_empty());
    let root = set.map_or_else(
        || PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/../..")),
        PathBuf::from,
    );
    Roots {
        std: root.join("std"),
    }
}

/// The target that the flag names, or `GOPPOS` and `GOPPARCH`, or this
/// machine.
pub(crate) fn target(flag: Option<&str>) -> Result<&'static TargetSpec, TargetError> {
    let goppos = env::var("GOPPOS").ok();
    let gopparch = env::var("GOPPARCH").ok();
    TargetSpec::selected(flag, goppos.as_deref(), gopparch.as_deref())
}

/// What the files and declarations of a build for the target are chosen
/// by.
pub(crate) fn build_context(target: &TargetSpec) -> BuildContext {
    BuildContext {
        os: target.os.name().to_owned(),
        arch: target.arch.name().to_owned(),
        ..BuildContext::default()
    }
}

/// The import path of the package that the arguments name `name`: a
/// directory under a root is the package of that root.
pub(crate) fn import_path(name: &str, files: &[PathBuf], roots: &Roots) -> String {
    let dir = files.first().and_then(|file| file.parent());
    let under_root = match name {
        "command-line-arguments" => None,
        _ => dir.and_then(|dir| roots.path_of(dir)),
    };
    under_root.unwrap_or_else(|| name.to_owned())
}

/// Finds the packages that a package imports among those that are
/// checked. Any other is a stand-in whose names are all taken to exist,
/// until every package can be found.
struct Imports<'a> {
    checked: &'a FxHashMap<String, Arc<Package>>,
}

impl PackageLoader for Imports<'_> {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        match self.checked.get(path) {
            Some(package) => Ok(Arc::clone(package)),
            None => Ok(Arc::new(Package::fake(path))),
        }
    }
}

/// Checks the packages of the graph in its order, with one table of
/// types. The package that was asked for is the last; if it is named
/// `main` it has that path in its symbols, wherever its files are, as in
/// Go.
pub(crate) fn checked(
    graph: Graph,
    names: &ThreadedRodeo,
    types: &Types,
    sink: &Sink,
) -> Vec<(Parsed, Arc<Package>)> {
    let mut by_path: FxHashMap<String, Arc<Package>> = FxHashMap::default();
    let last = graph.packages.len().saturating_sub(1);
    let mut all = Vec::with_capacity(graph.packages.len());
    for (at, parsed) in graph.packages.into_iter().enumerate() {
        let units: Vec<Unit> = parsed
            .files
            .iter()
            .map(|(file, source)| Unit { file, source })
            .collect();
        let named_main = units.iter().any(|unit| {
            let name = unit.file.package;
            name.is_some_and(|name| names.resolve(&name.name) == "main")
        });
        let path = match at == last && named_main {
            true => "main",
            false => &parsed.path,
        };
        let loader = Imports { checked: &by_path };
        let package = Arc::new(check(path, &units, &loader, names, types, sink));
        by_path.insert(parsed.path.clone(), Arc::clone(&package));
        all.push((parsed, package));
    }
    all
}
