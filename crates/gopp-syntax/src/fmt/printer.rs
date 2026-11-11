//! The core of the printer. Tokens go out one at a time; the white space
//! between two tokens is held back until the second one is known, so that
//! the comments before it can be placed inside that white space.

use std::borrow::Cow;
use std::collections::HashMap;

use super::context::{Cx, Group};
use super::doc;
use super::tabwriter::ESCAPE;
use crate::ast::{ExprId, StmtId};
use crate::token::TokenKind as T;

/// Larger than any offset or line of a source file.
pub(super) const INFINITY: i32 = 1 << 30;
const MAX_NEWLINES: i32 = 2;
/// Pending white space is written out when it reaches this many entries,
/// which moves the comments that follow; `gofmt` has the same limit.
const WHITE_SPACE_LIMIT: usize = 16;
const FORMFEED: u8 = 0x0C;

/// A place in the source or in the output. Line 0 means "unknown".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Pos {
    pub offset: i64,
    pub line: i32,
    pub column: i32,
}

impl Pos {
    pub const START: Self = Self {
        offset: 0,
        line: 1,
        column: 1,
    };
    pub const END: Self = Self {
        offset: INFINITY as i64,
        line: INFINITY,
        column: 0,
    };

    fn is_valid(self) -> bool {
        self.line > 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Ws {
    Ignore,
    Blank,
    /// Ends a cell of an aligned column.
    Vtab,
    Newline,
    /// A line end that also ends every aligned column.
    Formfeed,
    Indent,
    Unindent,
}

/// No blank after a block comment that is followed by a token on its line.
pub(super) const NO_EXTRA_BLANK: u8 = 1;
/// No line end after a block comment before a closing brace.
pub(super) const NO_EXTRA_LINEBREAK: u8 = 2;

/// A node whose one-line size is asked for.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) enum Node {
    Expr(ExprId),
    Stmt(StmtId),
}

/// What the operators of a binary expression need: whether it has
/// operators of precedence 4 and 5, and the lowest precedence at which
/// operator and operand must stay apart.
pub(super) type BinaryShape = (bool, bool, i32);

pub(super) struct Printer<'a> {
    pub cx: &'a Cx<'a>,
    /// The size limit when the printer only measures a node: such a run
    /// has no comments and stops at the first line end.
    pub measure: Option<i32>,
    pub aborted: bool,
    text_bytes: i64,

    pub output: Vec<u8>,
    pub indent: i32,
    /// Depth of composite literals around the current token.
    pub level: i32,
    mode: u8,
    /// A literal with a line end in it breaks the columns of its line.
    end_alignment: bool,
    /// A line end here would be read as a semicolon.
    implied_semi: bool,
    /// The last token ended an attribute line: what follows is kept right
    /// under it, without a blank line.
    pub after_attribute: bool,
    last_tok: Option<T>,
    /// The last token if it was `(` or `[`.
    prev_open: Option<T>,
    wsbuf: Vec<Ws>,
    pub go_build: Vec<usize>,
    pub plus_build: Vec<usize>,

    /// Where the next token is in the source; an estimate until a caller
    /// sets it.
    pub pos: Pos,
    pub out: Pos,
    /// `pos` after the last token or comment.
    last: Pos,
    line_slot: Option<usize>,
    line_slots: Vec<i32>,

    cindex: usize,
    comment: usize,
    comment_offset: i64,
    comment_newline: bool,

    pub sizes: HashMap<Node, i32>,
    pub shapes: HashMap<ExprId, BinaryShape>,
    /// Tokens, comments and measured bytes so far: the measure of work in
    /// the scaling tests.
    pub steps: u64,
}

impl<'a> Printer<'a> {
    pub fn new(cx: &'a Cx<'a>, measure: Option<i32>) -> Self {
        let mut printer = Self {
            cx,
            measure,
            aborted: false,
            text_bytes: 0,
            output: Vec::new(),
            indent: 0,
            level: 0,
            mode: 0,
            end_alignment: false,
            implied_semi: false,
            after_attribute: false,
            last_tok: None,
            prev_open: None,
            wsbuf: Vec::with_capacity(WHITE_SPACE_LIMIT),
            go_build: Vec::new(),
            plus_build: Vec::new(),
            pos: Pos::START,
            out: Pos::START,
            last: Pos {
                offset: 0,
                line: 0,
                column: 0,
            },
            line_slot: None,
            line_slots: Vec::new(),
            cindex: 0,
            comment: 0,
            comment_offset: INFINITY as i64,
            comment_newline: false,
            sizes: HashMap::new(),
            shapes: HashMap::new(),
            steps: 0,
        };
        printer.next_comment();
        printer
    }

