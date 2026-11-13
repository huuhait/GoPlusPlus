use std::fs;

use gopp_diag::Sink;
use gopp_span::{SourceFile, SourceMap};
use gopp_testutil::{linear, samples, stress};
use lasso::ThreadedRodeo;

use super::{format_counted, format_once};
use crate::ast::dump_shape;
use crate::lexer;
use crate::parser::parse;
use crate::token::TokenKind;

struct Formatted {
    text: String,
    /// The tree of the input without spans and layout.
    shape: String,
    comments: Vec<String>,
}

/// Formats `text`, or returns its first diagnostic.
fn try_format(text: &str) -> Result<Formatted, String> {
    let map = SourceMap::new();
    let source = SourceFile::new("test".into(), text.to_owned()).unwrap();
    let id = map.add(source);
    let source = map.file(id);
    let names = ThreadedRodeo::new();
    let sink = Sink::new(10);
    let file = parse(id, &source, &names, &sink);
    if let Some(first) = sink.finish().first() {
        return Err(format!("@{}: {}", first.span.lo, first.message));
    }
    let (formatted, _) = format_counted(&file, &source, &names);
    let lexed = lexer::lex(id, &source, &Sink::new(10));
    let comment = |comment: &crate::token::Comment| {
        let text = &source.text()[comment.lo as usize..comment.hi as usize];
        // The formatter indents the lines of a block comment anew.
        let lines: Vec<&str> = text.lines().map(str::trim).collect();
        lines.join("\n")
    };
    Ok(Formatted {
        text: formatted,
        shape: dump_shape(&file, source.text(), &names),
        comments: lexed.comments.iter().map(comment).collect(),
    })
}

fn formatted(text: &str) -> Formatted {
    try_format(text).unwrap_or_else(|error| panic!("{error}\n{text}"))
}

fn gopp(text: &str) -> String {
    formatted(text).text
}

/// A random number generator with a fixed sequence per seed.
struct Random(u64);

impl Random {
    fn below(&mut self, bound: u64) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0 % bound
    }
}

