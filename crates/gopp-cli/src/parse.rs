//! `gopp parse`.

use std::fs;
use std::io::{self, IsTerminal};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use gopp_diag::{Severity, Sink, render_text};
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_syntax::ast::{dump, dump_json};
use gopp_syntax::parser::parse;
use lasso::ThreadedRodeo;

use crate::{EXIT_ERRORS, Emit};

/// As Go's compiler: the errors after the tenth are rarely worth reading.
const MAX_DIAGNOSTICS: usize = 10;

pub fn run(files: &[PathBuf], emit: Option<Emit>) -> ExitCode {
    let sources = SourceMap::new();
    let names = ThreadedRodeo::new();
    let color = io::stderr().is_terminal();
    let mut failed = false;
    for path in files {
        let source = match load(path) {
            Ok(source) => source,
            Err(message) => {
                eprintln!("error: {message}");
                failed = true;
                continue;
            }
        };
        let id = sources.add(source);
        let source = sources.file(id);
        let sink = Sink::new(MAX_DIAGNOSTICS);
        let file = parse(id, &source, &names, &sink);
        for diagnostic in sink.finish() {
            failed |= diagnostic.code.severity() == Severity::Error;
            eprint!("{}", render_text(&diagnostic, &sources, color));
        }
        match emit {
            Some(Emit::Ast) => print!("{}", dump(&file, source.text(), &names)),
            Some(Emit::Json) => print!("{}", dump_json(&file, source.text(), &names)),
            None => {}
        }
    }
    if failed {
        ExitCode::from(EXIT_ERRORS)
    } else {
        ExitCode::SUCCESS
    }
}

fn load(path: &Path) -> Result<SourceFile, String> {
    let Some(dialect) = Dialect::from_path(path) else {
        return Err(format!("{}: not a .gopp or .go file", path.display()));
    };
    let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
    SourceFile::from_bytes(path.to_owned(), &bytes, dialect).map_err(|error| error.to_string())
}