    /// The printer's text with the pending white space and comments.
    pub fn finish(&mut self) {
        self.implied_semi = false;
        self.flush(Pos::END, Some(T::Eof));
    }

    // ---- positions

    pub fn set_pos(&mut self, offset: u32) {
        self.pos = self.cx.pos(offset);
    }

    /// Asks for the output line of the next token.
    pub fn record_line(&mut self, slot: usize) {
        self.line_slot = Some(slot);
    }

    pub fn new_line_slot(&mut self) -> usize {
        self.line_slots.push(0);
        self.line_slots.len() - 1
    }

    pub fn drop_line_slot(&mut self, slot: usize) {
        self.line_slots.truncate(slot);
        if self.line_slot.is_some_and(|pending| pending >= slot) {
            self.line_slot = None;
        }
    }

    /// Output lines since the line recorded in `slot`.
    pub fn lines_from(&self, slot: usize) -> i32 {
        self.out.line - self.line_slots[slot]
    }

    pub fn add_to_line_slot(&mut self, slot: usize, lines: i32) {
        self.line_slots[slot] += lines;
    }

    // ---- items

    fn before_item(&mut self) {
        match self.last_tok {
            None => {}
            Some(T::LParen | T::LBrack) => self.prev_open = self.last_tok,
            Some(_) => self.prev_open = None,
        }
    }

    pub fn toggle(&mut self, mode: u8) {
        self.before_item();
        self.mode ^= mode;
    }

    pub fn ws(&mut self, ws: Ws) {
        self.before_item();
        if ws == Ws::Ignore {
            return;
        }
        if self.wsbuf.len() == WHITE_SPACE_LIMIT {
            self.write_whitespace(WHITE_SPACE_LIMIT);
        }
        self.wsbuf.push(ws);
        if matches!(ws, Ws::Newline | Ws::Formfeed) {
            self.implied_semi = false;
        }
        self.last_tok = None;
    }

    pub fn tok(&mut self, kind: T) {
        self.before_item();
        let text = token_text(kind);
        if may_combine(self.last_tok, text.as_bytes()[0]) {
            self.wsbuf.clear();
            self.wsbuf.push(Ws::Blank);
        }
        let implied_semi = matches!(
            kind,
            T::Break
                | T::Continue
                | T::Fallthrough
                | T::Return
                | T::PlusPlus
                | T::MinusMinus
                | T::RParen
                | T::RBrack
                | T::RBrace
        );
        self.last_tok = Some(kind);
        self.item(text, false, implied_semi);
    }

    pub fn ident(&mut self, name: &str) {
        self.before_item();
        self.last_tok = Some(T::Ident);
        self.item(name, false, true);
    }

    /// A literal, or source text that is printed as it is.
    pub fn literal(&mut self, kind: T, text: &str) {
        self.before_item();
        self.last_tok = Some(kind);
        self.item(text, true, true);
    }

    fn item(&mut self, text: &str, is_literal: bool, mut implied_semi: bool) {
        if self.aborted {
            return;
        }
        self.steps += 1;
        let next = self.pos;
        let (wrote_newline, dropped_formfeed) = self.flush(next, self.last_tok);
        let attached = std::mem::take(&mut self.after_attribute);
        // Blank lines of the source after a comment are kept, where a line
        // end is not a semicolon.
        if !self.implied_semi {
            let mut lines = (next.line - self.pos.line).min(MAX_NEWLINES);
            if wrote_newline && lines == MAX_NEWLINES {
                lines = MAX_NEWLINES - 1;
            }
            if attached {
                lines = if wrote_newline { 0 } else { lines.min(1) };
            }
            if lines > 0 {
                let byte = if dropped_formfeed { FORMFEED } else { b'\n' };
                self.write_byte(byte, lines);
                implied_semi = false;
            }
        }
        if let Some(slot) = self.line_slot.take() {
            self.line_slots[slot] = self.out.line;
        }
        self.write_string(next, text, is_literal);
        self.implied_semi = implied_semi;
    }

