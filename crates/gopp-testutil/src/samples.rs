//! The sample programs of the specification.

use std::path::{Path, PathBuf};
use std::{fs, io};

use gopp_span::Dialect;

/// Every Go and Go++ source file under `docs/spec/examples`, sorted.
///
/// # Panics
///
/// Panics when the directory cannot be read or holds fewer files than the
/// specification has samples.
pub fn files() -> Vec<PathBuf> {
    let examples = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../docs/spec/examples");
    let mut files = Vec::new();
    for dir in entries(&examples) {
        if dir.is_dir() {
            let sources = entries(&dir).into_iter();
            files.extend(sources.filter(|path| Dialect::from_path(path).is_some()));
        }
    }
    files.sort();
    assert!(files.len() >= 20, "only {} sample files found", files.len());
    files
}

fn entries(dir: &Path) -> Vec<PathBuf> {
    let read = |result: io::Result<fs::DirEntry>| match result {
        Ok(entry) => entry.path(),
        Err(error) => panic!("{}: {error}", dir.display()),
    };
    match fs::read_dir(dir) {
        Ok(entries) => entries.map(read).collect(),
        Err(error) => panic!("{}: {error}", dir.display()),
    }
}
