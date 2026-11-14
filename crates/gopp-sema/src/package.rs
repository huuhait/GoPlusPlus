//! A checked package and where imported ones come from.

use std::sync::Arc;

use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::consteval::ConstEvaluator;
use crate::decls::Initializer;
use crate::foreign::ForeignPackage;
use crate::object::{FileInfo, ObjId, Object};

/// What a package and all that it imports do with the variables of
/// packages: a variable that a goroutine may use is not changed by a
/// plain write (CON-18).
#[derive(Clone, Debug, Default)]
pub struct Program {
    /// What the `go` statements start.
    pub roots: FxHashSet<crate::types::TouchKey>,
    /// The functions that are used as values, with the function
    /// literals: a call of a function value may run any of them. And
    /// the methods by their names: a call of a method of an interface
    /// may run any of that name.
    pub valued: FxHashSet<crate::types::TouchKey>,
    pub methods: FxHashMap<Spur, FxHashSet<crate::types::TouchKey>>,
    /// The variables that are changed outside `init`; whether one is
    /// changed through a pointer that may point into any of those
    /// that a pointer is taken into.
    pub written: FxHashSet<crate::types::Global>,
    pub written_any: bool,
    pub addressed: FxHashSet<crate::types::Global>,
    /// The variables whose changes were refused.
    pub refused: FxHashSet<crate::types::Global>,
}

#[derive(Debug, Default)]
pub struct Package {
    /// The import path.
    pub path: String,
    pub name: String,
    pub objects: Vec<Object>,
    /// The package scope.
    pub scope: FxHashMap<Spur, ObjId>,
    /// The macros of the package, which have a namespace of their own.
    pub macros: FxHashMap<Spur, ObjId>,
    pub files: Vec<FileInfo>,
    /// The values of the package-level variables in the order in which the
    /// program computes them at its start.
    pub init_order: Vec<Initializer>,
    /// For the offset of the name of each `@Operator` method that the
    /// package declares: its type, and which of the `@Operator` methods
    /// of the type it is, in the order of their declarations.
    pub operators: FxHashMap<u32, (crate::types::NamedId, u32)>,
    /// What each function of the package with a body does with the
    /// pointers that it is given, for those who call it.
    pub lends: FxHashMap<ObjId, Arc<crate::types::Lends>>,
    /// What the package and all that it imports do with the variables
    /// of packages.
    pub program: Arc<Program>,
    /// Stands in for a package that could not be loaded. Every name is
    /// taken to exist in it, so that one failed import is one error.
    pub fake: bool,
    /// For a C or C++ header: what says what each of its names is.
    pub foreign: Option<Arc<dyn ForeignPackage>>,
}

impl Package {
    /// A stand-in for the package at `path`, named as its path suggests.
    pub fn fake(path: &str) -> Self {
        Self {
            path: path.to_owned(),
            name: name_from_path(path).to_owned(),
            fake: true,
            ..Self::default()
        }
    }

    /// The header at `path` as a package, whose names `foreign` knows.
    pub fn header(path: &str, foreign: Arc<dyn ForeignPackage>) -> Self {
        Self {
            path: path.to_owned(),
            name: name_from_path(path).to_owned(),
            foreign: Some(foreign),
            ..Self::default()
        }
    }

    pub fn object(&self, id: ObjId) -> &Object {
        &self.objects[id.0 as usize]
    }
}

/// The likely name of the package at `path`: its last element; for a header
/// (`<vector>`, `"zlib/zlib.h"`), the file name without its extension.
fn name_from_path(path: &str) -> &str {
    let path = path.trim_start_matches('<').trim_end_matches('>');
    let last = path
        .trim_end_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or(path);
    let header = [".h", ".hh", ".hpp", ".hxx"];
    let stem = header
        .iter()
        .find_map(|extension| last.strip_suffix(extension));
    stem.unwrap_or(last)
}

/// Finds the packages a package imports. The checker does not know where
/// packages come from.
pub trait PackageLoader: Sync {
    /// The package that `path` names in an import of the package being
    /// checked, or why there is none.
    fn load(&self, path: &str) -> Result<Arc<Package>, String>;

    /// What runs the `@Const` functions; without it the value of a call
    /// in a constant expression is not known to the checker.
    fn evaluator(&self) -> Option<&dyn ConstEvaluator> {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::name_from_path;

    #[test]
    fn a_package_is_named_after_the_end_of_its_path() {
        let cases = [
            ("fmt", "fmt"),
            ("net/http", "http"),
            ("example.com/x/y/", "y"),
            ("<vector>", "vector"),
            ("<sys/stat.h>", "stat"),
            ("lib/zlib.h", "zlib"),
            ("a/b.hpp", "b"),
        ];
        for (path, name) in cases {
            assert_eq!(name_from_path(path), name);
        }
    }
}