    /// Writes the comments before `next` and the pending white space.
    /// Returns whether a line end was written and whether a formfeed was
    /// dropped.
    pub fn flush(&mut self, next: Pos, tok: Option<T>) -> (bool, bool) {
        if self.comment_before(next) {
            self.intersperse_comments(next, tok)
        } else {
            self.write_whitespace(self.wsbuf.len());
            (false, false)
        }
    }

    // ---- raw output

    fn write_indent(&mut self) {
        let count = self.indent.max(0);
        self.output
            .extend(std::iter::repeat_n(b'\t', count as usize));
        self.pos.offset += i64::from(count);
        self.pos.column += count;
        self.out.column += count;
    }

    fn write_byte(&mut self, mut byte: u8, count: i32) {
        if self.end_alignment {
            match byte {
                b'\t' | 0x0B => byte = b' ',
                b'\n' | FORMFEED => {
                    byte = FORMFEED;
                    self.end_alignment = false;
                }
                _ => {}
            }
        }
        if self.out.column == 1 {
            self.write_indent();
        }
        self.output
            .extend(std::iter::repeat_n(byte, count.max(0) as usize));
        self.pos.offset += i64::from(count);
        if byte == b'\n' || byte == FORMFEED {
            self.pos.line += count;
            self.out.line += count;
            self.pos.column = 1;
            self.out.column = 1;
            self.aborted |= self.measure.is_some();
            return;
        }
        self.pos.column += count;
        self.out.column += count;
    }

    fn write_string(&mut self, pos: Pos, text: &str, is_literal: bool) {
        if self.out.column == 1 {
            self.write_indent();
        }
        if pos.is_valid() {
            self.pos = pos;
        }
        if is_literal {
            self.output.push(ESCAPE);
        }
        self.output.extend_from_slice(text.as_bytes());

        let mut lines = 0;
        let mut last_newline = 0;
        for (index, byte) in text.bytes().enumerate() {
            if byte == b'\n' || byte == FORMFEED {
                lines += 1;
                last_newline = index;
                self.end_alignment = true;
            }
        }
        let length = text.len() as i32;
        self.pos.offset += i64::from(length);
        if lines > 0 {
            self.pos.line += lines;
            self.out.line += lines;
            let column = length - last_newline as i32;
            self.pos.column = column;
            self.out.column = column;
        } else {
            self.pos.column += length;
            self.out.column += length;
        }
        if is_literal {
            self.output.push(ESCAPE);
        }
        self.last = self.pos;

        if let Some(limit) = self.measure {
            self.text_bytes += i64::from(length);
            self.steps += text.len() as u64;
            self.aborted |= lines > 0 || self.text_bytes > i64::from(limit);
        }
    }

    fn write_whitespace(&mut self, count: usize) {
        let mut index = 0;
        while index < count {
            match self.wsbuf[index] {
                Ws::Ignore => {}
                Ws::Indent => self.indent += 1,
                Ws::Unindent => self.indent = (self.indent - 1).max(0),
                ws @ (Ws::Newline | Ws::Formfeed) => {
                    // An unindent right after a line end belongs before it:
                    // that is how a label gets its place. The formfeed keeps
                    // a wide label from moving the lines before it.
                    if index + 1 < count && self.wsbuf[index + 1] == Ws::Unindent {
                        self.wsbuf[index] = Ws::Unindent;
                        self.wsbuf[index + 1] = Ws::Formfeed;
                        continue;
                    }
                    self.write_byte(if ws == Ws::Newline { b'\n' } else { FORMFEED }, 1);
                }
                Ws::Blank => self.write_byte(b' ', 1),
                Ws::Vtab => self.write_byte(0x0B, 1),
            }
            index += 1;
        }
        self.wsbuf.drain(..count);
    }

    // ---- comments

    fn next_comment(&mut self) {
        let cx = self.cx;
        if self.measure.is_none() && self.cindex < cx.groups.len() {
            let group = cx.groups[self.cindex];
            self.comment = self.cindex;
            self.cindex += 1;
            self.comment_offset = i64::from(cx.comments[group.first].pos);
            self.comment_newline = cx.group_has_newline(group);
        } else {
            self.comment_offset = INFINITY as i64;
        }
    }

    /// Whether a comment comes before `next` and can be written here: one
    /// with a line end in it cannot where a line end is a semicolon.
    pub fn comment_before(&self, next: Pos) -> bool {
        self.comment_offset < next.offset && (!self.implied_semi || !self.comment_newline)
    }

