use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_testutil::{linear, samples, stress};
use lasso::ThreadedRodeo;
use proptest::prelude::*;

use super::{parse, parse_counted};
use crate::ast::{dump, dump_json};

struct Parsed {
    dump: String,
    json: String,
    diagnostics: Vec<Diagnostic>,
}

impl Parsed {
    /// Whether the tree has a `BadExpr`, `BadStmt` or `BadDecl`.
    fn has_bad_node(&self) -> bool {
        let mut lines = self.dump.lines();
        lines.any(|line| line.trim_start().starts_with("Bad"))
    }
}

fn parse_bytes(bytes: &[u8], dialect: Dialect) -> Parsed {
    let map = SourceMap::new();
    let id = map.add(SourceFile::from_bytes("test.gopp".into(), bytes, dialect).unwrap());
    let source = map.file(id);
    let names = ThreadedRodeo::new();
    let sink = Sink::new(1000);
    let file = parse(id, &source, &names, &sink);
    Parsed {
        dump: dump(&file, source.text(), &names),
        json: dump_json(&file, source.text(), &names),
        diagnostics: sink.finish(),
    }
}

fn parse_gopp(text: &str) -> Parsed {
    parse_bytes(text.as_bytes(), Dialect::Gopp)
}

/// The tree of a source that must parse without a diagnostic.
fn tree(text: &str) -> String {
    let parsed = parse_gopp(text);
    assert_eq!(parsed.diagnostics, [], "{text}");
    parsed.dump
}

#[test]
fn every_sample_parses_without_a_diagnostic() {
    for path in samples::files() {
        let dialect = Dialect::from_path(&path).unwrap();
        let parsed = parse_bytes(&fs::read(&path).unwrap(), dialect);
        let messages: Vec<_> = parsed
            .diagnostics
            .iter()
            .map(|d| format!("{}: {} {}", d.span.lo, d.code, d.message))
            .collect();
        assert!(messages.is_empty(), "{}: {messages:#?}", path.display());
        assert!(!parsed.has_bad_node(), "{}", path.display());
    }
}

#[test]
fn hello() {
    let source =
        "package main\n\nimport \"fmt\"\n\nfunc main() {\n\tfmt.Println(\"Hello, Go++!\")\n}\n";
    insta::assert_snapshot!(tree(source));
}

#[test]
fn a_group_without_declarations_parses() {
    let source = "package p\n\nimport ()\n\nconst ()\n\nvar ()\ntype ()\n\nfunc f() {}\n";
    let only_function =
        "File package=p\n  Func f @48..59\n    FuncType @48..56\n    Block @57..59\n";
    assert_eq!(tree(source), only_function);
    assert!(tree("package p\n\nvar ()").starts_with("File "));
}

/// The `.go` files under `dir`. `testdata` directories hold files that are
/// invalid on purpose.
fn go_files_under(dir: &Path, testdata: bool, files: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            if testdata || path.file_name().is_some_and(|name| name != "testdata") {
                go_files_under(&path, testdata, files);
            }
        } else if path.extension().is_some_and(|extension| extension == "go") {
            files.push(path);
        }
    }
}

fn go_root() -> PathBuf {
    let output = Command::new("go").args(["env", "GOROOT"]).output().unwrap();
    let goroot = String::from_utf8(output.stdout).unwrap();
    Path::new(goroot.trim()).join("src")
}

/// The program that prints the tree of Go's own parser.
const GOAST: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/goast/main.go");

/// FNV-1a, as Go's `hash/fnv` computes it.
fn fnv64(bytes: &[u8]) -> u64 {
    let hash = |hash: u64, &byte: &u8| (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3);
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, hash)
}

