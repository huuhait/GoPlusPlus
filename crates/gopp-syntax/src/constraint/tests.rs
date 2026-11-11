use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_diag::Sink;
use gopp_span::{Dialect, SourceFile, SourceMap};
use lasso::ThreadedRodeo;

use super::*;
use crate::ast::dump;
use crate::parser;

fn tag(name: &str) -> Expr {
    Expr::Tag(name.to_owned())
}

fn not(x: Expr) -> Expr {
    Expr::Not(Box::new(x))
}

fn and(x: Expr, y: Expr) -> Expr {
    Expr::And(Box::new(x), Box::new(y))
}

fn or(x: Expr, y: Expr) -> Expr {
    Expr::Or(Box::new(x), Box::new(y))
}

fn context(os: &str, arch: &str) -> BuildContext {
    BuildContext {
        os: os.to_owned(),
        arch: arch.to_owned(),
        ..BuildContext::default()
    }
}

// The tables below are those of Go's `go/build/constraint` and `go/build`
// tests.

#[test]
fn a_condition_is_written_as_gofmt_writes_it() {
    let cases = [
        (tag("abc"), "abc"),
        (not(tag("abc")), "!abc"),
        (not(and(tag("abc"), tag("def"))), "!(abc && def)"),
        (
            and(tag("abc"), or(tag("def"), tag("ghi"))),
            "abc && (def || ghi)",
        ),
        (
            or(and(tag("abc"), tag("def")), tag("ghi")),
            "(abc && def) || ghi",
        ),
    ];
    for (x, text) in cases {
        assert_eq!(x.text(), text);
    }
}

#[test]
fn a_condition_is_cut_into_go_s_tokens() {
    let cases = [
        ("", ""),
        ("x", "x"),
        ("x.y", "x.y"),
        ("x_y", "x_y"),
        ("αx", "αx"),
        ("αx²", "αx err: invalid syntax at ²"),
        ("go1.2", "go1.2"),
        ("x y", "x y"),
        ("x!y", "x ! y"),
        ("&&||!()xy yx ", "&& || ! ( ) xy yx"),
        ("x~", "x err: invalid syntax at ~"),
        ("x ~", "x err: invalid syntax at ~"),
        ("x &", "x err: invalid syntax at &"),
        ("x &y", "x err: invalid syntax at &"),
    ];
    for (text, tokens) in cases {
        let mut parser = Parser::new(text);
        let mut read = Vec::new();
        loop {
            match parser.lex() {
                Ok(()) if parser.token.is_empty() => break,
                Ok(()) => read.push(parser.token.to_owned()),
                Err(error) => {
                    read.push(format!("err: {}", error.message));
                    break;
                }
            }
        }
        assert_eq!(read.join(" "), tokens, "{text:?}");
    }
}

#[test]
fn a_condition_parses_as_in_go() {
    let (x, y, z) = (|| tag("x"), || tag("y"), || tag("z"));
    let cases = [
        ("x", x()),
        ("x&&y", and(x(), y())),
        ("x||y", or(x(), y())),
        ("(x)", x()),
        ("x||y&&z", or(x(), and(y(), z()))),
        ("x&&y||z", or(and(x(), y()), z())),
        ("x&&(y||z)", and(x(), or(y(), z()))),
        ("(x||y)&&z", and(or(x(), y()), z())),
        ("!(x&&y)", not(and(x(), y()))),
    ];
    for (text, tree) in cases {
        assert_eq!(parse(text), Ok(tree), "{text}");
    }
}

#[test]
fn an_error_has_go_s_wording_and_place() {
    let cases = [
        ("x && ", 5, "unexpected end of expression"),
        ("x && (", 6, "missing close paren"),
        ("x && ||", 5, "unexpected token ||"),
        ("x && !", 6, "unexpected end of expression"),
        ("x && !!", 6, "double negation not allowed"),
        ("x !", 2, "unexpected token !"),
        ("x && (y", 5, "missing close paren"),
    ];
    for (text, offset, message) in cases {
        let error = SyntaxError {
            offset,
            message: message.to_owned(),
        };
        assert_eq!(parse(text), Err(error), "{text}");
    }
}

#[test]
fn a_condition_larger_than_go_s_limit_is_refused() {
    let terms = |count: usize| vec!["x"; count].join(" && ");
    assert!(parse(&terms(MAX_TERMS)).is_ok());
    let error = parse(&terms(MAX_TERMS + 1)).unwrap_err();
    assert_eq!(error.message, "build expression too large");
    let nested = format!("{}x{}", "(".repeat(MAX_TERMS), ")".repeat(MAX_TERMS));
    assert_eq!(
        parse(&nested).unwrap_err().message,
        "build expression too large"
    );
}

#[test]
fn every_tag_of_a_condition_is_asked() {
    let cases = [
        ("x", false, "x"),
        ("x && y", false, "x y"),
        ("x || y", false, "x y"),
        ("!x && yes", true, "x yes"),
        ("yes || y", true, "y yes"),
    ];
    for (text, value, tags) in cases {
        let mut asked = BTreeSet::new();
        let mut holds = |tag: &str| {
            asked.insert(tag.to_owned());
            tag == "yes"
        };
        assert_eq!(parse(text).unwrap().eval(&mut holds), value, "{text}");
        let tags: BTreeSet<String> = tags.split(' ').map(str::to_owned).collect();
        assert_eq!(asked, tags, "{text}");
    }
}