    /// The bytes of the comments before `next`.
    pub fn comment_size_before(&mut self, next: Pos) -> i32 {
        let cx = self.cx;
        let saved = (
            self.cindex,
            self.comment,
            self.comment_offset,
            self.comment_newline,
        );
        let mut size = 0;
        while self.comment_before(next) {
            let group = cx.groups[self.comment];
            let comments = &cx.comments[group.first..group.end];
            size += comments.iter().map(|c| (c.hi - c.lo) as i32).sum::<i32>();
            self.next_comment();
        }
        (
            self.cindex,
            self.comment,
            self.comment_offset,
            self.comment_newline,
        ) = saved;
        size
    }

    fn contains_linebreak(&self) -> bool {
        let is_break = |ws: &Ws| matches!(ws, Ws::Newline | Ws::Formfeed);
        self.wsbuf.iter().any(is_break)
    }

    /// The lines of a documentation comment in canonical form, if the
    /// comments of `group` are one: `None` when they are left as they are.
    fn doc_comment(&self, group: Group) -> Option<Vec<String>> {
        let cx = self.cx;
        let comments = &cx.comments[group.first..group.end];
        let first = cx.comment_text(comments[0]);
        if comments.len() == 1 && first.starts_with("/*") {
            // A block comment on one line, or with a column of stars, would
            // only get worse.
            let stars = |line: &str| line.trim_start_matches([' ', '\t']).starts_with('*');
            if !first.contains('\n') || first.split('\n').skip(1).all(stars) {
                return None;
            }
            let text = doc::reformat(&first[2..first.len() - 2]);
            return Some(vec![format!("/*\n{text}*/")]);
        }
        let mut text = String::new();
        let mut directives = Vec::new();
        for &comment in comments {
            let line = cx.comment_text(comment);
            let after = line.strip_prefix("//")?;
            if doc::is_directive(after) {
                directives.push(line.into_owned());
                continue;
            }
            text.push_str(after.strip_prefix(' ').unwrap_or(after));
            text.push('\n');
        }
        if text.is_empty() {
            return None;
        }
        let text = doc::reformat(&text);
        let mut lines: Vec<String> = text
            .lines()
            .map(|line| match line {
                "" => "//".to_owned(),
                line if line.starts_with('\t') => format!("//{line}"),
                line => format!("// {line}"),
            })
            .collect();
        if !directives.is_empty() {
            lines.push("//".to_owned());
            lines.extend(directives);
        }
        Some(lines)
    }

    fn intersperse_comments(&mut self, next: Pos, tok: Option<T>) -> (bool, bool) {
        let cx = self.cx;
        // Whether the last comment written is a line comment, and where it
        // starts.
        let mut last: Option<(bool, u32)> = None;
        while self.comment_before(next) {
            let group = cx.groups[self.comment];
            let comments = &cx.comments[group.first..group.end];
            let first_pos = cx.pos(comments[0].pos);
            // A comment in the first column right above the next token is
            // documentation. The comment above an import may be cgo's.
            let end = cx.comment_end(comments[comments.len() - 1]);
            let is_doc = tok != Some(T::Import) && first_pos.column == 1 && cx.pos(end + 1) == next;
            match self.doc_comment(group).filter(|_| is_doc) {
                Some(lines) if lines.is_empty() => {
                    // Nothing is left of the comment.
                    self.write_comment_prefix(first_pos, next, last.map(|last| last.0), tok);
                    self.pos = next;
                    self.last = next;
                    self.next_comment();
                    return self.write_comment_suffix(false);
                }
                Some(lines) => {
                    for line in lines {
                        self.write_comment_prefix(first_pos, next, last.map(|last| last.0), tok);
                        self.write_comment(&line, first_pos);
                        last = Some((line.starts_with("//"), comments[0].pos));
                    }
                }
                None => {
                    for &comment in comments {
                        let pos = cx.pos(comment.pos);
                        self.write_comment_prefix(pos, next, last.map(|last| last.0), tok);
                        self.write_comment(&cx.comment_text(comment), pos);
                        last = Some((cx.is_line_comment(comment), comment.pos));
                    }
                }
            }
            if is_doc {
                // The comment may have been rewritten: go on from where it
                // ends in the source.
                let original = comments[comments.len() - 1];
                last = Some((cx.is_line_comment(original), original.pos));
                self.pos = cx.pos(end);
                self.last = self.pos;
            }
            self.next_comment();
        }
        let Some((last_is_line, last_pos)) = last else {
            return (false, false);
        };
        let block = !last_is_line;

        // A block comment followed by a token on its line gets a blank
        // after it, except before a comma and before a bracket that does
        // not close the one just opened. Where a line end is pending and
        // allowed, the line end comes instead.
        let mut needs_linebreak = false;
        if self.mode & NO_EXTRA_BLANK == 0
            && block
            && cx.line(last_pos) == next.line
            && tok != Some(T::Comma)
            && (tok != Some(T::RParen) || self.prev_open == Some(T::LParen))
            && (tok != Some(T::RBrack) || self.prev_open == Some(T::LBrack))
        {
            if self.contains_linebreak() && self.mode & NO_EXTRA_LINEBREAK == 0 && self.level == 0 {
                needs_linebreak = true;
            } else {
                self.write_byte(b' ', 1);
            }
        }
        if !block
            || tok == Some(T::Eof)
            || tok == Some(T::RBrace) && self.mode & NO_EXTRA_LINEBREAK == 0
        {
            needs_linebreak = true;
        }
        self.write_comment_suffix(needs_linebreak)
    }