/// The dump without the text of literals: the byte range says the same,
/// and the other side does not have to spell the text as Rust does.
fn without_literal_text(dump: &str) -> String {
    let mut out = String::with_capacity(dump.len());
    for line in dump.lines() {
        let literal = line.find("BasicLit ").filter(|&at| {
            let indent = line[..at].trim_start();
            indent.is_empty() || indent.starts_with("[+") && indent.ends_with("] ")
        });
        match (literal, line.rfind(" @")) {
            (Some(at), Some(span)) => {
                let kind_end = at + "BasicLit ".len();
                let kind = line[kind_end..].split(' ').next().unwrap_or("");
                out.push_str(&line[..kind_end]);
                out.push_str(kind);
                out.push_str(&line[span..]);
            }
            _ => out.push_str(line),
        }
        out.push('\n');
    }
    out
}

/// Files whose tree differs from that of Go's parser because the two read
/// the text differently, and this parser follows Go's compiler:
/// `[P(*struct{})]` after a type name starts a type parameter list.
const READ_AS_THE_COMPILER_DOES: &[&str] = &["internal/types/testdata/fixedbugs/issue49482.go"];

/// "Everything not mentioned works as in Go", for syntax: each `.go` file
/// under GOROOT that both parsers accept has the same tree here as in Go's
/// `go/parser`, node for node and byte range for byte range. Needs `go` on
/// `PATH`.
#[test]
#[ignore = "long: parses the Go standard library with Go's parser too"]
fn the_tree_of_a_go_file_is_the_tree_of_go_s_parser() {
    let mut files = Vec::new();
    go_files_under(&go_root(), true, &mut files);
    files.sort();
    assert!(files.len() > 7000, "only {} files", files.len());

    let mut ours = Vec::new();
    for path in &files {
        let parsed = parse_bytes(&fs::read(path).unwrap(), Dialect::Go);
        if parsed.diagnostics.is_empty() {
            let hash = fnv64(without_literal_text(&parsed.dump).as_bytes());
            ours.push((path, format!("{hash:016x}")));
        }
    }

    let list: String = ours
        .iter()
        .map(|(path, _)| format!("{}\n", path.display()))
        .collect();
    let list_path = std::env::temp_dir().join(format!("gopp-goast-{}", std::process::id()));
    fs::write(&list_path, list).unwrap();
    let output = Command::new("go")
        .args(["run", GOAST, "-hash"])
        .stdin(fs::File::open(&list_path).unwrap())
        .output()
        .unwrap();
    fs::remove_file(&list_path).unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let theirs = String::from_utf8(output.stdout).unwrap();
    assert_eq!(theirs.lines().count(), ours.len());

    let known = |path: &Path| {
        READ_AS_THE_COMPILER_DOES
            .iter()
            .any(|known| path.ends_with(known))
    };
    let mut compared = 0;
    let mut differences = Vec::new();
    for ((path, hash), line) in ours.iter().zip(theirs.lines()) {
        let (their_hash, their_path) = line.split_once(' ').unwrap();
        assert_eq!(Path::new(their_path), path.as_path());
        // Go's parser refuses some files that Go's compiler accepts.
        if their_hash == "error" || known(path) {
            continue;
        }
        compared += 1;
        if their_hash != hash {
            differences.push(path.as_path());
        }
    }
    println!(
        "{} files, {} parsed here, {compared} compared, {} differ",
        files.len(),
        ours.len(),
        differences.len()
    );
    assert!(compared > 7000, "only {compared} files compared");

    let mut report = String::new();
    for path in differences.iter().take(20) {
        let parsed = parse_bytes(&fs::read(path).unwrap(), Dialect::Go);
        let here = without_literal_text(&parsed.dump);
        let output = Command::new("go")
            .args(["run", GOAST, "-dump"])
            .arg(path)
            .output()
            .unwrap();
        let there = String::from_utf8_lossy(&output.stdout).into_owned();
        let first = here.lines().zip(there.lines()).find(|(a, b)| a != b);
        let (a, b) = first.unwrap_or(("(one tree is longer)", ""));
        report.push_str(&format!(
            "{}\n  here: {}\n  go:   {}\n",
            path.display(),
            a.trim(),
            b.trim()
        ));
    }
    assert!(
        differences.is_empty(),
        "{} of {compared} trees differ\n{report}",
        differences.len()
    );
}

