//! The values of string and rune literals.

/// What one character or escape sequence of a literal stands for.
enum Unit {
    Char(char),
    /// `\x41` or `\101`: one byte, which need not be a character.
    Byte(u8),
}

/// The first character or escape sequence of `text`, and what follows it.
fn unit(text: &str) -> Option<(Unit, &str)> {
    let c = text.chars().next()?;
    let rest = &text[c.len_utf8()..];
    if c != '\\' {
        return Some((Unit::Char(c), rest));
    }
    let escape = rest.chars().next()?;
    let simple = match escape {
        'a' => '\x07',
        'b' => '\x08',
        'f' => '\x0C',
        'n' => '\n',
        'r' => '\r',
        't' => '\t',
        'v' => '\x0B',
        '\\' | '"' | '\'' => escape,
        _ => {
            // An octal escape starts with its first digit.
            let (digits, from, radix, is_char) = match escape {
                '0'..='7' => (3, rest, 8, false),
                'x' => (2, &rest[1..], 16, false),
                'u' => (4, &rest[1..], 16, true),
                'U' => (8, &rest[1..], 16, true),
                _ => return None,
            };
            let value = u32::from_str_radix(from.get(..digits)?, radix).ok()?;
            let unit = if is_char {
                Unit::Char(char::from_u32(value)?)
            } else {
                Unit::Byte(u8::try_from(value).ok()?)
            };
            return Some((unit, &from[digits..]));
        }
    };
    Some((Unit::Char(simple), &rest[1..]))
}

/// The bytes a string literal stands for; `None` if it is not well formed.
pub fn unquote(literal: &str) -> Option<Vec<u8>> {
    if let Some(raw) = literal.strip_prefix('`') {
        // A raw string has no carriage returns.
        let raw = raw.strip_suffix('`')?;
        return Some(raw.bytes().filter(|&byte| byte != b'\r').collect());
    }
    let mut rest = literal.strip_prefix('"')?.strip_suffix('"')?;
    let mut out = Vec::with_capacity(rest.len());
    while !rest.is_empty() {
        let (unit, after) = unit(rest)?;
        match unit {
            Unit::Char(c) => out.extend_from_slice(c.encode_utf8(&mut [0; 4]).as_bytes()),
            Unit::Byte(byte) => out.push(byte),
        }
        rest = after;
    }
    Some(out)
}

/// The value of a rune literal: a code point, or the byte of `'\xff'`.
pub fn unquote_rune(literal: &str) -> Option<u32> {
    let inner = literal.strip_prefix('\'')?.strip_suffix('\'')?;
    match unit(inner)? {
        (Unit::Char(c), "") => Some(u32::from(c)),
        (Unit::Byte(byte), "") => Some(u32::from(byte)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::{unquote, unquote_rune};

    #[test]
    fn a_literal_stands_for_its_bytes() {
        let cases: [(&str, &[u8]); 8] = [
            (r#""fmt""#, b"fmt"),
            (r#""""#, b""),
            ("`a\\n\r\nb`", b"a\\n\nb"),
            (r#""a\tb\n\\\"""#, b"a\tb\n\\\""),
            (r#""\x41\101é\U0001F600""#, "AA\u{e9}\u{1F600}".as_bytes()),
            (r#""\xff\377""#, &[0xff, 0xff]),
            (r#""é""#, "é".as_bytes()),
            (r#""\a\b\f\r\v""#, &[7, 8, 12, 13, 11]),
        ];
        for (literal, bytes) in cases {
            assert_eq!(unquote(literal).as_deref(), Some(bytes), "{literal}");
        }
        for bad in [
            r#""\q""#,
            r#""\x4""#,
            r#""\400""#,
            r#""\ud800""#,
            "\"a",
            "a\"",
            "'a'",
        ] {
            assert_eq!(unquote(bad), None, "{bad}");
        }
    }

    #[test]
    fn a_rune_literal_stands_for_one_code_point_or_byte() {
        let cases = [
            ("'a'", 97),
            ("'é'", 0xe9),
            (r"'\n'", 10),
            (r"'\''", 39),
            (r"'\\'", 92),
            (r"'\x00'", 0),
            (r"'\xff'", 255),
            (r"'\377'", 255),
            (r"'é'", 0xe9),
            (r"'\U0001F600'", 0x1F600),
        ];
        for (literal, value) in cases {
            assert_eq!(unquote_rune(literal), Some(value), "{literal}");
        }
        for bad in ["''", "'ab'", r"'\q'", r"'\400'", "\"a\""] {
            assert_eq!(unquote_rune(bad), None, "{bad}");
        }
    }
}
