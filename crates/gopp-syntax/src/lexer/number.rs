//! Number literals.

use gopp_diag::Code;

use super::Scan;
use crate::token::TokenKind;

const HAS_DIGIT: u8 = 1;
const HAS_SEPARATOR: u8 = 2;

/// Scans the number that starts at `start` (a digit, or a `.` before a
/// digit). Returns its kind and end, and whether it is well formed.
pub(super) fn scan(scan: &mut Scan, text: &str, start: usize) -> (TokenKind, usize, bool) {
    let bytes = text.as_bytes();
    let at = |pos: usize| bytes.get(pos).copied().unwrap_or(0);
    let mut pos = start;
    let mut ok = true;
    let mut kind = TokenKind::Int;
    let mut base = 10;
    // 0 for decimal, b'0' for a leading zero, else the letter of the prefix.
    let mut prefix = 0;
    let mut seen = 0;
    let mut invalid_digit = None;

    let mut seen_point = at(pos) == b'.';
    if seen_point {
        pos += 1;
    } else {
        if at(pos) == b'0' {
            pos += 1;
            prefix = at(pos).to_ascii_lowercase();
            base = match prefix {
                b'x' => 16,
                b'o' => 8,
                b'b' => 2,
                _ => {
                    prefix = b'0';
                    seen = HAS_DIGIT;
                    8
                }
            };
            if prefix != b'0' {
                pos += 1;
            }
        }
        seen |= digits(bytes, &mut pos, base, &mut invalid_digit);
        if at(pos) == b'.' {
            if prefix == b'o' || prefix == b'b' {
                let message = format!("invalid radix point in {} literal", base_name(base));
                scan.error(Code::LexRadixPoint, pos, pos + 1, message);
                ok = false;
            }
            pos += 1;
            seen_point = true;
        }
    }

    if seen_point {
        kind = TokenKind::Float;
        seen |= digits(bytes, &mut pos, base, &mut invalid_digit);
    }
    if seen & HAS_DIGIT == 0 && ok {
        let message = format!("{} literal has no digits", base_name(base));
        scan.error(Code::LexNoDigits, start, pos, message);
        ok = false;
    }

    let exponent = at(pos).to_ascii_lowercase();
    if exponent == b'e' || exponent == b'p' {
        let letter = char::from(at(pos));
        if ok && exponent == b'e' && prefix != 0 && prefix != b'0' {
            let message = format!("{letter:?} exponent requires decimal mantissa");
            scan.error(Code::LexExponentNeedsDecimal, pos, pos + 1, message);
            ok = false;
        } else if ok && exponent == b'p' && prefix != b'x' {
            let message = format!("{letter:?} exponent requires hexadecimal mantissa");
            scan.error(Code::LexExponentNeedsHex, pos, pos + 1, message);
            ok = false;
        }
        pos += 1;
        kind = TokenKind::Float;
        if at(pos) == b'+' || at(pos) == b'-' {
            pos += 1;
        }
        let exponent_digits = digits(bytes, &mut pos, 10, &mut None);
        seen = exponent_digits | seen & HAS_SEPARATOR;
        if exponent_digits & HAS_DIGIT == 0 && ok {
            scan.error(
                Code::LexExponentNoDigits,
                pos,
                pos,
                "exponent has no digits",
            );
            ok = false;
        }
    } else if prefix == b'x' && kind == TokenKind::Float && ok {
        let message = "hexadecimal mantissa requires a 'p' exponent";
        scan.error(Code::LexHexMantissaNeedsP, start, pos, message);
        ok = false;
    }

    if at(pos) == b'i' {
        kind = TokenKind::Imag;
        pos += 1;
    }

    if let (TokenKind::Int, Some(digit), true) = (kind, invalid_digit, ok) {
        let message = format!(
            "invalid digit {:?} in {} literal",
            char::from(bytes[digit]),
            base_name(base)
        );
        scan.error(Code::LexInvalidDigit, digit, digit + 1, message);
        ok = false;
    }
    if seen & HAS_SEPARATOR != 0
        && ok
        && let Some(separator) = invalid_separator(&bytes[start..pos])
    {
        let at = start + separator;
        let message = "'_' must separate successive digits";
        scan.error(Code::LexBadSeparator, at, at + 1, message);
        ok = false;
    }
    (kind, pos, ok)
}

fn base_name(base: u8) -> &'static str {
    match base {
        2 => "binary",
        8 => "octal",
        10 => "decimal",
        _ => "hexadecimal",
    }
}

/// Skips digits and `_`. Says which of the two it saw and records the first
/// digit that is too large for `base`.
fn digits(bytes: &[u8], pos: &mut usize, base: u8, invalid: &mut Option<usize>) -> u8 {
    let mut seen = 0;
    while let Some(&byte) = bytes.get(*pos) {
        if byte == b'_' {
            seen |= HAS_SEPARATOR;
        } else if base == 16 && byte.is_ascii_hexdigit() {
            seen |= HAS_DIGIT;
        } else if base != 16 && byte.is_ascii_digit() {
            if byte - b'0' >= base && invalid.is_none() {
                *invalid = Some(*pos);
            }
            seen |= HAS_DIGIT;
        } else {
            break;
        }
        *pos += 1;
    }
    seen
}

/// The index of the first `_` of `literal` that does not sit between two
/// digits. A base prefix counts as a digit.
fn invalid_separator(literal: &[u8]) -> Option<usize> {
    #[derive(Clone, Copy, PartialEq)]
    enum Last {
        Digit,
        Separator,
        Other,
    }
    let mut last = Last::Other;
    let mut hex = false;
    let mut start = 0;
    if literal.len() >= 2 && literal[0] == b'0' {
        let prefix = literal[1].to_ascii_lowercase();
        if matches!(prefix, b'x' | b'o' | b'b') {
            hex = prefix == b'x';
            last = Last::Digit;
            start = 2;
        }
    }
    for (index, &byte) in literal.iter().enumerate().skip(start) {
        let previous = last;
        if byte == b'_' {
            if previous != Last::Digit {
                return Some(index);
            }
            last = Last::Separator;
        } else if byte.is_ascii_digit() || hex && byte.is_ascii_hexdigit() {
            last = Last::Digit;
        } else {
            if previous == Last::Separator {
                return Some(index - 1);
            }
            last = Last::Other;
        }
    }
    (last == Last::Separator).then(|| literal.len() - 1)
}
