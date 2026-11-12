//! Fuzz targets: plain functions from bytes to a verdict, for any engine.

use std::fmt;

use gopp_diag::{Diagnostic, Sink, render_json, render_text};
use gopp_span::{Dialect, FileId, SourceFile, SourceMap, Span};
use gopp_syntax::ast::{dump, dump_json, dump_shape};
use gopp_syntax::fmt::format;
use gopp_syntax::parser::parse;
use lasso::ThreadedRodeo;

/// Diagnostics kept per input.
const MAX_DIAGNOSTICS: usize = 10;

/// What an oracle found wrong with one input.
#[derive(Debug, PartialEq, Eq)]
pub struct Finding {
    pub oracle: &'static str,
    pub detail: String,
}

impl fmt::Display for Finding {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.oracle, self.detail)
    }
}

pub type Verdict = Result<(), Finding>;

/// Lexes, parses and dumps `data` as a Go++ file.
pub fn parse_gopp(data: &[u8]) -> Verdict {
    front_end(data, Dialect::Gopp)
}

/// Lexes, parses and dumps `data` as a Go file.
pub fn parse_go(data: &[u8]) -> Verdict {
    front_end(data, Dialect::Go)
}

/// How much of a symbol is written out as Go++ text.
const MAX_READABLE: usize = 4096;

/// Reads `data` as a symbol name: it is refused, or it is the one name
/// of what it names.
pub fn symbol_name(data: &[u8]) -> Verdict {
    let mut context = gopp_mangle::Context::new();
    let Ok(entity) = gopp_mangle::parse(&mut context, data) else {
        return Ok(());
    };
    if context.symbol(entity).as_bytes() != data {
        return finding("symbol", "what the name names has another name");
    }
    let readable = context.readable(entity, MAX_READABLE);
    if readable.len() > MAX_READABLE {
        return finding("symbol", "the readable name is longer than asked");
    }
    if gopp_mangle::demangle(data, MAX_READABLE).ok() != Some(readable) {
        return finding("determinism", "two runs gave different results");
    }
    Ok(())
}

fn front_end(data: &[u8], dialect: Dialect) -> Verdict {
    let Some(first) = run(data, dialect) else {
        return Ok(());
    };
    if !first.dump.starts_with("File ") {
        return finding("tree", "the dump does not start with a file node");
    }
    check_diagnostics(&first.diagnostics, first.id, &first.text)?;
    for diagnostic in &first.diagnostics {
        render_text(diagnostic, &first.map, false);
        render_json(diagnostic, &first.map);
    }
    let same = |second: Run| second.dump == first.dump && second.diagnostics == first.diagnostics;
    if !run(data, dialect).is_some_and(same) {
        return finding("determinism", "two runs gave different results");
    }
    check_format(&first, dialect)
}

/// A file without a diagnostic is formatted: the result must parse to the
/// same tree, and formatting it again must change nothing.
fn check_format(first: &Run, dialect: Dialect) -> Verdict {
    let Some(formatted) = &first.formatted else {
        return Ok(());
    };
    let Some(second) = run(formatted.as_bytes(), dialect) else {
        return finding("format", "the formatted text is too large");
    };
    if let Some(diagnostic) = second.diagnostics.first() {
        let detail = format!("the formatted text does not parse: {}", diagnostic.message);
        return finding("format", detail);
    }
    if second.shape != first.shape {
        return finding("format", "the formatted text has another tree");
    }
    if second.formatted.as_ref() != Some(formatted) {
        return finding("format", "formatting twice gives another text");
    }
    Ok(())
}

struct Run {
    id: FileId,
    text: String,
    dump: String,
    /// The tree without what the formatter may change.
    shape: String,
    /// The text in canonical layout, if there is no diagnostic.
    formatted: Option<String>,
    diagnostics: Vec<Diagnostic>,
    map: SourceMap,
}

/// `None` for an input over the size limit of a source file.
fn run(data: &[u8], dialect: Dialect) -> Option<Run> {
    let map = SourceMap::new();
    let id = map.add(SourceFile::from_bytes("fuzz".into(), data, dialect).ok()?);
    let source = map.file(id);
    let names = ThreadedRodeo::new();
    let sink = Sink::new(MAX_DIAGNOSTICS);
    let file = parse(id, &source, &names, &sink);
    let dump = dump(&file, source.text(), &names);
    dump_json(&file, source.text(), &names);
    let diagnostics = sink.finish();
    let formatted = diagnostics
        .is_empty()
        .then(|| format(&file, &source, &names));
    Some(Run {
        id,
        text: source.text().to_owned(),
        dump,
        shape: dump_shape(&file, source.text(), &names),
        formatted,
        diagnostics,
        map,
    })
}

