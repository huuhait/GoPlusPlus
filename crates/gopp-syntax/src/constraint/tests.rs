use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_diag::Sink;
use gopp_span::{SourceFile, SourceMap};
use lasso::ThreadedRodeo;

use super::*;
use crate::ast::dump;
use crate::parser;

fn context(os: &str, arch: &str) -> BuildContext {
    BuildContext {
        os: os.to_owned(),
        arch: arch.to_owned(),
        ..BuildContext::default()
    }
}

#[test]
fn a_build_comment_is_told_from_other_comments() {
    assert_eq!(go_build_text("//go:build  a || b "), Some("a || b"));
    assert_eq!(go_build_text("//go:build"), Some(""));
    for other in ["//go:buildx", "// go:build x", "//go:build x\nmore"] {
        assert_eq!(go_build_text(other), None, "{other:?}");
    }
}

#[test]
fn a_system_name_at_the_end_of_a_file_name_counts() {
    let plan9 = context("plan9", "arm");
    let linux = context("linux", "amd64");
    let cases = [
        (&plan9, "foo_arm.gopp", true),
        (&plan9, "foo_darwin.gopp", false),
        (&plan9, "foo.gopp", true),
        (&plan9, "plan9.gopp", true),
        (&linux, "foo_linux.gopp", true),
        (&linux, "foo_plan9.gopp", false),
        (&linux, "plan9.gopp", true),
        (&linux, "plan9_test.gopp", true),
        (&linux, "amd64.s", true),
        (&linux, "open_linux.gopp", true),
        (&linux, "open_windows.gopp", false),
        (&linux, "open_linux_amd64.gopp", true),
        (&linux, "open_linux_arm64.gopp", false),
        (&linux, "open_windows_amd64_test.gopp", false),
        (&linux, "open_amd64_test.gopp", true),
        (&linux, "open_wasm32.gopp", false),
        // Only the end of the name counts.
        (&linux, "open_windows_helper.gopp", true),
        (&linux, "windows_open.gopp", true),
        (&linux, "open_arm64.pb.gopp", false),
    ];
    for (cx, name, taken) in cases {
        assert_eq!(cx.takes_file_name(name), taken, "{name}");
    }
    let with_tag = BuildContext {
        tags: vec!["linux".to_owned()],
        ..context("darwin", "arm64")
    };
    assert!(with_tag.takes_file_name("hello_linux.gopp"));
}

#[test]
fn test_files_end_in_test() {
    assert!(is_test_file("open_test.gopp"));
    assert!(is_test_file("open_linux_test.gopp"));
    assert!(!is_test_file("test.gopp"));
    assert!(!is_test_file("open_test_helper.gopp"));
}

#[test]
fn a_word_holds_for_the_system_the_profile_and_the_tags() {
    let cx = BuildContext {
        profile: "debug".to_owned(),
        tags: vec!["fast".to_owned()],
        known: vec!["release".to_owned(), "slow".to_owned()],
        ..context("linux", "amd64")
    };
    for word in ["linux", "amd64", "unix", "debug", "fast"] {
        assert!(cx.holds(word), "{word}");
        assert!(cx.knows(word), "{word}");
    }
    for word in ["windows", "arm64", "release", "slow"] {
        assert!(!cx.holds(word), "{word}");
        assert!(cx.knows(word), "{word}");
    }
    for word in ["linx", "go1.25", "cgo", ""] {
        assert!(!cx.holds(word), "{word}");
        assert!(!cx.knows(word), "{word}");
    }
    assert!(!context("windows", "amd64").holds("unix"));
}

