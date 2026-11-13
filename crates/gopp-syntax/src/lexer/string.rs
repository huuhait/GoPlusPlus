//! Rune and string literals, with the rules and messages of Go's scanner.

use gopp_diag::Code;

use super::{Scan, describe_char};

/// Scans a rune literal whose opening `'` is at `start`. Returns its end and
/// whether it is well formed.
pub(super) fn rune(scan: &mut Scan, text: &str, start: usize) -> (usize, bool) {
    let mut pos = start + 1;
    let mut ok = true;
    let mut count = 0;
    loop {
        match next_char(text, pos) {
            Some('\'') => {
                if ok && count == 0 {
                    let message = "empty rune literal or unescaped ' in rune literal";
                    scan.error(Code::LexRuneIllegal, pos, pos + 1, message);
                    ok = false;
                } else if ok && count != 1 {
                    let message = "more than one character in rune literal";
                    scan.error(Code::LexRuneIllegal, start, pos + 1, message);
                    ok = false;
                }
                return (pos + 1, ok);
            }
            Some('\\') => {
                pos += 1;
                ok &= escape(scan, text, &mut pos, '\'');
            }
            Some('\n') => {
                if ok {
                    let message = "newline in rune literal";
                    scan.error(Code::LexRuneUnterminated, pos, pos, message);
                }
                return (pos, false);
            }
            None => {
                if ok {
                    let message = "rune literal not terminated";
                    scan.error(Code::LexRuneUnterminated, start, start + 1, message);
                }
                return (pos, false);
            }
            Some(other) => pos += other.len_utf8(),
        }
        count += 1;
    }
}

/// Scans an interpreted string whose opening `"` is at `start`.
pub(super) fn string(scan: &mut Scan, text: &str, start: usize) -> (usize, bool) {
    let mut pos = start + 1;
    let mut ok = true;
    loop {
        match next_char(text, pos) {
            Some('"') => return (pos + 1, ok),
            Some('\\') => {
                pos += 1;
                ok &= escape(scan, text, &mut pos, '"');
            }
            Some('\n') => {
                scan.error(Code::LexStringUnterminated, pos, pos, "newline in string");
                return (pos, false);
            }
            None => {
                let message = "string not terminated";
                scan.error(Code::LexStringUnterminated, start, start + 1, message);
                return (pos, false);
            }
            Some(other) => pos += other.len_utf8(),
        }
    }
}

/// Scans a raw string whose opening backquote is at `start`.
pub(super) fn raw_string(scan: &mut Scan, text: &str, start: usize) -> (usize, bool) {
    match text[start + 1..].find('`') {
        Some(length) => (start + 1 + length + 1, true),
        None => {
            let message = "string not terminated";
            scan.error(Code::LexRawStringUnterminated, start, start + 1, message);
            (text.len(), false)
        }
    }
}

fn next_char(text: &str, pos: usize) -> Option<char> {
    text[pos..].chars().next()
}

/// Scans an escape sequence after its backslash and moves `pos` past it.
/// Returns whether it is valid. At the end of the file it says nothing: the
/// literal is reported as not terminated instead.
fn escape(scan: &mut Scan, text: &str, pos: &mut usize, quote: char) -> bool {
    let (digits, base, max) = match next_char(text, *pos) {
        None => return true,
        Some(c) if c == quote || "abfnrtv\\".contains(c) => {
            *pos += 1;
            return true;
        }
        Some('0'..='7') => (3, 8, 255),
        Some('x') => {
            *pos += 1;
            (2, 16, 255)
        }
        Some('u') => {
            *pos += 1;
            (4, 16, u32::from(char::MAX))
        }
        Some('U') => {
            *pos += 1;
            (8, 16, u32::from(char::MAX))
        }
        Some(other) => {
            let end = *pos + other.len_utf8();
            scan.error(Code::LexEscapeUnknown, *pos, end, "unknown escape");
            return false;
        }
    };
    let base_name = if base == 8 { "octal" } else { "hexadecimal" };
    let start = *pos;
    let mut value: u32 = 0;
    for _ in 0..digits {
        let Some(c) = next_char(text, *pos) else {
            return true;
        };
        let Some(digit) = c.to_digit(base) else {
            let message = format!("invalid character {c:?} in {base_name} escape");
            let end = *pos + c.len_utf8();
            scan.error(Code::LexEscapeIllegalChar, *pos, end, message);
            return false;
        };
        value = value * base + digit;
        *pos += 1;
    }
    if value > max && base == 8 {
        let message = format!("octal escape value {value} > 255");
        scan.error(Code::LexEscapeBadCodePoint, start, *pos, message);
        return false;
    }
    if value > max || (0xD800..0xE000).contains(&value) {
        let message = match char::from_u32(value) {
            Some(c) => format!("escape is invalid Unicode code point {}", describe_char(c)),
            None => format!("escape is invalid Unicode code point U+{value:04X}"),
        };
        scan.error(Code::LexEscapeBadCodePoint, start, *pos, message);
        return false;
    }
    true
}
