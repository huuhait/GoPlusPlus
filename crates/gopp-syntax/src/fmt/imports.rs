//! Sorts the imports of each import group: within a run
//! of import lines that no blank line divides, by path; a duplicate without
//! a comment is dropped. The comments of a line move with it.

use super::context::{Cx, Group, Item, Spec};
use crate::ast::{DeclKind, ExprKind, LitKind};
use crate::token::TokenKind as T;

const HEADER_EXTENSIONS: [&str; 4] = [".h", ".hh", ".hpp", ".hxx"];

pub(super) fn sort(cx: &mut Cx) {
    let mut items = std::mem::take(&mut cx.items);
    for item in &mut items {
        let Item::Gen(decl) = item else { break };
        if decl.keyword != T::Import {
            break;
        }
        let Some((_, rparen)) = decl.parens else {
            continue;
        };
        let specs = std::mem::take(&mut decl.specs);
        let mut start = 0;
        for index in 1..=specs.len() {
            let ends_run = index == specs.len()
                || cx.line(specs[index].pos) > 1 + cx.line(specs[index - 1].end)
                || is_header(cx, specs[index])
                || is_header(cx, specs[index - 1]);
            if ends_run {
                sort_run(cx, &specs[start..index], rparen, &mut decl.specs);
                start = index;
            }
        }
        // Dropping a duplicate can leave a blank line before the `)`.
        if let Some(last) = decl.specs.last() {
            let last_line = cx.line(last.pos);
            let mut rparen_line = cx.line(rparen);
            while rparen_line > last_line + 1 {
                rparen_line -= 1;
                cx.merge_line(rparen_line);
            }
        }
    }
    cx.items = items;
}

/// FOR-5: the imports of C and C++ headers keep their order, so each is a
/// run of its own.
fn is_header(cx: &Cx, spec: Spec) -> bool {
    let path = path(cx, spec);
    path.starts_with('<')
        || HEADER_EXTENSIONS
            .iter()
            .any(|extension| path.ends_with(extension))
}

fn path_literal<'a>(cx: &Cx<'a>, spec: Spec) -> Option<&'a str> {
    let DeclKind::Import { path, .. } = cx.file.decl_nodes[spec.decl].kind else {
        return None;
    };
    let path = &cx.file.exprs[path];
    let is_string = matches!(
        path.kind,
        ExprKind::BasicLit {
            kind: LitKind::String,
            ..
        }
    );
    is_string.then(|| cx.source(path.span.lo, path.span.hi))
}

/// The import path, or nothing if it is not a valid string.
fn path(cx: &Cx, spec: Spec) -> String {
    path_literal(cx, spec).and_then(unquote).unwrap_or_default()
}

fn name<'a>(cx: &Cx<'a>, spec: Spec) -> &'a str {
    match cx.file.decl_nodes[spec.decl].kind {
        DeclKind::Import {
            name: Some(name), ..
        } => cx.name(name),
        _ => "",
    }
}

fn comment_text(cx: &Cx, comment: Option<Group>) -> String {
    let Some(group) = comment else {
        return String::new();
    };
    let comments = &cx.comments[group.first..group.end];
    let texts = comments.iter().map(|&comment| cx.comment_text(comment));
    texts.collect::<Vec<_>>().join("\n")
}