#[test]
fn a_plus_build_line_parses_as_in_go() {
    let (x, y, z) = (|| tag("x"), || tag("y"), || tag("z"));
    let cases = [
        ("x", x()),
        ("x,y", and(x(), y())),
        ("x y", or(x(), y())),
        ("x y,z", or(x(), and(y(), z()))),
        ("x,y z", or(and(x(), y()), z())),
        ("x,!y !z", or(and(x(), not(y())), not(z()))),
        ("!! x", or(tag("ignore"), x())),
        ("!!x", tag("ignore")),
        ("!x", not(x())),
        ("!", tag("ignore")),
        ("", tag("ignore")),
    ];
    for (text, tree) in cases {
        assert_eq!(parse_plus_build(text), Some(tree), "{text:?}");
    }
}

#[test]
fn build_lines_are_told_from_other_comments() {
    let cases = [
        ("//+build !", Some(tag("ignore"))),
        ("//+build", Some(tag("ignore"))),
        ("//+build x y", Some(or(tag("x"), tag("y")))),
        ("// +build x y \nmore", None),
        (" //+build x y", None),
        ("// +builder", None),
        ("//go:build x && y", Some(and(tag("x"), tag("y")))),
        ("//go:build x && y\nmore", None),
        (" //go:build x && y", None),
        ("//go:buildx", None),
        ("// go:build x", None),
        ("//go:build", None),
    ];
    for (line, tree) in cases {
        assert_eq!(parse_line(line), tree, "{line:?}");
    }
    assert!(is_go_build("//go:build"));
    assert_eq!(go_build_text("//go:build  a || b "), Some("a || b"));
}

/// Whether a build with the tag `yes` takes a `.go` file with this text.
fn go_file_is_taken(text: &str) -> Result<bool, String> {
    let map = SourceMap::new();
    let source = SourceFile::new("x.go".into(), text.to_owned(), Dialect::Go).unwrap();
    let id = map.add(source);
    let cx = BuildContext {
        tags: vec!["yes".to_owned()],
        ..BuildContext::default()
    };
    match go_header(id, &map.file(id)) {
        Ok(Some(x)) => Ok(x.eval(&mut |tag| cx.holds(tag))),
        Ok(None) => Ok(true),
        Err(diagnostic) => Err(diagnostic.message),
    }
}

#[test]
fn the_header_of_a_go_file_decides_as_in_go() {
    let cases = [
        ("// +build yes\n\npackage main\n", true),
        ("//go:build yes\npackage main\n", true),
        ("// +build no yes\n\npackage main\n", true),
        ("//go:build no || yes\npackage main\n", true),
        ("// +build no,yes\n\npackage main\n", false),
        ("//go:build no && yes\npackage main\n", false),
        (
            "// +build cgo\n\n// Copyright The Go Authors.\n\n\
             // This package implements parsing of tags like\n// +build tag1\npackage build",
            false,
        ),
        (
            "//go:build cgo\n// Copyright The Go Authors.\n\n\
             // This package implements parsing of tags like\n// +build tag1\npackage build",
            false,
        ),
        (
            "// Copyright The Go Authors.\n\npackage build\n\n\
             // shouldBuild checks tags given by lines of the form\n// +build tag\n\
             //go:build tag\nfunc shouldBuild(content []byte)\n",
            true,
        ),
        // A `// +build` line needs a blank line after it.
        ("// +build yes\npackage main\n", true),
        ("//go:build yes\npackage main\n", true),
        ("// +build no\npackage main\n", true),
        ("//go:build no\npackage main\n", false),
        ("// +build yes\n\n//go:build no\npackage main\n", false),
        (
            "/* */\n// +build yes\n\n//go:build no\npackage main\n",
            false,
        ),
        ("/*\n//go:build no\n*/\n\npackage main\n", true),
        ("/*\ntext\n*/\n\n//go:build no\npackage main\n", false),
        (
            "/*/*/ /* hi *//* \ntext\n*/\n\n//go:build no\npackage main\n",
            false,
        ),
        ("/**///go:build no\npackage main\n", true),
        ("/**/\n//go:build no\npackage main\n", false),
        // Two `// +build` lines must both hold.
        ("// +build yes\n// +build no\n\npackage main\n", false),
        ("  //go:build no  \r\npackage main\n", false),
        ("package main\n", true),
        ("", true),
    ];
    for (text, taken) in cases {
        assert_eq!(go_file_is_taken(text), Ok(taken), "{text:?}");
    }
    assert_eq!(
        go_file_is_taken("//go:build yes\n//go:build no\npackage main\n"),
        Err("multiple //go:build comments".to_owned())
    );
    assert_eq!(
        go_file_is_taken("//go:build yes &&\npackage main\n"),
        Err("parsing //go:build line: unexpected end of expression".to_owned())
    );
}

