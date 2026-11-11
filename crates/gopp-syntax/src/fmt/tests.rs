use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use gopp_diag::Sink;
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_testutil::{linear, samples, stress};
use lasso::ThreadedRodeo;

use super::{format_counted, format_once};
use crate::ast::dump_shape;
use crate::lexer;
use crate::parser::parse;
use crate::token::TokenKind;

struct Formatted {
    text: String,
    /// The text after one pass of the layout rules, as one run of gofmt.
    one_pass: String,
    /// The tree of the input without spans and layout.
    shape: String,
    comments: Vec<String>,
}

/// Formats `text`, or returns its first diagnostic.
fn try_format(text: &str, dialect: Dialect) -> Result<Formatted, String> {
    let map = SourceMap::new();
    let source = SourceFile::new("test".into(), text.to_owned(), dialect).unwrap();
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
        one_pass: format_once(&file, &source, &names).0,
        shape: dump_shape(&file, source.text(), &names),
        comments: lexed.comments.iter().map(comment).collect(),
    })
}

fn formatted(text: &str, dialect: Dialect) -> Formatted {
    try_format(text, dialect).unwrap_or_else(|error| panic!("{error}\n{text}"))
}

fn gopp(text: &str) -> String {
    formatted(text, Dialect::Gopp).text
}

