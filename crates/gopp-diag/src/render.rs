//! Text and JSON output of diagnostics.

use std::sync::Arc;

use ariadne::{Color, Config, Fmt, IndexType, Label, Report, ReportKind};
use gopp_span::{SourceFile, SourceMap, Span};
use serde::Serialize;

use crate::{Code, Diagnostic, Severity};

type Place = (String, std::ops::Range<usize>);

/// The diagnostic as `path:line:column: error[E0006]: message` followed by
/// the source lines it points at. Notes that have a place are labels in the
/// excerpt.
pub fn render_text(diagnostic: &Diagnostic, sources: &SourceMap, color: bool) -> String {
    let severity = diagnostic.code.severity();
    let severity_color = match severity {
        Severity::Error => Color::Red,
        Severity::Warning => Color::Yellow,
        Severity::Note => Color::Cyan,
    };
    let spans = std::iter::once(diagnostic.span)
        .chain(diagnostic.notes.iter().filter_map(|note| note.span));
    let mut files: Vec<(String, Arc<SourceFile>)> = Vec::new();
    for span in spans {
        let file = sources.file(span.file);
        let id = file.path().display().to_string();
        if files.iter().all(|(known, _)| *known != id) {
            files.push((id, file));
        }
    }
    let place = |span: Span| -> Place {
        let id = sources.file(span.file).path().display().to_string();
        (id, span.lo as usize..span.hi as usize)
    };

    let config = Config::default()
        .with_index_type(IndexType::Byte)
        .with_color(color);
    let mut report = Report::build(ReportKind::Error, place(diagnostic.span))
        .with_config(config)
        .with_label(
            Label::new(place(diagnostic.span))
                .with_message("here")
                .with_color(severity_color),
        );
    for note in &diagnostic.notes {
        match note.span {
            Some(span) => report.add_label(
                Label::new(place(span))
                    .with_message(&note.message)
                    .with_color(Color::Cyan),
            ),
            None => report.add_note(&note.message),
        }
    }

    let cache = ariadne::sources(files.iter().map(|(id, file)| (id.clone(), file.text())));
    let mut out = Vec::new();
    // Writing to a Vec cannot fail, and every label's file is in the cache.
    #[allow(clippy::expect_used)]
    report
        .finish()
        .write(cache, &mut out)
        .expect("report rendering failed");
    let excerpt = String::from_utf8_lossy(&out);
    // The header is written here: ariadne's has no position, and it colors a
    // custom report kind even when color is off.
    let excerpt = excerpt.split_once('\n').map_or("", |(_, rest)| rest);
    let location = sources.location(diagnostic.span);
    let kind = format!("{severity}[{}]:", diagnostic.code);
    let kind = kind.fg(color.then_some(severity_color));
    format!("{location}: {kind} {}\n{excerpt}", diagnostic.message)
}

#[derive(Serialize)]
struct JsonDiagnostic<'a> {
    id: &'static str,
    code: String,
    severity: String,
    message: &'a str,
    #[serde(flatten)]
    place: JsonPlace,
    notes: Vec<JsonNote<'a>>,
}

#[derive(Serialize)]
struct JsonNote<'a> {
    id: &'static str,
    code: String,
    message: &'a str,
    #[serde(flatten)]
    place: Option<JsonPlace>,
}

#[derive(Serialize)]
struct JsonPlace {
    file: String,
    line: u32,
    column: u32,
    end_line: u32,
    end_column: u32,
}

/// The diagnostic as one line of JSON with its `id` (the name in the code
/// registry) and its `code`.
pub fn render_json(diagnostic: &Diagnostic, sources: &SourceMap) -> String {
    let place = |span: Span| {
        let file = sources.file(span.file);
        let (start, end) = (file.line_col(span.lo), file.line_col(span.hi));
        JsonPlace {
            file: file.path().display().to_string(),
            line: start.line,
            column: start.column,
            end_line: end.line,
            end_column: end.column,
        }
    };
    let code = |code: Code| (code.name(), code.to_string());
    let (id, number) = code(diagnostic.code);
    let json = JsonDiagnostic {
        id,
        code: number,
        severity: diagnostic.code.severity().to_string(),
        message: &diagnostic.message,
        place: place(diagnostic.span),
        notes: diagnostic
            .notes
            .iter()
            .map(|note| {
                let (id, number) = code(note.code);
                JsonNote {
                    id,
                    code: number,
                    message: &note.message,
                    place: note.span.map(place),
                }
            })
            .collect(),
    };
    // Serializing strings and integers to a String cannot fail.
    #[allow(clippy::expect_used)]
    serde_json::to_string(&json).expect("diagnostic JSON failed")
}

#[cfg(test)]
mod tests {

    use super::*;

    fn span_of(sources: &SourceMap, file: gopp_span::FileId, needle: &str) -> Span {
        let lo = sources.file(file).text().find(needle).unwrap() as u32;
        Span {
            file,
            lo,
            hi: lo + needle.len() as u32,
        }
    }

    fn sample() -> (SourceMap, Diagnostic) {
        let sources = SourceMap::new();
        let text = "package main\n\n//gopp:cflags -O2\nvar π = 1 $ 2\n";
        let file = SourceFile::new("main.gopp".into(), text.to_owned()).unwrap();
        let file = sources.add(file);
        let diagnostic = Diagnostic::new(
            Code::GoppDirectiveInSource,
            span_of(&sources, file, "//gopp:cflags -O2"),
            "//gopp:cflags is not a source directive",
        )
        .with_note(Code::NoteGoppModLine, None, "add `cflags -O2` to gopp.mod")
        .with_note(
            Code::NoteGoppModLine,
            Some(span_of(&sources, file, "$")),
            "a note with a place",
        );
        (sources, diagnostic)
    }

    #[test]
    fn text_has_the_code_the_excerpt_and_the_notes() {
        let (sources, diagnostic) = sample();
        insta::assert_snapshot!(render_text(&diagnostic, &sources, false));
    }

    #[test]
    fn text_is_colored_only_on_request() {
        let (sources, diagnostic) = sample();
        assert!(!render_text(&diagnostic, &sources, false).contains('\x1b'));
        assert!(render_text(&diagnostic, &sources, true).contains('\x1b'));
    }

    #[test]
    fn json_has_id_code_and_byte_columns() {
        let (sources, diagnostic) = sample();
        insta::assert_snapshot!(render_json(&diagnostic, &sources));
    }
}
