//! The value of a string literal.

/// The bytes a string literal stands for; `None` if it is not well formed.
pub fn unquote(literal: &str) -> Option<Vec<u8>> {
    if let Some(raw) = literal.strip_prefix('`') {
        // A raw string has no carriage returns.
        let raw = raw.strip_suffix('`')?;
        return Some(raw.bytes().filter(|&byte| byte != b'\r').collect());
    }
    let inner = literal.strip_prefix('"')?.strip_suffix('"')?;
    let mut out = Vec::with_capacity(inner.len());
    let mut rest = inner;
    while let Some(c) = rest.chars().next() {
        rest = &rest[c.len_utf8()..];
        if c != '\\' {
            let mut utf8 = [0; 4];
            out.extend_from_slice(c.encode_utf8(&mut utf8).as_bytes());
            continue;
        }
        let escape = rest.chars().next()?;
        rest = &rest[escape.len_utf8()..];
        let simple = match escape {
            'a' => 0x07,
            'b' => 0x08,
            'f' => 0x0C,
            'n' => b'\n',
            'r' => b'\r',
            't' => b'\t',
            'v' => 0x0B,
            '\\' => b'\\',
            '"' => b'"',
            _ => {
                let (digits, radix, is_char) = match escape {
                    '0'..='7' => (3, 8, false),
                    'x' => (2, 16, false),
                    'u' => (4, 16, true),
                    'U' => (8, 16, true),
                    _ => return None,
                };
                // An octal escape starts with its first digit.
                let from = if radix == 8 {
                    &inner[inner.len() - rest.len() - 1..]
                } else {
                    rest
                };
                let text = from.get(..digits)?;
                let value = u32::from_str_radix(text, radix).ok()?;
                rest = &from[digits..];
                if is_char {
                    let mut utf8 = [0; 4];
                    let c = char::from_u32(value)?;
                    out.extend_from_slice(c.encode_utf8(&mut utf8).as_bytes());
                } else {
                    out.push(u8::try_from(value).ok()?);
                }
                continue;
            }
        };
        out.push(simple);
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::unquote;

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
}