fn go(text: &str) -> String {
    formatted(text, Dialect::Go).text
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
fn damage(text: &str, dialect: Dialect, seed: u64) -> String {
    let source = SourceFile::new("damage".into(), text.to_owned(), dialect).unwrap();
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
fn check_properties(text: &str, dialect: Dialect, name: &str) -> String {
    let first = formatted(text, dialect);
    let second = formatted(&first.text, dialect);
    assert!(first.text == second.text, "{name}: not idempotent");
    assert!(first.shape == second.shape, "{name}: the tree changed");
    assert_eq!(first.comments, second.comments, "{name}");
    first.text
}

#[test]
fn every_sample_is_in_canonical_layout() {
    for path in samples::files() {
        let dialect = Dialect::from_path(&path).unwrap();
        let text = fs::read_to_string(&path).unwrap();
        let formatted = check_properties(&text, dialect, &path.display().to_string());
        assert!(formatted == text, "{} is not formatted", path.display());
    }
}

#[test]
fn damaged_samples_keep_their_tree_and_comments() {
    for (index, path) in samples::files().iter().enumerate() {
        let dialect = Dialect::from_path(path).unwrap();
        let text = fs::read_to_string(path).unwrap();
        for seed in 0..5 {
            let damaged = damage(&text, dialect, (index * 5 + seed) as u64);
            let name = format!("{}, damage {seed}", path.display());
            let formatted = check_properties(&damaged, dialect, &name);
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
    // To Go these are paths like any other.
    let as_go = "package p\n\nimport (\n\t\"<algorithm>\"\n\t\"<vector>\"\n\t\"a\"\n\t\"b/zlib.h\"\n\t\"fmt\"\n\t\"os\"\n)\n";
    assert_eq!(go(source), as_go);
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

#[test]
fn build_lines_move_to_the_top_in_canonical_form() {
    let source = "// Copyright.\n\npackage p\n\n//go:build (linux&&amd64)||darwin\n// +build linux\n\nvar x int\n";
    let expected = "// Copyright.\n\n//go:build (linux && amd64) || darwin\n// +build linux,amd64 darwin\n\npackage p\n\nvar x int\n";
    assert_eq!(go(source), expected);
    assert_eq!(go(expected), expected);
}

fn go_root() -> Option<PathBuf> {
    let output = Command::new("go").args(["env", "GOROOT"]).output().ok()?;
    let root = String::from_utf8(output.stdout).ok()?;
    Some(Path::new(root.trim()).join("src"))
}

fn go_files_under(dir: &Path, files: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            go_files_under(&path, files);
        } else if path.extension().is_some_and(|extension| extension == "go") {
            files.push(path);
        }
    }
}

/// The files among `paths` that `gofmt` would change or cannot parse.
fn not_in_gofmt_layout(paths: &[PathBuf]) -> HashSet<PathBuf> {
    let mut rejected = HashSet::new();
    for chunk in paths.chunks(500) {
        let output = Command::new("gofmt")
            .arg("-l")
            .args(chunk)
            .output()
            .unwrap();
        let listed = String::from_utf8_lossy(&output.stdout).into_owned();
        rejected.extend(listed.lines().map(PathBuf::from));
        // A syntax error is reported as `path:line:column: message`.
        let errors = String::from_utf8_lossy(&output.stderr).into_owned();
        let paths = errors.lines().filter_map(|line| line.split_once(".go:"));
        rejected.extend(paths.map(|(path, _)| PathBuf::from(format!("{path}.go"))));
    }
    rejected
}

/// Where `actual`, the formatter's text for `path`, first differs from
/// `expected`.
fn difference(path: &Path, actual: &str, expected: &str) -> Option<String> {
    let mut lines = actual.lines().zip(expected.lines()).enumerate();
    let (line, (got, want)) = match lines.find(|(_, (got, want))| got != want) {
        Some(found) => found,
        None if actual == expected => return None,
        None => (
            actual.lines().count().min(expected.lines().count()),
            ("", ""),
        ),
    };
    let line = line + 1;
    Some(format!(
        "{}:{line}:\n  gofmt: {want:?}\n  gopp:  {got:?}",
        path.display()
    ))
}

/// How the file at `path`, which is in gofmt's layout, comes out changed.
fn change(path: &Path) -> Option<String> {
    let text = fs::read_to_string(path).unwrap();
    match try_format(&text, Dialect::Go) {
        Ok(formatted) => difference(path, &formatted.text, &text),
        Err(error) => Some(format!("{}: does not parse: {error}", path.display())),
    }
}

fn assert_no_failures(failures: &[String], tried: usize) {
    assert!(
        failures.is_empty(),
        "{} of {tried} files differ:\n{}",
        failures.len(),
        failures[..failures.len().min(40)].join("\n")
    );
}

/// Files of the standard library that between them have every kind of
/// declaration, statement, expression and comment.
const STANDARD_LIBRARY_SUBSET: &[&str] = &[
    "bufio/bufio.go",
    "bytes/buffer.go",
    "container/heap/heap.go",
    "context/context.go",
    "crypto/sha256/sha256.go",
    "encoding/binary/binary.go",
    "encoding/json/decode.go",
    "errors/wrap.go",
    "fmt/print.go",
    "go/ast/ast.go",
    "go/printer/nodes.go",
    "go/token/token.go",
    "io/io.go",
    "maps/maps.go",
    "math/bits/bits.go",
    "net/ip.go",
    "os/file.go",
    "path/filepath/path.go",
    "regexp/syntax/prog.go",
    "runtime/chan.go",
    "runtime/select.go",
    "slices/sort.go",
    "sort/sort.go",
    "strconv/atoi.go",
    "strings/builder.go",
    "sync/once.go",
    "syscall/syscall_linux.go",
    "text/tabwriter/tabwriter.go",
    "time/format.go",
    "unicode/utf8/utf8.go",
];

#[test]
fn standard_library_files_in_gofmt_layout_are_unchanged() {
    let Some(root) = go_root() else {
        eprintln!("skipped: `go` is not on PATH");
        return;
    };
    let files = STANDARD_LIBRARY_SUBSET.iter().map(|file| root.join(file));
    let files: Vec<PathBuf> = files.filter(|path| path.exists()).collect();
    assert!(files.len() >= 20, "only {} of the files exist", files.len());
    let rejected = not_in_gofmt_layout(&files);
    let mut failures = Vec::new();
    for path in files.iter().filter(|path| !rejected.contains(*path)) {
        failures.extend(change(path));
    }
    assert_no_failures(&failures, files.len());
}

/// Files that `gofmt` accepts and this parser, like Go's compiler, does
/// not: `func ( /* receiver type */ ) f0()` has no receiver.
const KNOWN_REJECTED: &[&str] = &["go/doc/testdata/issue17788.go"];

/// Every file under GOROOT that `gofmt` leaves as it is must come out of
/// the formatter as it is. Needs `go` and `gofmt` on `PATH`.
#[test]
#[ignore = "long: formats the whole Go standard library"]
fn the_go_standard_library_is_unchanged() {
    let mut files = Vec::new();
    go_files_under(&go_root().unwrap(), &mut files);
    files.sort();
    assert!(
        files.len() > 5000,
        "only {} files under GOROOT",
        files.len()
    );
    let rejected = not_in_gofmt_layout(&files);
    let known = |path: &PathBuf| KNOWN_REJECTED.iter().any(|known| path.ends_with(known));
    files.retain(|path| !rejected.contains(path) && !known(path));

    let mut failures = Vec::new();
    for path in &files {
        failures.extend(change(path));
    }
    println!(
        "{} files in gofmt layout, {} unchanged",
        files.len(),
        files.len() - failures.len()
    );
    assert_no_failures(&failures, files.len());
}

/// The layout rules themselves: on text whose layout is damaged, the
/// formatter and `gofmt` must agree.
#[test]
#[ignore = "long: runs gofmt on damaged files of the Go standard library"]
fn damaged_standard_library_files_come_out_as_gofmt_writes_them() {
    const STRIDE: usize = 12;
    let mut files = Vec::new();
    go_files_under(&go_root().unwrap(), &mut files);
    files.sort();
    let rejected = not_in_gofmt_layout(&files);
    let known = |path: &PathBuf| KNOWN_REJECTED.iter().any(|known| path.ends_with(known));
    files.retain(|path| !rejected.contains(path) && !known(path));
    let files: Vec<&PathBuf> = files.iter().step_by(STRIDE).collect();
    assert!(files.len() >= 300, "only {} files", files.len());

    let dir = std::env::temp_dir().join(format!("gopp-fmt-damaged-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let mut damaged = Vec::new();
    for (index, path) in files.iter().enumerate() {
        let text = damage(
            &fs::read_to_string(path).unwrap(),
            Dialect::Go,
            index as u64,
        );
        fs::write(dir.join(format!("{index}.go")), &text).unwrap();
        damaged.push(text);
    }
    let gofmt = |flag: &str| Command::new("gofmt").arg(flag).arg(&dir).output().unwrap();
    let read = |index: usize| fs::read_to_string(dir.join(format!("{index}.go"))).unwrap();
    // Damage can make a file that gofmt refuses: a comment that starts
    // with `//line` is a directive once it is in the first column.
    let errors = String::from_utf8_lossy(&gofmt("-w").stderr).into_owned();
    let refused = errors.lines().filter_map(|line| line.split_once(".go:"));
    let refused: HashSet<PathBuf> = refused
        .map(|(path, _)| PathBuf::from(format!("{path}.go")))
        .collect();
    assert!(refused.len() * 100 < files.len(), "gofmt failed: {errors}");
    let one_run: Vec<String> = (0..files.len()).map(read).collect();
    // On some of its own results gofmt changes its mind; it is run until
    // it does not.
    for _ in 0..8 {
        if gofmt("-l").stdout.is_empty() {
            break;
        }
        gofmt("-w");
    }
    let settled: Vec<String> = (0..files.len()).map(read).collect();
    fs::remove_dir_all(&dir).unwrap();

    let mut failures = Vec::new();
    let mut repeated = 0;
    for (index, path) in files.iter().enumerate() {
        if refused.contains(&dir.join(format!("{index}.go"))) {
            continue;
        }
        repeated += usize::from(one_run[index] != settled[index]);
        let formatted = match try_format(&damaged[index], Dialect::Go) {
            Ok(formatted) => formatted,
            Err(error) => {
                failures.push(format!("{}: does not parse: {error}", path.display()));
                continue;
            }
        };
        failures.extend(difference(path, &formatted.one_pass, &one_run[index]));
        failures.extend(difference(path, &formatted.text, &settled[index]));
        let problem = match try_format(&formatted.text, Dialect::Go) {
            Err(error) => format!("the result does not parse: {error}"),
            Ok(again) if again.text != formatted.text => "not idempotent".to_owned(),
            Ok(again) if again.shape != formatted.shape => "the tree changed".to_owned(),
            Ok(_) => continue,
        };
        failures.push(format!("{}: {problem}", path.display()));
    }
    let compared = files.len() - refused.len();
    println!("{compared} damaged files, {repeated} that gofmt formats more than once");
    assert_no_failures(&failures, compared);
}

/// The steps of one pass over `text`.
fn steps(text: &str) -> u64 {
    let map = SourceMap::new();
    let source = SourceFile::new("test".into(), text.to_owned(), Dialect::Gopp).unwrap();
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
