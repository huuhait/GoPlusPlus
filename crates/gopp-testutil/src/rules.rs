//! Which rules of the specification a test checks.
//!
//! A rule is defined in `docs/spec/` by a paragraph that starts with its
//! ID in bold, `**OWN-R3a.**`. A test names the rules it checks on a
//! comment line of its own: `// rule: OWN-R3a`, or several IDs separated
//! by commas.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use crate::samples::entries;

const MARK: &str = "// rule:";

/// The rules that tests name, set against the rules of the specification.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Coverage {
    /// How many rules the specification defines.
    pub defined: usize,
    /// The rules that no test names, in the order of their IDs.
    pub untested: Vec<String>,
    /// What a test names that is no rule of the specification, with the
    /// file that names it.
    pub unknown: Vec<(String, PathBuf)>,
}

/// Whether `text` is a rule ID: capital letters, `-`, letters and digits.
fn is_id(text: &str) -> bool {
    let Some((chapter, number)) = text.split_once('-') else {
        return false;
    };
    !chapter.is_empty()
        && chapter.chars().all(|c| c.is_ascii_uppercase())
        && !number.is_empty()
        && number.chars().all(|c| c.is_ascii_alphanumeric())
}

/// The IDs of the rules that the Markdown files of `spec` define.
pub fn defined(spec: &Path) -> BTreeSet<String> {
    let mut rules = BTreeSet::new();
    for path in entries(spec) {
        if path.extension().is_none_or(|extension| extension != "md") {
            continue;
        }
        for line in read(&path).lines() {
            let id = line
                .strip_prefix("**")
                .and_then(|rest| rest.split_once(".**"));
            if let Some((id, _)) = id.filter(|(id, _)| is_id(id)) {
                rules.insert(id.to_owned());
            }
        }
    }
    rules
}

/// What the source files under `roots` name on their `// rule:` lines,
/// each with the files that name it.
pub fn named(roots: &[PathBuf]) -> BTreeMap<String, Vec<PathBuf>> {
    let mut named: BTreeMap<String, Vec<PathBuf>> = BTreeMap::new();
    let mut pending = roots.to_vec();
    while let Some(path) = pending.pop() {
        if path.is_dir() {
            if path.file_name().is_none_or(|name| name != "target") {
                pending.extend(entries(&path));
            }
            continue;
        }
        let is_source = path
            .extension()
            .and_then(|extension| extension.to_str())
            .is_some_and(|extension| ["rs", "go", "gopp"].contains(&extension));
        if !is_source {
            continue;
        }
        for line in read(&path).lines() {
            let Some(ids) = line.trim().strip_prefix(MARK) else {
                continue;
            };
            for id in ids.split(',').map(str::trim) {
                let files = named.entry(id.to_owned()).or_default();
                if files.last() != Some(&path) {
                    files.push(path.clone());
                }
            }
        }
    }
    named
}

/// Sets what the sources under `roots` name against the rules of `spec`.
pub fn coverage(spec: &Path, roots: &[PathBuf]) -> Coverage {
    let defined = defined(spec);
    let named = named(roots);
    let untested = defined.iter().filter(|id| !named.contains_key(*id));
    let mut unknown = Vec::new();
    for (id, files) in &named {
        if !defined.contains(id) {
            unknown.extend(files.iter().map(|file| (id.clone(), file.clone())));
        }
    }
    Coverage {
        defined: defined.len(),
        untested: untested.cloned().collect(),
        unknown,
    }
}

/// The coverage of the specification of this repository by its tests.
pub fn of_workspace() -> Coverage {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let roots = ["crates", "tests", "docs/spec/examples"].map(|dir| root.join(dir));
    coverage(&root.join("docs/spec"), &roots)
}

fn read(path: &Path) -> String {
    match fs::read(path) {
        Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
        Err(error) => panic!("{}: {error}", path.display()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The rules of this repository's specification without a test may
    /// only become fewer.
    const UNTESTED_AT_MOST: usize = 246;

    /// `OWN-R3a` after `OWN-R3` and before `OWN-R10`: the letters, the
    /// number and what follows it.
    fn order(id: &str) -> (&str, u32, &str) {
        let number = id.split_once('-').map_or(id, |(_, number)| number);
        let digits = number.find(|c: char| c.is_ascii_digit()).unwrap_or(0);
        let rest = &number[digits..];
        let end = rest
            .find(|c: char| !c.is_ascii_digit())
            .unwrap_or(rest.len());
        (
            &number[..digits],
            rest[..end].parse().unwrap_or(0),
            &rest[end..],
        )
    }

    #[test]
    fn a_rule_that_no_test_names_is_listed() {
        let dir = tempfile::tempdir().unwrap();
        let spec = dir.path().join("spec");
        let tests = dir.path().join("tests");
        fs::create_dir_all(tests.join("nested/target")).unwrap();
        fs::create_dir(&spec).unwrap();
        let chapter = "# Ownership\n\n**OWN-R3a.** *Places.* Text that names [OWN-P5](x).\n\n\
                       **OWN-P5.** More text.\n\n**ERR-3.** Returns.\n\n- **not-a-rule.** No.\n";
        fs::write(spec.join("04-ownership.md"), chapter).unwrap();
        fs::write(spec.join("notes.txt"), "**OWN-X1.** Not Markdown.\n").unwrap();
        let test = "fn f() {}\n    // rule: OWN-R3a, ERR-3\n#[test]\nfn g() {}\n// rule: OWN-Z9\n\
                    let text = \"// rule: OWN-P5 is inside a line\";\n";
        fs::write(tests.join("nested/a.rs"), test).unwrap();
        fs::write(tests.join("nested/b.gopp"), "// rule: ERR-3\n").unwrap();
        fs::write(tests.join("nested/c.txt"), "// rule: OWN-P5\n").unwrap();
        fs::write(tests.join("nested/target/d.rs"), "// rule: OWN-P5\n").unwrap();

        let found = coverage(&spec, std::slice::from_ref(&tests));
        let expected = Coverage {
            defined: 3,
            untested: vec!["OWN-P5".to_owned()],
            unknown: vec![("OWN-Z9".to_owned(), tests.join("nested/a.rs"))],
        };
        assert_eq!(found, expected);
    }

    #[test]
    fn every_rule_that_a_test_names_is_in_the_specification() {
        let found = of_workspace();
        assert!(found.defined >= 291, "only {} rules found", found.defined);
        assert_eq!(found.unknown, []);
    }

    /// Prints the rules that no test names.
    #[test]
    #[ignore = "long: lists the rules of the specification without a test"]
    fn rules_without_a_test_are_listed() {
        let found = of_workspace();
        println!(
            "{} of {} rules have no test:",
            found.untested.len(),
            found.defined
        );
        // One line for the rules of each chapter.
        let mut chapters: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
        for id in &found.untested {
            let chapter = id.split_once('-').map_or("", |(chapter, _)| chapter);
            chapters.entry(chapter).or_default().push(id);
        }
        for ids in chapters.values_mut() {
            ids.sort_by_key(|id| order(id));
            println!("{}", ids.join(" "));
        }
        assert!(
            found.untested.len() <= UNTESTED_AT_MOST,
            "{} rules have no test, {UNTESTED_AT_MOST} had none before",
            found.untested.len()
        );
    }
}
