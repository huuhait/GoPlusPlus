//! Go-style command-line flags.

use std::ffi::OsString;

/// Rewrites `-flag` and `-flag=value` to `--flag`, so long flags work with one
/// dash as in Go. One-letter flags, negative numbers, and everything after
/// `--` are left alone.
pub fn normalize(args: impl Iterator<Item = OsString>) -> Vec<OsString> {
    let mut flags_ended = false;
    args.map(|arg| {
        flags_ended |= arg == "--";
        match arg.to_str() {
            Some(text) if !flags_ended && is_single_dash_long(text) => format!("-{text}").into(),
            _ => arg,
        }
    })
    .collect()
}

fn is_single_dash_long(arg: &str) -> bool {
    let Some(rest) = arg.strip_prefix('-') else {
        return false;
    };
    let name = rest.split('=').next().unwrap_or(rest);
    name.len() > 1 && name.starts_with(|c: char| c.is_ascii_alphabetic())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn normalized(args: &[&str]) -> Vec<OsString> {
        normalize(args.iter().map(OsString::from))
    }

    #[test]
    fn single_dash_long_flags_gain_a_dash() {
        assert_eq!(
            normalized(&["gopp", "build", "-target=linux/arm64", "-trace", "--emit=c"]),
            [
                "gopp",
                "build",
                "--target=linux/arm64",
                "--trace",
                "--emit=c"
            ]
        );
    }

    #[test]
    fn short_flags_numbers_and_operands_are_kept() {
        let args = ["gopp", "-j", "8", "-o=out", "-10", "-", "a-b.gopp"];
        assert_eq!(normalized(&args), args);
    }

    #[test]
    fn nothing_changes_after_the_terminator() {
        let args = ["gopp", "run", "--", "-verbose"];
        assert_eq!(normalized(&args), args);
    }
}
