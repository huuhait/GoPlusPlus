//! Turns source text into tokens: Go's lexical rules plus `#`, `@` and the
//! keyword `macro`.

mod number;
mod string;

use std::mem;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{Dialect, FileId, SourceFile, Span};
use logos::{Filter, Lexer, Logos};

use crate::token::{Comment, Token, TokenKind};

/// The tokens of a file, ending with [`TokenKind::Eof`], and its comments in
/// source order.
#[derive(Debug, PartialEq, Eq)]
pub struct Lexed {
    pub tokens: Vec<Token>,
    pub comments: Vec<Comment>,
}

/// What the token callbacks share while one file is scanned.
pub struct Scan {
    file: FileId,
    dialect: Dialect,
    invalid_utf8: Vec<u32>,
    /// Go's rule: the last token on a line decides whether the line end is a `;`.
    insert_semi: bool,
    bad_literal: bool,
    comments: Vec<Comment>,
    diagnostics: Vec<Diagnostic>,
}

impl Scan {
    fn error(&mut self, code: Code, lo: usize, hi: usize, message: impl Into<String>) {
        let span = Span {
            file: self.file,
            lo: lo as u32,
            hi: hi as u32,
        };
        self.diagnostics.push(Diagnostic::new(code, span, message));
    }

    /// NUL, a byte order mark after the start, and invalid UTF-8 are errors
    /// wherever they are, inside comments and literals too.
    fn report_bad_bytes(&mut self, text: &str) {
        for offset in mem::take(&mut self.invalid_utf8) {
            let offset = offset as usize;
            let end = offset + char::REPLACEMENT_CHARACTER.len_utf8();
            self.error(Code::LexIllegalUtf8, offset, end, "invalid UTF-8 encoding");
            self.invalid_utf8.push(offset as u32);
        }
        let bytes = text.as_bytes();
        for (offset, &byte) in bytes.iter().enumerate() {
            if byte == 0 {
                self.error(
                    Code::LexIllegalNul,
                    offset,
                    offset + 1,
                    "invalid NUL character",
                );
            } else if byte == 0xEF && offset > 0 && bytes[offset..].starts_with(BOM.as_bytes()) {
                let message = "invalid BOM in the middle of the file";
                self.error(Code::LexIllegalBom, offset, offset + BOM.len(), message);
            }
        }
    }
}

const BOM: &str = "\u{FEFF}";

pub fn lex(file: FileId, source: &SourceFile, sink: &Sink) -> Lexed {
    let text = source.text();
    let end = text.len() as u32;
    let eof = Token {
        kind: TokenKind::Eof,
        lo: end,
        hi: end,
        bad: false,
    };
    if source.is_utf16() {
        let span = Span { file, lo: 0, hi: 0 };
        let message = "file is UTF-16; source files must be UTF-8";
        sink.push(Diagnostic::new(Code::LexUtf16, span, message));
        return Lexed {
            tokens: vec![eof],
            comments: Vec::new(),
        };
    }

    let mut scan = Scan {
        file,
        dialect: source.dialect(),
        invalid_utf8: source.invalid_utf8().to_vec(),
        insert_semi: false,
        bad_literal: false,
        comments: Vec::new(),
        diagnostics: Vec::new(),
    };
    scan.report_bad_bytes(text);

    let mut lexer = TokenKind::lexer_with_extras(text, scan);
    let mut tokens = Vec::new();
    while let Some(result) = lexer.next() {
        let span = lexer.span();
        // Every byte starts some pattern of the token table, so `Err` is not
        // expected; if the table ever loses one, it is an illegal character.
        let Ok(kind) = result else {
            report_illegal(&mut lexer);
            continue;
        };
        // Left out here, in this loop: a callback that makes the lexer skip
        // nests its calls one level deeper for every token skipped in a row.
        if kind == TokenKind::Skipped {
            continue;
        }
        // A comment that ends a line stands for a `;`: an empty token at its start.
        let end = if lexer.slice().starts_with("/*") {
            span.start
        } else {
            span.end
        };
        tokens.push(Token {
            kind,
            lo: span.start as u32,
            hi: end as u32,
            bad: mem::take(&mut lexer.extras.bad_literal),
        });
        lexer.extras.insert_semi = kind.ends_statement();
    }
    if lexer.extras.insert_semi {
        tokens.push(Token {
            kind: TokenKind::Semi,
            ..eof
        });
    }
    tokens.push(eof);

    let scan = lexer.extras;
    for diagnostic in scan.diagnostics {
        sink.push(diagnostic);
    }
    Lexed {
        tokens,
        comments: scan.comments,
    }
}

/// `U+0024 '$'`, as Go's `%#U` prints a character.
fn describe_char(c: char) -> String {
    if c.is_control() {
        format!("U+{:04X}", u32::from(c))
    } else {
        format!("U+{:04X} '{c}'", u32::from(c))
    }
}

fn report_illegal(lexer: &mut Lexer<TokenKind>) {
    let span = lexer.span();
    if let Some(c) = lexer.slice().chars().next() {
        let message = format!("invalid character {}", describe_char(c));
        lexer
            .extras
            .error(Code::LexIllegalChar, span.start, span.end, message);
    }
}

/// Extends the match over the identifier characters that follow it.
fn bump_identifier(lexer: &mut Lexer<TokenKind>) {
    let rest = lexer.remainder();
    let length = rest
        .find(|c: char| !unicode_ident::is_xid_continue(c))
        .unwrap_or(rest.len());
    lexer.bump(length);
}

