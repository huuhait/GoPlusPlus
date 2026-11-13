use std::fs;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{SourceFile, SourceMap};
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

fn parse_bytes(bytes: &[u8]) -> Parsed {
    let map = SourceMap::new();
    let id = map.add(SourceFile::from_bytes("test.gopp".into(), bytes).unwrap());
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
    parse_bytes(text.as_bytes())
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
        let parsed = parse_bytes(&fs::read(&path).unwrap());
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
fn forms_that_need_care() {
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
    let source = SourceFile::new("t".into(), text.to_owned()).unwrap();
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

// rule: EXP-22
#[test]
fn an_index_list_holds_expressions() {
    let text = "package p\nfunc f() {\n\tm[0, i+1] = m[i, j] + g[int, []string](x)[k, 2]\n\
                \t_ = h[a, <-chan int, func(int) bool, *T,]\n}\n";
    let parsed = parse_gopp(text);
    assert_eq!(parsed.diagnostics, []);
    assert!(!parsed.has_bad_node(), "{}", parsed.dump);
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
        assert_json_is_the_tree(&parse_bytes(&fs::read(&path).unwrap()));
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
    let id = map.add(SourceFile::new("test.gopp".into(), text.to_owned()).unwrap());
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
        let bytes = fs::read(&path).unwrap();
        for end in (0..bytes.len()).step_by(stride) {
            let parsed = parse_bytes(&bytes[..end]);
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
