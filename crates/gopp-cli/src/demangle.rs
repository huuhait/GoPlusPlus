//! `gopp demangle`: Go++ symbols as Go++ text.

use std::io::{self, BufRead, Write};
use std::process::ExitCode;

/// Prints each of `names` as Go++ text, cut after `at_most` bytes. With
/// no name, copies standard input and replaces every word that is a
/// symbol, as `c++filt` does.
pub fn run(names: &[String], at_most: usize) -> ExitCode {
    if names.is_empty() {
        return crate::exit_code(filter(at_most).is_err());
    }
    let mut failed = false;
    for name in names {
        match gopp_mangle::demangle(name.as_bytes(), at_most) {
            Ok(readable) => println!("{readable}"),
            Err(error) => {
                eprintln!("error: {name}: {error}");
                failed = true;
            }
        }
    }
    crate::exit_code(failed)
}

fn filter(at_most: usize) -> io::Result<()> {
    let (mut input, mut output) = (io::stdin().lock(), io::stdout().lock());
    let mut line = Vec::new();
    while input.read_until(b'\n', &mut line)? > 0 {
        // A line that is no text holds no symbol to read.
        match std::str::from_utf8(&line) {
            Ok(text) => output.write_all(gopp_mangle::filter(text, at_most).as_bytes())?,
            Err(_) => output.write_all(&line)?,
        }
        line.clear();
    }
    output.flush()
}
