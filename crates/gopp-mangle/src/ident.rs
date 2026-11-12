//! Identifiers, package paths and numbers as a name writes them
//! (appendix A.3).

use std::fmt::Write;

/// The identifier without its length: every byte that is no letter, no
/// `_` and no digit after the first place is written as its code point
/// between `$`.
pub(crate) fn escape(bytes: &[u8], out: &mut String) {
    let start = out.len();
    for chunk in bytes.utf8_chunks() {
        for c in chunk.valid().chars() {
            let plain =
                c == '_' || c.is_ascii_alphabetic() || (c.is_ascii_digit() && out.len() > start);
            match plain {
                true => out.push(c),
                false => {
                    let _ = write!(out, "${:x}$", c as u32);
                }
            }
        }
        for byte in chunk.invalid() {
            let _ = write!(out, "$x{byte:02x}$");
        }
    }
}

pub(crate) fn identifier(bytes: &[u8], out: &mut String) {
    if bytes.is_empty() {
        out.push_str("0_");
        return;
    }
    let mut text = String::with_capacity(bytes.len());
    escape(bytes, &mut text);
    let _ = write!(out, "{}{text}", text.len());
}

pub(crate) fn path(path: &[u8], out: &mut String) {
    for segment in path.split(|&byte| byte == b'/') {
        identifier(segment, out);
    }
    out.push('E');
}

pub(crate) fn number(value: u64, out: &mut String) {
    let _ = write!(out, "{value}_");
}

pub(crate) fn signed(value: i64, out: &mut String) {
    if value < 0 {
        out.push('n');
    }
    number(value.unsigned_abs(), out);
}

/// The bytes that the escaped text stands for; `None` if it is not
/// escaped text.
pub(crate) fn unescape(text: &[u8]) -> Option<Vec<u8>> {
    let mut bytes = Vec::with_capacity(text.len());
    let mut rest = text;
    while let Some((&first, after)) = rest.split_first() {
        rest = after;
        if first != b'$' {
            bytes.push(first);
            continue;
        }
        let end = rest.iter().position(|&byte| byte == b'$')?;
        let (code, after) = (&rest[..end], &rest[end + 1..]);
        rest = after;
        let hex = |digits: &[u8]| {
            let text = std::str::from_utf8(digits).ok()?;
            let all = digits.iter().all(u8::is_ascii_hexdigit);
            (all && (1..=8).contains(&digits.len())).then(|| u32::from_str_radix(text, 16).ok())?
        };
        match code.split_first() {
            Some((b'x', digits)) => bytes.push(u8::try_from(hex(digits)?).ok()?),
            _ => {
                let c = char::from_u32(hex(code)?)?;
                bytes.extend_from_slice(c.encode_utf8(&mut [0; 4]).as_bytes());
            }
        }
    }
    Some(bytes)
}