fn finding(oracle: &'static str, detail: impl Into<String>) -> Verdict {
    Err(Finding {
        oracle,
        detail: detail.into(),
    })
}

/// Diagnostics are in source order, within the cap, and every span lies in
/// the file on character boundaries.
fn check_diagnostics(diagnostics: &[Diagnostic], id: FileId, text: &str) -> Verdict {
    let in_file = |span: Span| {
        let (lo, hi) = (span.lo as usize, span.hi as usize);
        span.file == id && lo <= hi && text.is_char_boundary(lo) && text.is_char_boundary(hi)
    };
    if diagnostics.len() > MAX_DIAGNOSTICS + 1 {
        let detail = format!(
            "{} diagnostics, the cap is {MAX_DIAGNOSTICS}",
            diagnostics.len()
        );
        return finding("diagnostics", detail);
    }
    for diagnostic in diagnostics {
        let notes = diagnostic.notes.iter().filter_map(|note| note.span);
        if let Some(span) = std::iter::once(diagnostic.span)
            .chain(notes)
            .find(|&span| !in_file(span))
        {
            let detail = format!(
                "span {}..{} of {:?} is not in the file",
                span.lo, span.hi, diagnostic.message
            );
            return finding("diagnostics", detail);
        }
    }
    if !diagnostics.is_sorted_by_key(|diagnostic| diagnostic.span.lo) {
        return finding("diagnostics", "not in source order");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use gopp_diag::Code;

    use super::*;

    fn checked(spans: &[(u32, u32)], text: &str) -> Verdict {
        let map = SourceMap::new();
        let source = SourceFile::new("t.gopp".into(), text.to_owned(), Dialect::Gopp).unwrap();
        let file = map.add(source);
        let diagnostics: Vec<_> = spans
            .iter()
            .map(|&(lo, hi)| Diagnostic::new(Code::LexIllegalChar, Span { file, lo, hi }, "m"))
            .collect();
        check_diagnostics(&diagnostics, file, text)
    }

    #[test]
    fn a_valid_file_and_a_broken_file_pass() {
        assert_eq!(
            parse_gopp(b"package p\n\nfunc f() int { return 1 }\n"),
            Ok(())
        );
        assert_eq!(parse_gopp(b"package p\nfunc ( { ] \xff \0 'x"), Ok(()));
        assert_eq!(parse_go(b"package p\n@A\nfunc f() {}\n"), Ok(()));
        assert_eq!(parse_gopp(b""), Ok(()));
    }

    #[test]
    fn a_symbol_and_what_is_none_pass() {
        assert_eq!(symbol_name(b"_GPTo6op_AddN4mainE4Vec2LB12_E"), Ok(()));
        assert_eq!(symbol_name(b"_GPTo6op_AddN4mainE4Vec2LB13_E"), Ok(()));
        assert_eq!(symbol_name(b"\xff_GP\0"), Ok(()));
        assert_eq!(symbol_name(b""), Ok(()));
    }

    #[test]
    fn spans_in_the_file_in_order_pass() {
        assert_eq!(checked(&[(0, 1), (0, 3), (3, 3)], "a\u{e9}"), Ok(()));
    }

    #[test]
    fn a_bad_span_is_a_finding() {
        for spans in [[(0, 4)], [(2, 1)], [(2, 3)]] {
            let verdict = checked(&spans, "a\u{e9}");
            assert_eq!(verdict.unwrap_err().oracle, "diagnostics", "{spans:?}");
        }
    }

    #[test]
    fn diagnostics_out_of_order_or_over_the_cap_are_findings() {
        let unordered = checked(&[(1, 1), (0, 1)], "ab").unwrap_err();
        assert_eq!(unordered.detail, "not in source order");
        let many = checked(&[(0, 1); MAX_DIAGNOSTICS + 2], "ab").unwrap_err();
        assert_eq!(many.detail, "12 diagnostics, the cap is 10");
    }
}