    /// The white space before the comment at `pos`. `prev` is the comment
    /// before it in the same run, `next` and `tok` the token after the run.
    fn write_comment_prefix(
        &mut self,
        pos: Pos,
        next: Pos,
        prev_is_line: Option<bool>,
        tok: Option<T>,
    ) {
        if self.output.is_empty() {
            return;
        }
        if pos.line == self.last.line && prev_is_line != Some(true) {
            // On the line of the last token: one separator.
            let mut has_separator = false;
            if prev_is_line.is_none() {
                let mut end = 0;
                for index in 0..self.wsbuf.len() {
                    match self.wsbuf[index] {
                        Ws::Blank => {
                            self.wsbuf[index] = Ws::Ignore;
                            continue;
                        }
                        Ws::Vtab => {
                            // The column of a commented field stays.
                            has_separator = true;
                            continue;
                        }
                        Ws::Indent => continue,
                        _ => {}
                    }
                    end = index;
                    break;
                }
                self.write_whitespace(end);
            }
            if !has_separator {
                let separator = if pos.line == next.line { b' ' } else { b'\t' };
                self.write_byte(separator, 1);
            }
            return;
        }

        // On a line of its own: at least one line end before it.
        let mut dropped_linebreak = false;
        let mut end = 0;
        for index in 0..self.wsbuf.len() {
            match self.wsbuf[index] {
                Ws::Blank | Ws::Vtab => {
                    self.wsbuf[index] = Ws::Ignore;
                    continue;
                }
                Ws::Indent => continue,
                Ws::Unindent => {
                    // All but the last unindent end an expression, not a
                    // block.
                    if self.wsbuf.get(index + 1) == Some(&Ws::Unindent) {
                        continue;
                    }
                    // The last one is applied when the comment is lined up
                    // with the next token; otherwise the comment belongs to
                    // the block that ends here, as before a `case`.
                    if tok != Some(T::RBrace) && pos.column == next.column {
                        continue;
                    }
                }
                Ws::Newline | Ws::Formfeed => {
                    self.wsbuf[index] = Ws::Ignore;
                    dropped_linebreak = prev_is_line.is_none();
                }
                Ws::Ignore => {}
            }
            end = index;
            break;
        }
        self.write_whitespace(end);

        let mut lines = 0;
        if pos.is_valid() && self.last.is_valid() {
            lines = (pos.line - self.last.line).max(0);
        }
        // A blank line before a comment at the top level is kept.
        if self.indent == 0 && dropped_linebreak {
            lines += 1;
        }
        if self.after_attribute {
            lines = lines.min(1);
        }
        if lines == 0 && prev_is_line == Some(true) {
            lines = 1;
        }
        if lines > 0 {
            self.write_byte(FORMFEED, lines.min(MAX_NEWLINES));
        }
    }

