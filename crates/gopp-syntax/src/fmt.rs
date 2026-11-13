//! The source formatter: one layout for every `.gopp` file.
//!
//! The layout rules are those of Go's `go/printer` and `text/tabwriter`, so
//! Go code comes out as `gofmt` writes it. The forms that only Go++ has are
//! laid out like the nearest Go form.

mod context;
mod decls;
mod doc;
mod exprs;
mod imports;
mod printer;
mod stmts;
mod tabwriter;
#[cfg(test)]
mod tests;

use gopp_diag::Sink;
use gopp_span::SourceFile;
use lasso::ThreadedRodeo;

use crate::ast::File;
use crate::parser;
pub(crate) use exprs::normalized_number;
pub(crate) use imports::canonical_path as canonical_import_path;

/// How often the text is formatted until it no longer changes.
const MAX_PASSES: usize = 8;

/// The text of `file` in canonical layout. `file` is the tree of `source`,
/// parsed without a syntax error.
///
/// Formatting the result again changes nothing. One pass of the rules does
/// not promise that: where a line break of the source decides a layout,
/// `gofmt` itself can give a text that it would lay out differently. Such a
/// text is formatted again, so the result is what `gofmt` settles on.
pub fn format(file: &File, source: &SourceFile, names: &ThreadedRodeo) -> String {
    format_counted(file, source, names).0
}

/// The formatted text and the number of steps the formatter took.
fn format_counted(file: &File, source: &SourceFile, names: &ThreadedRodeo) -> (String, u64) {
    let (mut text, mut steps) = format_once(file, source, names);
    if text == source.text() {
        return (text, steps);
    }
    for _ in 1..MAX_PASSES {
        let again = SourceFile::new(source.path().to_owned(), text.clone());
        let Ok(again) = again else { break };
        let sink = Sink::new(1);
        let tree = parser::parse(file.id, &again, names, &sink);
        if !sink.finish().is_empty() {
            break;
        }
        let (next, more) = format_once(&tree, &again, names);
        steps += more;
        if next == text {
            break;
        }
        text = next;
    }
    (text, steps)
}

/// One pass of the layout rules over `file`.
fn format_once(file: &File, source: &SourceFile, names: &ThreadedRodeo) -> (String, u64) {
    let cx = context::Cx::new(file, source, names);
    let mut printer = printer::Printer::new(&cx, None);
    printer.file();
    printer.finish();

    let mut writer = tabwriter::Writer::new();
    writer.write(&printer.output);
    let (laid_out, cells) = writer.finish();
    let (text, _) = tabwriter::trim(&laid_out);
    let text = String::from_utf8_lossy(&text).into_owned();
    (text, printer.steps + cells)
}