/// Everything Go's own parser accepts must parse here too. Needs `go` on
/// `PATH`, for the location of its standard library.
#[test]
#[ignore = "long: parses the whole Go standard library"]
fn the_go_standard_library_parses_without_a_diagnostic() {
    let mut files = Vec::new();
    go_files_under(&go_root(), false, &mut files);
    files.sort();
    assert!(
        files.len() > 5000,
        "only {} files under GOROOT",
        files.len()
    );

    let mut failures = Vec::new();
    for path in &files {
        let parsed = parse_bytes(&fs::read(path).unwrap(), Dialect::Go);
        if let Some(first) = parsed.diagnostics.first() {
            failures.push(format!(
                "{} @{}: {} {}",
                path.display(),
                first.span.lo,
                first.code,
                first.message
            ));
        } else if parsed.has_bad_node() {
            failures.push(format!("{}: Bad node without a diagnostic", path.display()));
        }
    }
    assert!(
        failures.is_empty(),
        "{} of {} files failed:\n{}",
        failures.len(),
        files.len(),
        failures[..failures.len().min(40)].join("\n")
    );
}

#[test]
fn functions_that_can_fail_and_unwrapping_calls() {
    insta::assert_snapshot!(tree(
        "package p

func load!(path string) []byte {
	f := os.Open!(path)
	return io.ReadAll!(f).Trim()
}

func parse[T any]!(s string) T { return conv[T]!(s) }

func (r *R) Read!(p []byte) int

var f = func!(x int) T { return g!(x) }

type Loader interface {
	Load!(path string) []byte
	Close()
}

type H func!(int) T
"
    ));
}