    fn write_comment(&mut self, text: &str, mut pos: Pos) {
        self.steps += 1;
        let indent = self.indent;
        if text.starts_with("//line ") && (!pos.is_valid() || pos.column == 1) {
            // A line directive works only in the first column.
            self.indent = 0;
        }
        if text.starts_with("//") {
            if super::build::is_go_build(text) {
                self.go_build.push(self.output.len());
            } else if super::build::is_plus_build(text) {
                self.plus_build.push(self.output.len());
            }
            self.write_string(pos, text.trim_end(), true);
            self.indent = indent;
            return;
        }

        let mut lines: Vec<Cow<str>> = text.split('\n').map(Cow::Borrowed).collect();
        // A comment that moves away from the first column must give the
        // same result when it is formatted again from where it lands.
        if pos.is_valid() && pos.column == 1 && self.indent > 0 {
            for line in &mut lines[1..] {
                *line = Cow::Owned(format!("   {line}"));
            }
        }
        strip_common_prefix(&mut lines);
        for (number, line) in lines.iter().enumerate() {
            if number > 0 {
                self.write_byte(FORMFEED, 1);
                pos = self.pos;
            }
            if !line.is_empty() {
                self.write_string(pos, line.trim_end(), true);
            }
        }
        self.indent = indent;
    }

    /// The white space after the last comment of a run.
    fn write_comment_suffix(&mut self, mut needs_linebreak: bool) -> (bool, bool) {
        let mut wrote_newline = false;
        let mut dropped_formfeed = false;
        for index in 0..self.wsbuf.len() {
            match self.wsbuf[index] {
                Ws::Blank | Ws::Vtab => self.wsbuf[index] = Ws::Ignore,
                Ws::Indent | Ws::Unindent | Ws::Ignore => {}
                ws @ (Ws::Newline | Ws::Formfeed) => {
                    if needs_linebreak {
                        needs_linebreak = false;
                        wrote_newline = true;
                    } else {
                        dropped_formfeed |= ws == Ws::Formfeed;
                        self.wsbuf[index] = Ws::Ignore;
                    }
                }
            }
        }
        self.write_whitespace(self.wsbuf.len());
        if needs_linebreak {
            self.write_byte(b'\n', 1);
            wrote_newline = true;
        }
        (wrote_newline, dropped_formfeed)
    }
}

/// Whether two tokens written without a blank would read as another token.
fn may_combine(prev: Option<T>, next: u8) -> bool {
    match prev {
        Some(T::Int) => next == b'.',
        Some(T::Plus) => next == b'+',
        Some(T::Minus) => next == b'-',
        Some(T::Slash) => next == b'*',
        Some(T::Lt) => next == b'-' || next == b'<',
        Some(T::Amp) => next == b'&' || next == b'^',
        _ => false,
    }
}

fn is_blank(text: &str) -> bool {
    text.bytes().all(|byte| byte <= b' ')
}

/// The start that `a` and `b` share and that is white space or stars.
fn common_prefix<'s>(a: &'s str, b: &str) -> &'s str {
    let shared = a
        .bytes()
        .zip(b.bytes())
        .take_while(|&(x, y)| x == y && (x <= b' ' || x == b'*'))
        .count();
    &a[..shared]
}

/// Removes from the lines of a block comment the indentation they have in
/// the source, so that the printer's indentation can take its place. The
/// first line starts with `/*` and has none.
fn strip_common_prefix(lines: &mut [Cow<str>]) {
    let count = lines.len();
    if count <= 1 {
        return;
    }
    let mut prefix: Option<String> = None;
    for line in &mut lines[1..count - 1] {
        if is_blank(line) {
            *line = Cow::Borrowed("");
        } else {
            let shared = match &prefix {
                Some(prefix) => common_prefix(prefix, line),
                None => common_prefix(line, line),
            };
            prefix = Some(shared.to_owned());
        }
    }
    let mut prefix = prefix.unwrap_or_else(|| {
        let last = &lines[count - 1];
        common_prefix(last, last).to_owned()
    });

    let mut line_of_stars = false;
    if let Some(star) = prefix.find('*') {
        // A column of stars: they stay lined up under the first one.
        prefix.truncate(star);
        if prefix.ends_with(' ') {
            prefix.pop();
        }
        line_of_stars = true;
    } else {
        let first = &lines[0];
        if is_blank(&first[2..]) {
            // Text that was indented against the `/*` stays indented: by up
            // to three blanks or one tab.
            let bytes = prefix.as_bytes();
            let mut end = bytes.len();
            let mut blanks = 0;
            while blanks < 3 && end > 0 && bytes[end - 1] == b' ' {
                end -= 1;
                blanks += 1;
            }
            if end == bytes.len() && end > 0 && bytes[end - 1] == b'\t' {
                end -= 1;
            }
            prefix.truncate(end);
        } else {
            // Text after the `/*`: the lines below are lined up with it,
            // two columns in, or one tab.
            let bytes = first.as_bytes();
            let mut end = 2;
            while end < bytes.len() && bytes[end] <= b' ' {
                end += 1;
            }
            let suffix = if end > 2 && bytes[2] == b'\t' {
                first[2..end].to_owned()
            } else {
                format!("  {}", &first[2..end])
            };
            if prefix.ends_with(&suffix) {
                prefix.truncate(prefix.len() - suffix.len());
            }
        }
    }

    let last = &lines[count - 1];
    let before = last.split("*/").next().unwrap_or("");
    if is_blank(before) {
        let closing = if line_of_stars { " */" } else { "*/" };
        lines[count - 1] = Cow::Owned(format!("{prefix}{closing}"));
    } else {
        prefix = common_prefix(&prefix, last).to_owned();
    }

    for line in &mut lines[1..] {
        if !line.is_empty() {
            let rest = line.get(prefix.len()..).unwrap_or("").to_owned();
            *line = Cow::Owned(rest);
        }
    }
}