/// Changes the layout of `text` and no token. It removes the indentation,
/// joins runs of blanks, removes and adds blank lines, breaks and joins
/// lines after tokens that do not end a statement, or adds comments between
/// tokens; the seed decides which of these it does.
fn damage(text: &str, seed: u64) -> String {
    let source = SourceFile::new("damage".into(), text.to_owned()).unwrap();
    let map = SourceMap::new();
    let id = map.add(source);
    let lexed = lexer::lex(id, &map.file(id), &Sink::new(10));
    let literals = lexed
        .tokens
        .iter()
        .filter(|token| matches!(token.kind, TokenKind::String | TokenKind::Rune))
        .map(|token| (token.lo as usize, token.hi as usize));
    let comments = lexed
        .comments
        .iter()
        .map(|comment| (comment.lo as usize, comment.hi as usize));
    let mut kept: Vec<(usize, usize)> = literals.chain(comments).collect();
    kept.sort_unstable();

    let mut random = Random(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1);
    let (blanks, lines, breaks, comments) = match seed % 5 {
        0 => (true, false, false, false),
        1 => (false, true, false, false),
        2 => (true, true, false, false),
        3 => (true, false, true, false),
        _ => (false, false, false, true),
    };
    let bytes = text.as_bytes();

    // The gaps where a line end is not a semicolon: after a token that
    // does not end a statement, with only white space up to the next one.
    let mut gaps = Vec::new();
    // Comments to add after a token: a line comment only where the line
    // end after it is no semicolon.
    let mut added: Vec<(usize, &str)> = Vec::new();
    for pair in lexed.tokens.windows(2) {
        let (lo, hi) = (pair[0].hi as usize, pair[1].lo as usize);
        let inserted = pair[1].kind == TokenKind::Semi && &text[hi..pair[1].hi as usize] != ";";
        let free = !pair[0].kind.ends_statement()
            && pair[0].lo < pair[0].hi
            && pair[1].kind != TokenKind::Eof
            && !inserted
            && bytes[lo..hi].iter().all(u8::is_ascii_whitespace);
        if breaks && free {
            gaps.push((lo, hi));
        }
        if comments && pair[0].lo < pair[0].hi && random.below(12) == 0 {
            let line_comment = free && random.below(2) == 0;
            added.push((lo, if line_comment { " // c\n" } else { " /* c */ " }));
        }
    }
    let mut next_added = 0;
    let mut next_gap = 0;

    let mut out = Vec::with_capacity(bytes.len());
    let mut next_kept = 0;
    let mut line_start = true;
    let mut at = 0;
    while at < bytes.len() {
        while gaps.get(next_gap).is_some_and(|&(lo, _)| lo < at) {
            next_gap += 1;
        }
        while added.get(next_added).is_some_and(|&(lo, _)| lo <= at) {
            if added[next_added].0 == at {
                out.extend_from_slice(added[next_added].1.as_bytes());
                line_start = false;
            }
            next_added += 1;
        }
        if let Some(&(lo, hi)) = gaps.get(next_gap).filter(|&&(lo, _)| lo == at) {
            next_gap += 1;
            let broken = bytes[lo..hi].contains(&b'\n');
            let choice = random.below(6);
            if choice == 0 && !broken {
                out.push(b'\n');
                line_start = true;
                at = hi;
                continue;
            }
            if choice < 3 && broken {
                out.push(b' ');
                at = hi;
                continue;
            }
        }
        if kept.get(next_kept).is_some_and(|&(lo, _)| lo == at) {
            let (lo, hi) = kept[next_kept];
            out.extend_from_slice(&bytes[lo..hi]);
            next_kept += 1;
            line_start = false;
            at = hi;
            continue;
        }
        match bytes[at] {
            b'\n' => {
                let blank_line = line_start && out.last().is_none_or(|&last| last == b'\n');
                if !(lines && blank_line && random.below(2) == 0) {
                    out.push(b'\n');
                }
                if lines && random.below(8) == 0 {
                    out.push(b'\n');
                }
                line_start = true;
                at += 1;
            }
            b' ' | b'\t' if blanks => {
                while bytes
                    .get(at)
                    .is_some_and(|&byte| byte == b' ' || byte == b'\t')
                {
                    at += 1;
                }
                if !line_start && bytes.get(at).is_some_and(|&byte| byte != b'\n') {
                    out.push(b' ');
                }
            }
            byte => {
                out.push(byte);
                line_start &= byte == b' ' || byte == b'\t';
                at += 1;
            }
        }
    }
    String::from_utf8(out).unwrap()
}

/// What the formatter promises for `text`: formatting again changes
/// nothing, and the tree and the comments are those of `text`.
fn check_properties(text: &str, name: &str) -> String {
    let first = formatted(text);
    let second = formatted(&first.text);
    assert!(first.text == second.text, "{name}: not idempotent");
    assert!(first.shape == second.shape, "{name}: the tree changed");
    assert_eq!(first.comments, second.comments, "{name}");
    first.text
}

#[test]
fn every_sample_is_in_canonical_layout() {
    for path in samples::files() {
        let text = fs::read_to_string(&path).unwrap();
        let formatted = check_properties(&text, &path.display().to_string());
        assert!(formatted == text, "{} is not formatted", path.display());
    }
}

#[test]
fn damaged_samples_keep_their_tree_and_comments() {
    for (index, path) in samples::files().iter().enumerate() {
        let text = fs::read_to_string(path).unwrap();
        for seed in 0..5 {
            let damaged = damage(&text, (index * 5 + seed) as u64);
            let name = format!("{}, damage {seed}", path.display());
            let formatted = check_properties(&damaged, &name);
            // Without changed blank lines the sample itself comes back.
            if seed == 0 {
                assert!(formatted == text, "{name}");
            }
        }
    }
}