#[test]
fn attributes_macros_and_parents() {
    insta::assert_snapshot!(tree(
        "package p

@Derive(JSON, Equal)
type Config struct {
	ID   int    @Required
	Name string @json.Name(\"n\") @Required `x:\"y\"`
}

@Operator @Right
func (a Vec2) Mul(s float64) Vec2

@Pinned
@Extends(Payment)
@Extends(audit.Trail[int])
type Card struct {
	Number string
}

@Extends(Base[T]) @Abstract
type Box[T any, extends any] struct{ v T }

@Extends(Animal)
type Pet interface {
	Clone
	extends
	extends(n int) bool
}

type extends struct{ extends extends }

macro Swap(a expr, rest ...tokens, t type) {
	a, b = b, a
}

macro func Equal!(target decl) decl

#Register(Config)

func use(extends, base int) {
	#Assert(extends > 0, \"positive\")
	x := #pkg.Make(func() { run() })
	for _, &d := range docs {
		d.n++
	}
	for &p := range seq {
	}
}
"
    ));
}

#[test]
fn go_forms_that_need_care() {
    insta::assert_snapshot!(tree(
        "package p

import (
	. \"dot\"
	_ \"blank\"
	name \"path\"
)

const (
	A = iota
	B
)

type (
	Arr    [N]int
	Slice  []int
	Dots   [...]int
	Gen[P any, Q interface{ ~int | ~string }] struct{}
	Ptr[P *T,]  struct{}
	Alias = map[string]chan<- int
	Recv  <-chan <-chan int
)

func f(a, b int, c ...string) (x int, err error) {
	if v, ok := m[k]; ok && v == (T{1, 2}) {
	}
	for i := 0; i < 10; i++ {
	}
	for {
	}
	switch x := v.(type) {
	case []int, *T:
	default:
	}
	switch {
	case a < b:
		fallthrough
	case a > b:
	}
	select {
	case v := <-ch:
	case ch <- 1:
	default:
	}
L:
	for range ch {
		break L
	}
	go func() {}()
	defer mu.Unlock()
	s = a[1:2:3]
	s = a[:]
	g := G[int, string]{k: {1, 2}, 3: v}
	x, y = y, x
	x <<= 2
	p := &T{}
	return *p, nil
}
"
    ));
}

/// Code, 1-based line and column, and message of every diagnostic.
fn errors_in(text: &str) -> Vec<String> {
    let source = SourceFile::new("t".into(), text.to_owned(), Dialect::Gopp).unwrap();
    let diagnostics = parse_gopp(text).diagnostics;
    diagnostics
        .iter()
        .map(|d| {
            let at = source.line_col(d.span.lo);
            format!("{} {}:{} {}", d.code, at.line, at.column, d.message)
        })
        .collect()
}

#[test]
fn syntax_errors() {
    let cases = [
        "func f() {}",
        "package p\nimport \"a\"\nvar x int\nimport \"b\"\n",
        "package p\nimport 5\n",
        "package p\nimport x\n",
        "package p\nx := 1\n",
        "package p\nfunc f()\n{\n}\n",
        "package p\nfunc f() { x := (1 + ; }\n",
        "package p\nfunc f() { f(1 2) }\n",
        "package p\nfunc f() { f(1,\n2\n) }\n",
        "package p\nfunc f() { if { } }\n",
        "package p\nfunc f() { if x := 1; { } }\n",
        "package p\nfunc f() { if x\n{ } }\n",
        "package p\nfunc f() { if var x = 1; x > 0 { } }\n",
        "package p\nfunc f() { if x = 1 { } }\n",
        "package p\nfunc f() { for i := 0; i < 3; j := 1 { } }\n",
        "package p\nfunc f() { x = a[1::3] }\n",
        "package p\nfunc f() { x = a[1:2:] }\n",
        "package p\nfunc f() { x = a[] }\n",
        "package p\nfunc f() { go (g()) }\n",
        "package p\nfunc f() { L: }\nfunc g() { x := (T){} }\n",
        "package p\nfunc f(a, b int, string) {}\n",
        "package p\nfunc f(a int, b) {}\n",
        "package p\nfunc f[]() {}\n",
        "package p\nfunc f[T]() {}\n",
        "package p\ntype T[P any, Q] struct{}\n",
        "package p\ntype T[P,] int\n",
        "package p\ntype T [N,]int\n",
        "package p\nvar f func[T any]()\n",
        "package p\ntype I interface { m[T any]() }\n",
        "package p\ntype S struct { *(T) }\n",
        "package p\ntype S struct { T; sync.Mutex; *U; G[int] }\n",
        "package p\nfunc () m() {}\nfunc (a, b T) n() {}\n",
        "package p\nfunc f() { for _, &d = range xs {}\nfor &i, v := range xs {} }\n",
        "package p\nfunc f() { @Inline x := 1 }\n",
        "package p\n@Inline\nimport \"a\"\n",
        "package p\n@inline\nfunc f()\n",
        "package p\nmacro swap(a expr, b thing) {}\n",
        "package p\nvar x #Bits(12)\n",
        "package p\nfunc f() { #File }\n",
        "package p\n@Inline\n",
        "package p\nfunc f() { else }\n",
        "package p\nfunc f() { x := 1 y := 2 }\n",
        "package p\nvar x = 1 var y = 2\n",
        "package p\nfunc f() { var c <-<-chan int; var d <-chan<- int }\n",
        "package p\nfunc f() { if x { } else y }\n",
        "package p\nfunc f() { switch x { foo } }\n",
        "package p\nfunc f() { a, b }\n",
        "package p\nvar x := 1\n",
        "package p\ntype T\n",
        "package p\nfunc f() { x.[1] }\n",
        "package p\nfunc f() { #M(1, xs...) }\n@A(xs...)\nfunc g()\n",
        "package p\n@A\nvar ()\n@B\nconst (\n\tc = 1\n)\n",
        "@Inline\npackage p\n",
        "@Build\npackage p\n",
        "package p\n@Build(linux, amd64)\nfunc f()\n@Build()\nfunc g()\n",
        "package p\n@Build(linux == 1)\nvar a int\n@Build(os.Linux)\nvar b int\n@Build(\"linux\")\nvar c int\n",
        "package p\n@Build(linux)\n@Build(amd64)\nfunc f()\n",
        "package p\ntype T struct {\n\tx int @Build(linux)\n}\n",
        "package p\n@Build(linux)\n@Inline\nimport \"a\"\n",
        "package p\nfunc f() {\n\t@Build(linux)\n\tx := 1\n}\n",
    ];
    let mut report = String::new();
    for case in cases {
        report.push_str(&format!("{case:?}\n"));
        for error in errors_in(case) {
            report.push_str(&format!("    {error}\n"));
        }
    }
    insta::assert_snapshot!(report);
}

#[test]
fn an_unknown_macro_fragment_kind_lists_the_kinds() {
    assert_eq!(
        errors_in("package p\nmacro M(\n\ta thing,\n\tb type,\n) {}\n"),
        [
            "E0065 3:4 unknown macro fragment kind thing; the kinds are expr, stmt, block, type, \
          ident, lit, decl, tokens"
        ]
    );
}

#[test]
fn extends_is_not_a_keyword() {
    for source in [
        "package p\ntype Card extends Payment { n int }\n",
        "package p\ntype Box[T extends Animal] struct{}\n",
        "package p\ntype Pet interface { extends Animal }\n",
    ] {
        assert_eq!(errors_in(source).len(), 1, "{source}");
    }
}

#[test]
fn an_attribute_does_not_end_the_imports() {
    assert_eq!(
        errors_in("package p\n@Inline\nimport \"a\"\n"),
        ["E0067 2:1 the only attribute of an import is @Build"]
    );
}

#[test]
fn build_conditions() {
    insta::assert_snapshot!(tree(
        "@Build(linux && !arm64)\n\npackage p\n\n@Build(windows)\nimport \"<windows.h>\"\n\n\
         @Build(linux || (unix && debug))\nfunc f() {}\n\n\
         @Build(!windows)\n@Inline\nvar (\n\ta = 1\n\tb = 2\n)\n"
    ));
}

fn parse_go(text: &str) -> Parsed {
    parse_bytes(text.as_bytes(), Dialect::Go)
}

#[test]
fn go_files_have_no_gopp_syntax() {
    let names = "package p

type extends struct {
	sync.Mutex
	*base
}

func macro(extends, base int) int { return extends + base }
";
    let parsed = parse_go(names);
    assert_eq!(parsed.diagnostics, []);
    assert!(parsed.dump.contains("Field  @"), "{}", parsed.dump);

    for (text, code) in [
        ("package p\nfunc f() { g!(x) }\n", Code::Expected),
        ("package p\nfunc f!() {}\n", Code::ExpectedFound),
        ("package p\n@Inline\nfunc f() {}\n", Code::LexIllegalChar),
        ("package p\nfunc f() { #Assert(x) }\n", Code::LexIllegalChar),
        (
            "package p\nfunc (r R) m[T any]() {}\n",
            Code::MethodTypeParams,
        ),
    ] {
        let diagnostics = parse_go(text).diagnostics;
        assert_eq!(diagnostics.first().map(|d| d.code), Some(code), "{text}");
    }
}

#[test]
fn nesting_past_the_limit_is_one_diagnostic() {
    let deep = [
        (
            format!(
                "package p\nvar x = {}1{}\n",
                "(".repeat(5000),
                ")".repeat(5000)
            ),
            Code::ExpressionTooDeeplyNested,
        ),
        (
            format!("package p\nvar x = {}1\n", "!".repeat(5000)),
            Code::ExpressionTooDeeplyNested,
        ),
        (
            format!(
                "package p\nvar x = T{}{}\n",
                "{".repeat(5000),
                "}".repeat(5000)
            ),
            Code::ExpressionTooDeeplyNested,
        ),
        (
            format!("package p\nvar x {}int\n", "[]".repeat(5000)),
            Code::NestingTooDeep,
        ),
        (
            format!("package p\nvar x {}int\n", "*".repeat(5000)),
            Code::NestingTooDeep,
        ),
        (
            format!(
                "package p\nfunc f() {}{}\n",
                "{".repeat(5000),
                "}".repeat(5000)
            ),
            Code::NestingTooDeep,
        ),
        (
            format!("package p\nfunc f() {{ {} }}\n", "L: ".repeat(5000)),
            Code::ExpressionTooDeeplyNested,
        ),
        (
            format!(
                "package p\nvar x = {}nil{}\n",
                "func() { return ".repeat(600),
                " }".repeat(600)
            ),
            Code::ExpressionTooDeeplyNested,
        ),
    ];
    for (source, code) in deep {
        let codes: Vec<_> = parse_gopp(&source)
            .diagnostics
            .iter()
            .map(|d| d.code)
            .collect();
        assert_eq!(codes, [code], "{}", &source[..40]);
    }
}

#[test]
fn nesting_at_the_limit_is_accepted() {
    let source = format!(
        "package p\nvar x = {}1{}\n",
        "(".repeat(1000),
        ")".repeat(1000)
    );
    assert_eq!(parse_gopp(&source).diagnostics, []);
}

#[test]
fn long_chains_are_not_nesting() {
    let sources = [
        format!("package p\nvar x = 1{}\n", " + 1".repeat(30_000)),
        format!("package p\nvar x = a{}\n", "[0]".repeat(20_000)),
        format!("package p\nvar x = a{}\n", ".b()".repeat(20_000)),
        format!(
            "package p\nfunc f() {{ if a {{}}{} }}\n",
            " else if a {}".repeat(10_000)
        ),
        format!(
            "package p\nfunc f() {{\n{}}}\n",
            "x = x + 1\n".repeat(20_000)
        ),
        format!("package p\ntype T[P a{}] int\n", "|a".repeat(10_000)),
    ];
    for source in sources {
        let parsed = parse_gopp(&source);
        assert_eq!(parsed.diagnostics, [], "{}", &source[..40]);
        assert!(parsed.dump.len() > source.len());
    }
}

/// The text form that a JSON node and its children stand for.
fn json_as_text(node: &serde_json::Value, depth: usize, out: &mut String) {
    let text = |key: &str| node[key].as_str();
    out.push_str(&"  ".repeat(depth));
    match text("field") {
        Some(field) => out.push_str(&format!("{field}:")),
        None => out.push_str(text("kind").unwrap()),
    }
    if let Some(rest) = text("text") {
        out.push_str(&format!(" {rest}"));
    }
    if let Some(span) = node["span"].as_array() {
        out.push_str(&format!(" @{}..{}", span[0], span[1]));
    }
    out.push('\n');
    for child in node["children"].as_array().into_iter().flatten() {
        json_as_text(child, depth + 1, out);
    }
}

fn assert_json_is_the_tree(parsed: &Parsed) {
    assert_eq!(parsed.json.lines().count(), 1);
    let mut text = String::new();
    json_as_text(&serde_json::from_str(&parsed.json).unwrap(), 0, &mut text);
    assert_eq!(text, parsed.dump);
}

#[test]
fn the_json_tree_has_the_nodes_of_the_text_tree() {
    for path in samples::files() {
        let dialect = Dialect::from_path(&path).unwrap();
        assert_json_is_the_tree(&parse_bytes(&fs::read(&path).unwrap(), dialect));
    }
    let escapes = "package p\nvar s = \"q\\\"b\\\\\" + `tab\there` + '\\n'\n@A(\"x\")\nfunc f() { g(1 2) \x01 }\n";
    for source in ["", "package", escapes] {
        assert_json_is_the_tree(&parse_gopp(source));
    }
}

#[test]
fn a_long_chain_is_one_json_line() {
    let json = parse_gopp(&stress::long_chain(20_000)).json;
    assert!(json.ends_with("}\n"));
    assert_eq!(json.matches('{').count(), json.matches('}').count());
    assert_eq!(json.matches('[').count(), json.matches(']').count());
    assert!(json.matches("\"kind\":\"Binary\"").count() == 19_999);
}

fn parse_steps(text: &str) -> u64 {
    let map = SourceMap::new();
    let id = map.add(SourceFile::new("test.gopp".into(), text.to_owned(), Dialect::Gopp).unwrap());
    parse_counted(id, &map.file(id), &ThreadedRodeo::new(), &Sink::new(10)).1
}

#[test]
fn token_looks_grow_with_the_input_not_faster() {
    let broken_lines = |count: usize| format!("package p\n{}", "var = ) x := [ }\n".repeat(count));
    let broken_body = |count: usize| {
        let body = "x := ) + ; if { for ] case 1: } else\n".repeat(count);
        format!("package p\nfunc f() {{\n{body}}}\n")
    };
    let unclosed = |count: usize| format!("package p\nvar x = f({}\n", "g(1, ".repeat(count));
    let sizes = [1_000, 2_000, 4_000, 8_000];
    linear::assert_near_linear(&sizes, |n| parse_steps(&stress::wide_block(n)));
    linear::assert_near_linear(&sizes, |n| parse_steps(&stress::long_chain(n)));
    linear::assert_near_linear(&sizes, |n| parse_steps(&stress::diamonds(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| {
        parse_steps(&stress::deep_expr(n))
    });
    linear::assert_near_linear(&sizes, |n| parse_steps(&broken_lines(n)));
    linear::assert_near_linear(&sizes, |n| parse_steps(&broken_body(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| parse_steps(&unclosed(n)));
}

/// Parses the prefixes of every sample whose length is a multiple of `stride`.
fn parse_sample_prefixes(stride: usize) {
    for path in samples::files() {
        let dialect = Dialect::from_path(&path).unwrap();
        let bytes = fs::read(&path).unwrap();
        for end in (0..bytes.len()).step_by(stride) {
            let parsed = parse_bytes(&bytes[..end], dialect);
            assert!(
                parsed.dump.starts_with("File "),
                "{} at {end}",
                path.display()
            );
        }
    }
}

#[test]
fn truncated_samples_parse_to_a_tree() {
    parse_sample_prefixes(17);
}

#[test]
#[ignore = "long: every prefix of every sample"]
fn every_prefix_of_every_sample_parses_to_a_tree() {
    parse_sample_prefixes(1);
}

/// Token soup: fragments that reach every part of the grammar, in any order.
fn source_soup() -> impl Strategy<Value = String> {
    let fragments = prop::sample::select(vec![
        "package p\n",
        "import ",
        "func ",
        "type ",
        "var ",
        "const ",
        "macro ",
        "struct ",
        "interface ",
        "map",
        "chan ",
        "if ",
        "else ",
        "for ",
        "range ",
        "switch ",
        "case ",
        "default",
        "select ",
        "go ",
        "defer ",
        "return ",
        "break ",
        "continue ",
        "goto ",
        "fallthrough",
        "extends ",
        "x",
        "T",
        " ",
        "\n",
        ";",
        ",",
        ".",
        ":",
        ":=",
        "=",
        "==",
        "(",
        ")",
        "[",
        "]",
        "{",
        "}",
        "!",
        "#",
        "@",
        "&",
        "*",
        "<-",
        "...",
        "|",
        "~",
        "+",
        "++",
        "1",
        "\"s\"",
        "'c'",
        "/* c */",
        "// c\n",
        "x.(type)",
        "f!(x)",
        "[]T{}",
    ]);
    prop::collection::vec(fragments, 0..60).prop_map(|fragments| fragments.concat())
}

proptest! {
    #[test]
    fn any_token_sequence_parses_to_a_tree(source in source_soup()) {
        let parsed = parse_gopp(&source);
        prop_assert!(parsed.dump.starts_with("File "));
        let again = parse_gopp(&source);
        prop_assert_eq!(parsed.dump, again.dump);
        prop_assert_eq!(parsed.diagnostics, again.diagnostics);
    }
}
