//! The stable codes of the diagnostics.

use std::fmt;

use crate::Severity;

macro_rules! codes {
    ($($number:literal $variant:ident $name:literal,)*) => {
        /// A diagnostic's identity. Number and name are the row in
        /// `docs/errors/codes.lock`, which never changes once written.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
        #[repr(u16)]
        pub enum Code {
            $($variant = $number,)*
        }

        impl Code {
            pub const ALL: &[Code] = &[$(Code::$variant,)*];

            pub fn name(self) -> &'static str {
                match self {
                    $(Code::$variant => $name,)*
                }
            }
        }
    };
}

codes! {
    1 TooManyErrors "err_too_many_errors",
    2 LexIllegalNul "err_lex_illegal_nul",
    3 LexUtf16 "err_lex_utf16",
    4 LexIllegalUtf8 "err_lex_illegal_utf8",
    5 LexIllegalBom "err_lex_illegal_bom",
    6 LexIllegalChar "err_lex_illegal_char",
    7 LexCurlyQuote "err_lex_curly_quote",
    8 LexCommentUnterminated "err_lex_comment_unterminated",
    9 LexRadixPoint "err_lex_radix_point",
    10 LexNoDigits "err_lex_no_digits",
    11 LexExponentNeedsDecimal "err_lex_exponent_needs_decimal",
    12 LexExponentNeedsHex "err_lex_exponent_needs_hex",
    13 LexExponentNoDigits "err_lex_exponent_no_digits",
    14 LexHexMantissaNeedsP "err_lex_hex_mantissa_needs_p",
    15 LexInvalidDigit "err_lex_invalid_digit",
    16 LexBadSeparator "err_lex_bad_separator",
    18 LexEscapeUnknown "err_lex_escape_unknown",
    19 LexEscapeIllegalChar "err_lex_escape_illegal_char",
    20 LexEscapeBadCodePoint "err_lex_escape_bad_code_point",
    21 LexRuneUnterminated "err_lex_rune_unterminated",
    22 LexRuneIllegal "err_lex_rune_illegal",
    23 LexStringUnterminated "err_lex_string_unterminated",
    24 LexRawStringUnterminated "err_lex_raw_string_unterminated",
    72 GoppDirectiveInSource "err_gopp_directive_in_source",
    73 NoteGoppModLine "note_gopp_mod_line",
}

impl Code {
    pub fn number(self) -> u16 {
        self as u16
    }

    /// The severity the name's prefix states: `err_`, `warn_` or `note_`.
    pub fn severity(self) -> Severity {
        match self.name().split('_').next() {
            Some("warn") => Severity::Warning,
            Some("note") => Severity::Note,
            _ => Severity::Error,
        }
    }
}

impl fmt::Display for Code {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "E{:04}", self.number())
    }
}

#[cfg(test)]
mod tests {
    use std::env;
    use std::fmt::Write;
    use std::fs;

    use super::*;
    use crate::LockRegistry;

    const LOCK_PATH: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../docs/errors/codes.lock");

    #[test]
    fn a_code_prints_as_e_and_four_digits() {
        assert_eq!(Code::LexIllegalNul.to_string(), "E0002");
        assert_eq!(Code::NoteGoppModLine.to_string(), "E0073");
    }

    #[test]
    fn severity_follows_the_name() {
        assert_eq!(Code::TooManyErrors.severity(), Severity::Error);
        assert_eq!(Code::NoteGoppModLine.severity(), Severity::Note);
    }

    /// With `GOPP_UPDATE_CODES=1`, codes numbered right after the last row
    /// are appended to the lock file first.
    #[test]
    fn every_code_is_its_row_of_the_lock_file() {
        let mut text = fs::read_to_string(LOCK_PATH).unwrap();
        let mut lock = LockRegistry::parse(&text).unwrap();
        if env::var_os("GOPP_UPDATE_CODES").is_some() {
            let mut new: Vec<Code> = Code::ALL.to_vec();
            new.retain(|code| usize::from(code.number()) > lock.len());
            new.sort();
            for code in new {
                writeln!(text, "{code} {}", code.name()).unwrap();
            }
            lock = LockRegistry::parse(&text).unwrap();
            fs::write(LOCK_PATH, &text).unwrap();
        }
        for &code in Code::ALL {
            assert_eq!(
                lock.name(code.number()),
                Some(code.name()),
                "{code} {} is not a row of docs/errors/codes.lock",
                code.name()
            );
        }
    }

    #[test]
    fn codes_are_listed_in_order_without_repeats() {
        assert!(Code::ALL.windows(2).all(|pair| pair[0] < pair[1]));
    }
}