/// The spelling of a token that has only one.
pub(super) fn token_text(kind: T) -> &'static str {
    match kind {
        T::Plus => "+",
        T::Minus => "-",
        T::Star => "*",
        T::Slash => "/",
        T::Percent => "%",
        T::Amp => "&",
        T::Pipe => "|",
        T::Caret => "^",
        T::Shl => "<<",
        T::Shr => ">>",
        T::AmpCaret => "&^",
        T::PlusEq => "+=",
        T::MinusEq => "-=",
        T::StarEq => "*=",
        T::SlashEq => "/=",
        T::PercentEq => "%=",
        T::AmpEq => "&=",
        T::PipeEq => "|=",
        T::CaretEq => "^=",
        T::ShlEq => "<<=",
        T::ShrEq => ">>=",
        T::AmpCaretEq => "&^=",
        T::AmpAmp => "&&",
        T::PipePipe => "||",
        T::Arrow => "<-",
        T::PlusPlus => "++",
        T::MinusMinus => "--",
        T::EqEq => "==",
        T::Lt => "<",
        T::Gt => ">",
        T::Eq => "=",
        T::Bang => "!",
        T::Tilde => "~",
        T::NotEq => "!=",
        T::LtEq => "<=",
        T::GtEq => ">=",
        T::ColonEq => ":=",
        T::Ellipsis => "...",
        T::LParen => "(",
        T::LBrack => "[",
        T::LBrace => "{",
        T::Comma => ",",
        T::Dot => ".",
        T::RParen => ")",
        T::RBrack => "]",
        T::RBrace => "}",
        T::Semi => ";",
        T::Colon => ":",
        T::Hash => "#",
        T::At => "@",
        T::Break => "break",
        T::Case => "case",
        T::Chan => "chan",
        T::Const => "const",
        T::Continue => "continue",
        T::Default => "default",
        T::Defer => "defer",
        T::Else => "else",
        T::Fallthrough => "fallthrough",
        T::For => "for",
        T::Func => "func",
        T::Go => "go",
        T::Goto => "goto",
        T::If => "if",
        T::Import => "import",
        T::Interface => "interface",
        T::Macro => "macro",
        T::Map => "map",
        T::Package => "package",
        T::Range => "range",
        T::Return => "return",
        T::Select => "select",
        T::Struct => "struct",
        T::Switch => "switch",
        T::Type => "type",
        T::Var => "var",
        T::Ident | T::Int | T::Float | T::Imag | T::Rune | T::String | T::Eof | T::Skipped => "?",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stripped(comment: &str) -> String {
        let mut lines: Vec<Cow<str>> = comment.split('\n').map(Cow::Borrowed).collect();
        strip_common_prefix(&mut lines);
        lines.join("\n")
    }

    #[test]
    fn the_indentation_of_a_block_comment_is_removed() {
        assert_eq!(stripped("/*\n\t\ta\n\t\t  b\n\t*/"), "/*\n\ta\n\t  b\n*/");
        assert_eq!(stripped("/* a\n\t   b\n\t*/"), "/* a\n   b\n*/");
        assert_eq!(stripped("/*\n\t * a\n\t * b\n\t */"), "/*\n * a\n * b\n */");
        assert_eq!(stripped("/* a\n\tb */"), "/* a\nb */");
        assert_eq!(stripped("/* one line */"), "/* one line */");
    }
}