#[test]
fn attributes_go_on_their_own_lines_before_the_declaration() {
    let source = "package p\n\n// T is documented.\n@Derive(JSON,Equal) @Packed\n\n\ntype T struct{\nName string @JSON(\"name\") `db:\"name\"`\nAddress string @Required // where\nn int\n}\n\n@Inline func f(){}\n@A\nvar (\nx = 1\n)\n";
    let expected = "package p\n\n// T is documented.\n@Derive(JSON, Equal)\n@Packed\ntype T struct {\n\tName    string @JSON(\"name\") `db:\"name\"`\n\tAddress string @Required     // where\n\tn       int\n}\n\n@Inline\nfunc f() {}\n\n@A\nvar (\n\tx = 1\n)\n";
    assert_eq!(gopp(source), expected);
    assert_eq!(gopp(expected), expected);
}

#[test]
fn a_comment_under_an_attribute_stays_attached() {
    let source = "package p\n\n@Inline\n\n// About f.\n\nfunc f() {}\n\n@A // why\n@B\n\n/* c */\nvar x int\n";
    let expected =
        "package p\n\n@Inline\n// About f.\nfunc f() {}\n\n@A // why\n@B\n/* c */\nvar x int\n";
    assert_eq!(gopp(source), expected);
    assert_eq!(gopp(expected), expected);
}

#[test]
fn the_unwrapping_mark_stays_with_its_call() {
    let source = "package p\n\nfunc load ! (path string) ([]byte,error) {\nf := os.Open !(path)\nreturn io.ReadAll!(\nf,\n), nil\n}\n\ntype R interface{ Read ! (p []byte) int }\n\nvar g = func ! () {}\n";
    let expected = "package p\n\nfunc load!(path string) ([]byte, error) {\n\tf := os.Open!(path)\n\treturn io.ReadAll!(\n\t\tf,\n\t), nil\n}\n\ntype R interface{ Read!(p []byte) int }\n\nvar g = func!() {}\n";
    assert_eq!(gopp(source), expected);
    assert_eq!(gopp(expected), expected);
}

#[test]
fn macros_are_laid_out_as_functions_and_calls() {
    let source = "package p\n\nmacro Swap(a expr,b expr){\ntmp:=a\na=b\nb=tmp\n}\nmacro Log(format lit,args ...expr){}\nmacro func Derive(t ast.Type)[]ast.Decl{return nil}\n#Register(1,2)\nfunc f(){\n#Swap(x,y)\nz:=#pkg.Max(x,\ny)\n}\n";
    let expected = "package p\n\nmacro Swap(a expr, b expr) {\n\ttmp := a\n\ta = b\n\tb = tmp\n}\nmacro Log(format lit, args ...expr)      {}\nmacro func Derive(t ast.Type) []ast.Decl { return nil }\n\n#Register(1, 2)\n\nfunc f() {\n\t#Swap(x, y)\n\tz := #pkg.Max(x,\n\t\ty)\n}\n";
    assert_eq!(gopp(source), expected);
    assert_eq!(gopp(expected), expected);
}

#[test]
fn header_imports_keep_their_order() {
    let source = "package p\n\nimport (\n\"os\"\n\"fmt\"\n\"<vector>\"\n\"<algorithm>\"\n\"b/zlib.h\"\n\"a\"\n\"a\"\n)\n";
    let sorted = "package p\n\nimport (\n\t\"fmt\"\n\t\"os\"\n\t\"<vector>\"\n\t\"<algorithm>\"\n\t\"b/zlib.h\"\n\t\"a\"\n)\n";
    assert_eq!(gopp(source), sorted);
}

#[test]
fn a_build_condition_stands_above_what_it_is_for() {
    let source = "// Licence.\n@Build( linux&&!arm64 )\n// Package p.\npackage p\n\
                  @Build(windows) import \"<windows.h>\"\n@Build(linux)\n\nimport (\n\"b\"\n\"a\"\n)\n\
                  @Build(linux||(unix&&debug))\nfunc f(){}\n";
    let expected = "// Licence.\n@Build(linux && !arm64)\n\n// Package p.\npackage p\n\n\
                    @Build(windows)\nimport \"<windows.h>\"\n@Build(linux)\nimport (\n\t\"a\"\n\t\"b\"\n)\n\n\
                    @Build(linux || (unix && debug))\nfunc f() {}\n";
    assert_eq!(gopp(source), expected);
    assert_eq!(gopp(expected), expected);
    let short = "@Build(linux)\n\npackage p\n";
    assert_eq!(gopp("@Build(linux)\npackage p\n"), short);
    assert_eq!(gopp("@Build(linux)\n\n\n\npackage p\n"), short);
}