#[test]
fn a_system_name_at_the_end_of_a_file_name_counts() {
    let plan9 = context("plan9", "arm");
    let linux = context("linux", "amd64");
    let cases = [
        (&plan9, "foo_arm.go", true),
        (&plan9, "foo_darwin.go", false),
        (&plan9, "foo.go", true),
        (&plan9, "plan9.go", true),
        (&linux, "foo_linux.go", true),
        (&linux, "foo_plan9.go", false),
        (&linux, "plan9.go", true),
        (&linux, "plan9_test.go", true),
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
    assert!(with_tag.takes_file_name("hello_linux.go"));
}

#[test]
fn test_files_end_in_test() {
    assert!(is_test_file("open_test.gopp"));
    assert!(is_test_file("open_linux_test.go"));
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
    for word in ["linux", "amd64", "unix", "debug", "fast", "go1.1", "go1.25"] {
        assert!(cx.holds(word), "{word}");
        assert!(cx.knows(word), "{word}");
    }
    for word in ["windows", "arm64", "release", "slow"] {
        assert!(!cx.holds(word), "{word}");
        assert!(cx.knows(word), "{word}");
    }
    for word in ["linx", "go1.26", "go1.x", "go1", "cgo", ""] {
        assert!(!cx.holds(word), "{word}");
        assert!(!cx.knows(word), "{word}");
    }
    assert!(!context("windows", "amd64").holds("unix"));
}

/// What `@Build` leaves of `text` for `cx`: whether the file is taken, the
/// names of its declarations, and the diagnostics.
fn applied(text: &str, cx: &BuildContext) -> (bool, Vec<String>, Vec<String>) {
    let map = SourceMap::new();
    let source = SourceFile::new("x.gopp".into(), text.to_owned(), Dialect::Gopp).unwrap();
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

fn go(arguments: &[&str], os: &str) -> String {
    let output = Command::new("go")
        .args(arguments)
        .env("GOOS", os)
        .env("GOARCH", "amd64")
        .env("CGO_ENABLED", "0")
        .env("GOFLAGS", "")
        .output()
        .unwrap();
    assert!(output.status.success(), "go {arguments:?} failed");
    String::from_utf8(output.stdout).unwrap()
}

/// For both targets of version 1, every `.go` file of Go's standard library
/// and commands is taken or left out as `go list` says. Needs `go` on `PATH`.
#[test]
#[ignore = "long: asks go list about the Go standard library"]
fn files_of_the_go_standard_library_are_chosen_as_go_chooses_them() {
    const FORMAT: &str = "{{.Dir}}|{{join .GoFiles \" \"}} {{join .TestGoFiles \" \"}} \
                          {{join .XTestGoFiles \" \"}}|{{join .IgnoredGoFiles \" \"}}";
    for os in ["linux", "windows"] {
        // What the Go tool adds to the tags by itself: its compiler and its
        // experiments.
        let tool_tags = go(&["list", "-f", "{{context.ToolTags}}", "runtime"], os);
        let tool_tags = tool_tags.trim().trim_matches(['[', ']']).split(' ');
        let mut tags: Vec<String> = tool_tags.map(str::to_owned).collect();
        tags.push("gc".to_owned());
        let cx = BuildContext {
            tags,
            ..context(os, "amd64")
        };

        let mut wanted: BTreeMap<PathBuf, bool> = BTreeMap::new();
        for line in go(&["list", "-e", "-f", FORMAT, "std", "cmd"], os).lines() {
            let fields: Vec<&str> = line.split('|').collect();
            let dir = Path::new(fields[0]);
            for (names, taken) in [(fields[1], true), (fields[2], false)] {
                for name in names.split_whitespace() {
                    wanted.insert(dir.join(name), taken);
                }
            }
        }
        assert!(wanted.len() > 5000, "{os}: only {} files", wanted.len());

        let mut differences = Vec::new();
        let mut left_out = 0;
        for (path, &taken) in &wanted {
            let name = path.file_name().unwrap().to_str().unwrap();
            let bytes = fs::read(path).unwrap();
            // Go leaves these out for other reasons than a condition.
            let uses_cgo = bytes.windows(10).any(|w| w == b"import \"C\"");
            if name.starts_with(['_', '.']) || !taken && uses_cgo {
                continue;
            }
            let map = SourceMap::new();
            let id = map.add(SourceFile::from_bytes(path.clone(), &bytes, Dialect::Go).unwrap());
            let header = go_header(id, &map.file(id)).unwrap();
            let ours =
                cx.takes_file_name(name) && header.is_none_or(|x| x.eval(&mut |tag| cx.holds(tag)));
            left_out += usize::from(!ours);
            if ours != taken {
                differences.push(format!("{}: go says {taken}", path.display()));
            }
        }
        println!("{os}: {} files, {left_out} left out", wanted.len());
        assert!(left_out > 500, "{os}: only {left_out} files left out");
        assert!(differences.is_empty(), "{os}:\n{}", differences.join("\n"));
    }
}