fn sort_run(cx: &mut Cx, run: &[Spec], rparen: u32, sorted: &mut Vec<Spec>) {
    if run.len() <= 1 {
        sorted.extend_from_slice(run);
        return;
    }
    let count = run.len();
    let slots: Vec<(u32, u32)> = run.iter().map(|spec| (spec.pos, spec.end)).collect();

    // The comments on the lines of the run.
    let first_line = cx.line(slots[0].0);
    let last_line = cx.line(slots[count - 1].1);
    let begin = cx.lines[first_line as usize - 1];
    let end = match cx.lines.get(last_line as usize) {
        Some(&next_line) => next_line,
        None => slots[count - 1].1,
    };
    // The groups are in the order of their places, so those of the run
    // follow each other.
    let first = cx
        .groups
        .partition_point(|&group| cx.group_pos(group) < begin);
    let in_run = |group: &&Group| cx.group_end(**group) < end;
    let count_in_run = cx.groups[first..].iter().take_while(in_run).count();
    let range = (count_in_run > 0).then(|| (first, first + count_in_run - 1));

    // Each comment belongs to the import on its line; `true` marks one to
    // the left of its import.
    let mut owned: Vec<Vec<(bool, Group)>> = vec![Vec::new(); count];
    if let Some((first, last)) = range {
        let mut at = 0;
        for &group in &cx.groups[first..=last] {
            let pos = cx.group_pos(group);
            while at + 1 < count && slots[at + 1].0 <= pos {
                at += 1;
            }
            let mut left = false;
            if at == 0 && slots[0].0 > pos {
                left = true;
            } else if at + 1 < count && cx.line(slots[at].0) + 1 == cx.line(pos) {
                at += 1;
                left = true;
            }
            owned[at].push((left, group));
        }
    }

    let line_comments: Vec<Option<Group>> =
        run.iter().map(|spec| cx.line_comment(spec.end)).collect();
    let keys: Vec<(String, &str, String)> = (0..count)
        .map(|index| {
            let comment = comment_text(cx, line_comments[index]);
            (path(cx, run[index]), name(cx, run[index]), comment)
        })
        .collect();
    let mut order: Vec<usize> = (0..count).collect();
    order.sort_by(|&a, &b| keys[a].cmp(&keys[b]));

    let mut kept = Vec::with_capacity(count);
    for (index, &spec) in order.iter().enumerate() {
        let duplicate = order.get(index + 1).is_some_and(|&next| {
            keys[spec].0 == keys[next].0
                && keys[spec].1 == keys[next].1
                && line_comments[spec].is_none()
        });
        if !duplicate {
            kept.push(spec);
            continue;
        }
        // The line of a dropped import must not stay behind as a blank one.
        let line = cx.line(run[spec].pos);
        if line != cx.line(rparen) {
            cx.merge_line(line);
        }
    }

    for (slot, &spec) in kept.iter().enumerate() {
        let (pos, end) = slots[slot];
        sorted.push(Spec {
            decl: run[spec].decl,
            pos,
            end,
            moved: true,
        });
        for &(left, group) in &owned[spec] {
            for comment in &mut cx.comments[group.first..group.end] {
                comment.pos = if left { pos - 1 } else { end };
            }
        }
    }
    if let Some((first, last)) = range {
        let mut groups = cx.groups[first..=last].to_vec();
        groups.sort_by_key(|&group| cx.group_pos(group));
        cx.groups[first..=last].copy_from_slice(&groups);
    }
}

/// The value of a string literal.
pub(super) fn unquote(literal: &str) -> Option<String> {
    if let Some(raw) = literal.strip_prefix('`') {
        return Some(raw.strip_suffix('`')?.replace('\r', ""));
    }
    let body = literal.strip_prefix('"')?.strip_suffix('"')?;
    let mut value = String::with_capacity(body.len());
    let mut chars = body.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            value.push(c);
            continue;
        }
        let escape = chars.next()?;
        let mut number = |digits: usize, radix: u32| {
            let text: String = chars.by_ref().take(digits).collect();
            (text.len() == digits)
                .then(|| u32::from_str_radix(&text, radix).ok())
                .flatten()
        };
        match escape {
            'a' => value.push('\x07'),
            'b' => value.push('\x08'),
            'f' => value.push('\x0C'),
            'n' => value.push('\n'),
            'r' => value.push('\r'),
            't' => value.push('\t'),
            'v' => value.push('\x0B'),
            '\\' => value.push('\\'),
            '"' => value.push('"'),
            'x' => value.push(char::from_u32(number(2, 16)?)?),
            'u' => value.push(char::from_u32(number(4, 16)?)?),
            'U' => value.push(char::from_u32(number(8, 16)?)?),
            digit @ '0'..='7' => {
                let rest = number(2, 8)?;
                value.push(char::from_u32(digit.to_digit(8)? * 64 + rest)?);
            }
            _ => return None,
        }
    }
    Some(value)
}

/// The import path in canonical form: a path in a raw string becomes an
/// ordinary string when nothing in it needs the raw form.
pub(crate) fn canonical_path(literal: &str) -> Option<String> {
    let value = unquote(literal)?;
    const ILLEGAL: &str = "!\"#$%&'()*,:;<=>?[\\]^{|}`\u{FFFD}";
    let plain = !value.is_empty()
        && value
            .chars()
            .all(|c| c.is_ascii_graphic() && !ILLEGAL.contains(c));
    let quoted = format!("\"{value}\"");
    (plain && quoted != literal).then_some(quoted)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn string_literals_are_unquoted() {
        assert_eq!(unquote("\"a/b\"").as_deref(), Some("a/b"));
        assert_eq!(unquote("`a\\b`").as_deref(), Some("a\\b"));
        assert_eq!(
            unquote("\"\\x41\\u00e9\\101\\n\"").as_deref(),
            Some("A\u{e9}A\n")
        );
        assert_eq!(unquote("\"\\q\""), None);
        assert_eq!(unquote("'a'"), None);
    }

    #[test]
    fn a_raw_import_path_becomes_an_ordinary_string() {
        assert_eq!(canonical_path("`fmt`").as_deref(), Some("\"fmt\""));
        assert_eq!(canonical_path("\"fmt\""), None);
        assert_eq!(canonical_path("`<stdio.h>`"), None);
    }
}