/// What `@Build` leaves of `text` for `cx`: whether the file is taken, the
/// names of its declarations, and the diagnostics.
fn applied(text: &str, cx: &BuildContext) -> (bool, Vec<String>, Vec<String>) {
    let map = SourceMap::new();
    let source = SourceFile::new("x.gopp".into(), text.to_owned()).unwrap();
    let id = map.add(source);
    let source = map.file(id);
    let names = ThreadedRodeo::new();
    let sink = Sink::new(10);
    let mut file = parser::parse(id, &source, &names, &sink);
    let taken = apply(&mut file, &source, cx, &sink);
    let tree = dump(&file, source.text(), &names);
    fn declared(line: &str) -> Option<String> {
        let mut words = line.strip_prefix("  ")?.split(' ');
        let kind = words.next()?;
        let name = words.next()?;
        matches!(kind, "Func" | "Var" | "Const" | "Type").then(|| name.to_owned())
    }
    let declarations = tree.lines().filter_map(declared);
    let message = |d: gopp_diag::Diagnostic| format!("{}..{} {}", d.span.lo, d.span.hi, d.message);
    (
        taken,
        declarations.collect(),
        sink.finish().into_iter().map(message).collect(),
    )
}

#[test]
fn a_build_attribute_keeps_or_leaves_out_its_declaration() {
    let text = "package p\n\n\
                @Build(linux)\nfunc onLinux() {}\n\n\
                @Build(windows)\nfunc onWindows() {}\n\n\
                @Build(!windows && (amd64 || arm64))\nvar (\n\ta = 1\n\tb = 2\n)\n\n\
                @Build(debug)\n@Inline\nfunc inDebug() {}\n\n\
                @Build(release || fast)\nconst c = 1\n\n\
                @Inline\nfunc always() {}\n\n\
                type T int\n";
    let cx = |os: &str, profile: &str| BuildContext {
        profile: profile.to_owned(),
        known: ["debug", "release", "fast"].map(str::to_owned).to_vec(),
        ..context(os, "amd64")
    };
    assert_eq!(
        applied(text, &cx("linux", "debug")),
        (
            true,
            ["onLinux", "a", "b", "inDebug", "always", "T"]
                .map(str::to_owned)
                .to_vec(),
            vec![]
        )
    );
    assert_eq!(
        applied(text, &cx("windows", "release")),
        (
            true,
            ["onWindows", "c", "always", "T"]
                .map(str::to_owned)
                .to_vec(),
            vec![]
        )
    );
}

#[test]
fn a_build_attribute_above_the_package_clause_is_for_the_file() {
    let text = "@Build(windows && amd64)\n\npackage p\n\nfunc f() {}\n";
    let (taken, declarations, diagnostics) = applied(text, &context("windows", "amd64"));
    assert_eq!((taken, diagnostics.len()), (true, 0));
    assert_eq!(declarations, ["f"]);
    let (taken, _, diagnostics) = applied(text, &context("linux", "amd64"));
    assert_eq!((taken, diagnostics.len()), (false, 0));
}

#[test]
fn a_word_nobody_knows_is_an_error() {
    let text = "@Build(linx)\n\npackage p\n\n@Build(windows && amd46 || fast)\nfunc f() {}\n";
    let unknown = |word: &str| {
        format!("{word} is not a system, an architecture, a build profile or a tag of this module")
    };
    // The condition of the file does not hold, so the file is not looked at.
    assert_eq!(
        applied(text, &context("linux", "amd64")),
        (
            false,
            vec!["f".to_owned()],
            vec![format!("7..11 {}", unknown("linx"))]
        )
    );
    let text = &text["@Build(linx)\n\n".len()..];
    assert_eq!(
        applied(text, &context("linux", "amd64")),
        (
            true,
            vec![],
            vec![
                format!("29..34 {}", unknown("amd46")),
                format!("38..42 {}", unknown("fast"))
            ]
        )
    );
}

#[test]
fn a_condition_the_parser_refused_leaves_the_declaration_in() {
    let (taken, declarations, diagnostics) = applied(
        "package p\n\n@Build(1)\nfunc f() {}\n",
        &context("linux", "amd64"),
    );
    assert!(taken);
    assert_eq!(declarations, ["f"]);
    assert_eq!(diagnostics.len(), 1);
}

