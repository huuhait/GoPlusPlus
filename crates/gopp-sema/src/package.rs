//! A checked package and where imported ones come from.

use std::sync::Arc;

use lasso::Spur;
use rustc_hash::FxHashMap;

use crate::object::{FileInfo, ObjId, Object};

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
    /// Stands in for a package that could not be loaded. Every name is
    /// taken to exist in it, so that one failed import is one error.
    pub fake: bool,
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