pub(crate) fn word(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    bump_identifier(lexer);
    match TokenKind::keyword(lexer.slice()) {
        Some(TokenKind::Macro) if lexer.extras.dialect == Dialect::Go => TokenKind::Ident,
        Some(keyword) => keyword,
        None => TokenKind::Ident,
    }
}

pub(crate) fn non_ascii(lexer: &mut Lexer<TokenKind>) -> Filter<TokenKind> {
    let start = lexer.span().start;
    let Some(c) = lexer.slice().chars().next() else {
        return Filter::Emit(TokenKind::Skipped);
    };
    if unicode_ident::is_xid_start(c) {
        bump_identifier(lexer);
        return Filter::Emit(TokenKind::Ident);
    }
    let scan = &mut lexer.extras;
    let reported_as_bad_byte = c == '\u{FEFF}'
        || c == char::REPLACEMENT_CHARACTER
            && scan.invalid_utf8.binary_search(&(start as u32)).is_ok();
    if reported_as_bad_byte {
        return Filter::Emit(TokenKind::Skipped);
    }
    let end = start + c.len_utf8();
    match c {
        '\u{2018}' | '\u{2019}' | '\u{201C}' | '\u{201D}' => {
            let neutral = if c < '\u{201C}' { '\'' } else { '"' };
            let message = format!("curly quotation mark {c:?} (use neutral {neutral:?})");
            scan.error(Code::LexCurlyQuote, start, end, message);
        }
        _ => {
            let message = format!("invalid character {}", describe_char(c));
            scan.error(Code::LexIllegalChar, start, end, message);
        }
    }
    Filter::Emit(TokenKind::Skipped)
}

/// A character no token starts with. NUL was reported with the bad bytes.
pub(crate) fn illegal(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    if lexer.slice() != "\0" {
        report_illegal(lexer);
    }
    TokenKind::Skipped
}

/// `#` or `@`.
pub(crate) fn gopp_only(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    if lexer.extras.dialect == Dialect::Go {
        report_illegal(lexer);
        return TokenKind::Skipped;
    }
    if lexer.slice() == "#" {
        TokenKind::Hash
    } else {
        TokenKind::At
    }
}

pub(crate) fn number(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    let (text, start) = (lexer.source(), lexer.span().start);
    let (kind, end, ok) = number::scan(&mut lexer.extras, text, start);
    finish_literal(lexer, end, ok);
    kind
}

pub(crate) fn rune(lexer: &mut Lexer<TokenKind>) {
    let (text, start) = (lexer.source(), lexer.span().start);
    let (end, ok) = string::rune(&mut lexer.extras, text, start);
    finish_literal(lexer, end, ok);
}

pub(crate) fn string(lexer: &mut Lexer<TokenKind>) {
    let (text, start) = (lexer.source(), lexer.span().start);
    let (end, ok) = string::string(&mut lexer.extras, text, start);
    finish_literal(lexer, end, ok);
}

pub(crate) fn raw_string(lexer: &mut Lexer<TokenKind>) {
    let (text, start) = (lexer.source(), lexer.span().start);
    let (end, ok) = string::raw_string(&mut lexer.extras, text, start);
    finish_literal(lexer, end, ok);
}

fn finish_literal(lexer: &mut Lexer<TokenKind>, end: usize, ok: bool) {
    // A scanner may stop before the end of the pattern that started it
    // (`.5` matched, `.` scanned), never before its first byte.
    let matched = lexer.span().end;
    lexer.bump(end.saturating_sub(matched));
    lexer.extras.bad_literal = !ok;
}

pub(crate) fn newline(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    if lexer.extras.insert_semi {
        TokenKind::Semi
    } else {
        TokenKind::Skipped
    }
}

pub(crate) fn line_comment(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    let span = lexer.span();
    let directive = lexer.slice().strip_prefix("//gopp:");
    if let Some(directive) = directive {
        let name = directive.split_whitespace().next().unwrap_or("");
        if matches!(name, "cflags" | "cxxstd" | "ldflags") {
            let file = lexer.extras.file;
            let span = Span {
                file,
                lo: span.start as u32,
                hi: span.end as u32,
            };
            let message = format!("//gopp:{name} is not a directive of source files");
            let note = format!("add the line `{}` to gopp.mod", directive.trim_end());
            let diagnostic = Diagnostic::new(Code::GoppDirectiveInSource, span, message).with_note(
                Code::NoteGoppModLine,
                None,
                note,
            );
            lexer.extras.diagnostics.push(diagnostic);
        }
    }
    lexer.extras.comments.push(Comment {
        lo: span.start as u32,
        hi: span.end as u32,
        directive: directive.is_some(),
    });
    TokenKind::Skipped
}

/// A comment with a line end in it ends the line, as in Go.
pub(crate) fn block_comment(lexer: &mut Lexer<TokenKind>) -> TokenKind {
    let start = lexer.span().start;
    let rest = lexer.remainder();
    let length = match rest.find("*/") {
        Some(close) => close + 2,
        None => {
            let message = "comment not terminated";
            lexer
                .extras
                .error(Code::LexCommentUnterminated, start, start + 2, message);
            rest.len()
        }
    };
    let ends_line = rest[..length].contains('\n');
    lexer.bump(length);
    lexer.extras.comments.push(Comment {
        lo: start as u32,
        hi: lexer.span().end as u32,
        directive: false,
    });
    if ends_line && lexer.extras.insert_semi {
        TokenKind::Semi
    } else {
        TokenKind::Skipped
    }
}

#[cfg(test)]
mod tests;