#[test]
fn groups_without_declarations_are_kept() {
    let source = "package p\n\nimport()\nconst (\n)\n\nvar ()\nfunc f(){\nvar()\n}\n";
    let expected = "package p\n\nimport ()\n\nconst ()\n\nvar ()\n\nfunc f() {\n\tvar ()\n}\n";
    assert_eq!(gopp(source), expected);
    assert_eq!(gopp(expected), expected);
}

/// The steps of one pass over `text`.
fn steps(text: &str) -> u64 {
    let map = SourceMap::new();
    let source = SourceFile::new("test".into(), text.to_owned()).unwrap();
    let id = map.add(source);
    let source = map.file(id);
    let names = ThreadedRodeo::new();
    let sink = Sink::new(10);
    let file = parse(id, &source, &names, &sink);
    assert_eq!(sink.finish(), []);
    format_once(&file, &source, &names).1
}

fn in_function(body: &str) -> String {
    format!("package p\n\nfunc f() {{\n{body}}}\n")
}

#[test]
fn steps_grow_with_the_input_not_faster() {
    let sizes = [500, 1_000, 2_000, 4_000];
    let repeated = |line: &str, count: usize| line.repeat(count);
    let aligned_fields = |count: usize| {
        let fields = repeated(
            "\tname int `tag` // comment\n\tlonger, names string // other\n",
            count,
        );
        format!("package p\n\ntype T struct {{\n{fields}}}\n")
    };
    let aligned_values = |count: usize| {
        let specs = repeated("\ta = 1 // one\n\tlonger int = 2 /* two */\n", count);
        format!("package p\n\nconst (\n{specs})\n")
    };
    let trailing_comments =
        |count: usize| in_function(&repeated("\tx = 1 // one\n\tlonger := 22 // two\n", count));
    let key_values = |count: usize| {
        in_function(&format!(
            "\tm := M{{\n{}\t}}\n",
            repeated("\t\tkey: {1, 2},\n\t\tlonger: nil,\n", count)
        ))
    };
    let arguments = |count: usize| {
        in_function(&format!(
            "\tg(\n{}\t)\n",
            repeated("\t\th(1, x+y*z),\n", count)
        ))
    };
    let method_chain =
        |count: usize| in_function(&format!("\tx{}\n", repeated(".\n\t\tf(1)", count)));
    let else_chain = |count: usize| {
        in_function(&format!(
            "\tif a {{\n\t}}{}\n",
            repeated(" else if b {\n\t}", count)
        ))
    };
    let nested_calls = |count: usize| {
        in_function(&format!(
            "\tx := {}1{}\n",
            repeated("g(a, ", count),
            repeated(",\n)", count)
        ))
    };
    let import_block = |count: usize| {
        let imports: String = (0..count)
            .rev()
            .map(|n| format!("\t\"p/{n}\" // {n}\n"))
            .collect();
        format!("package p\n\nimport (\n{imports})\n")
    };

    linear::assert_near_linear(&sizes, |n| steps(&stress::wide_block(n)));
    linear::assert_near_linear(&sizes, |n| steps(&stress::long_chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(&stress::diamonds(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(&stress::deep_expr(n)));
    linear::assert_near_linear(&sizes, |n| steps(&aligned_fields(n)));
    linear::assert_near_linear(&sizes, |n| steps(&aligned_values(n)));
    linear::assert_near_linear(&sizes, |n| steps(&trailing_comments(n)));
    linear::assert_near_linear(&sizes, |n| steps(&key_values(n)));
    linear::assert_near_linear(&sizes, |n| steps(&arguments(n)));
    linear::assert_near_linear(&sizes, |n| steps(&method_chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(&else_chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(&import_block(n)));
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(&nested_calls(n)));
}
