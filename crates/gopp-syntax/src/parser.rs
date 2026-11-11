//! The parser: Go's grammar with the Go++ productions (spec appendix B).
//!
//! It follows the decisions of Go's own parser, so that Go code gets the
//! same tree and the same errors. It always returns a tree: a syntax error
//! leaves a `Bad` node, reports a diagnostic and resynchronizes.

mod decl;
mod expr;
mod stmt;
mod types;

use std::cell::Cell;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{Dialect, FileId, SourceFile, Span};
use la_arena::Arena;
use lasso::ThreadedRodeo;

use crate::ast::*;
use crate::lexer;
use crate::token::{Token, TokenKind};

/// How deep expressions, types and statements may nest.
const MAX_DEPTH: u32 = 1024;

pub fn parse(id: FileId, source: &SourceFile, names: &ThreadedRodeo, sink: &Sink) -> File {
    parse_counted(id, source, names, sink).0
}

/// The tree and the number of times the parser looked at a token.
fn parse_counted(
    id: FileId,
    source: &SourceFile,
    names: &ThreadedRodeo,
    sink: &Sink,
) -> (File, u64) {
    let lexed = lexer::lex(id, source, sink);
    let mut parser = Parser {
        id,
        source,
        text: source.text(),
        gopp: source.dialect() == Dialect::Gopp,
        tokens: lexed.tokens,
        pos: 0,
        names,
        sink,
        exprs: Arena::new(),
        stmts: Arena::new(),
        decls: Arena::new(),
        expr_level: 0,
        depth: 0,
        func_depth: 0,
        last_error_line: 0,
        groups: 0,
        steps: Cell::new(0),
    };
    let (package, decls) = parser.file();
    let file = File {
        id,
        package,
        decls,
        comments: lexed.comments,
        exprs: parser.exprs,
        stmts: parser.stmts,
        decl_nodes: parser.decls,
    };
    (file, parser.steps.get())
}

struct Parser<'a> {
    id: FileId,
    source: &'a SourceFile,
    text: &'a str,
    gopp: bool,
    tokens: Vec<Token>,
    pos: usize,
    names: &'a ThreadedRodeo,
    sink: &'a Sink,
    exprs: Arena<Expr>,
    stmts: Arena<Stmt>,
    decls: Arena<Decl>,
    /// Negative in the header of `if`, `for` and `switch`, where `{` opens
    /// the block and not a composite literal; raised again inside brackets.
    expr_level: i32,
    depth: u32,
    func_depth: u32,
    /// Line of the last reported error, 0 before the first.
    last_error_line: u32,
    groups: u32,
    /// Token looks so far: the measure of work in the scaling tests.
    steps: Cell<u64>,
}

impl Parser<'_> {
    fn kind(&self) -> TokenKind {
        self.steps.set(self.steps.get() + 1);
        self.tokens[self.pos].kind
    }

    fn at(&self, kind: TokenKind) -> bool {
        self.kind() == kind
    }

    /// The kind of the token `n` after the current one.
    fn peek(&self, n: usize) -> TokenKind {
        self.steps.set(self.steps.get() + 1);
        let index = (self.pos + n).min(self.tokens.len() - 1);
        self.tokens[index].kind
    }

    fn token_text(&self, token: Token) -> &str {
        &self.text[token.lo as usize..token.hi as usize]
    }

    /// Takes the current token. The final `Eof` is never passed.
    fn bump(&mut self) -> Token {
        let token = self.tokens[self.pos];
        if self.pos + 1 < self.tokens.len() {
            self.pos += 1;
        }
        token
    }

    fn got(&mut self, kind: TokenKind) -> bool {
        let found = self.at(kind);
        if found {
            self.bump();
        }
        found
    }

    fn want(&mut self, kind: TokenKind) {
        if !self.got(kind) {
            self.expected(fixed_text(kind));
            self.advance(&[]);
        }
    }

    /// Start of the current token.
    fn lo(&self) -> u32 {
        self.tokens[self.pos].lo
    }

    /// End of the token before the current one.
    fn end(&self) -> u32 {
        match self.pos.checked_sub(1) {
            Some(previous) => self.tokens[previous].hi,
            None => 0,
        }
    }

    fn span_from(&self, lo: u32) -> Span {
        Span {
            file: self.id,
            lo,
            hi: self.end().max(lo),
        }
    }

    fn token_span(&self, token: Token) -> Span {
        Span {
            file: self.id,
            lo: token.lo,
            hi: token.hi,
        }
    }

    fn here(&self) -> Span {
        self.token_span(self.tokens[self.pos])
    }

    fn alloc_expr(&mut self, kind: ExprKind, lo: u32) -> ExprId {
        let span = self.span_from(lo);
        self.exprs.alloc(Expr { kind, span })
    }

    fn bad_expr(&mut self) -> ExprId {
        let span = Span {
            hi: self.lo(),
            ..self.here()
        };
        self.exprs.alloc(Expr {
            kind: ExprKind::Bad,
            span,
        })
    }

    fn alloc_stmt(&mut self, kind: StmtKind, lo: u32) -> StmtId {
        let span = self.span_from(lo);
        self.stmts.alloc(Stmt { kind, span })
    }

    fn expr_lo(&self, expr: ExprId) -> u32 {
        self.exprs[expr].span.lo
    }

    fn intern(&self, token: Token) -> Ident {
        Ident {
            name: self.names.get_or_intern(self.token_text(token)),
            span: self.token_span(token),
        }
    }

    /// An identifier; if there is none, an error and the name `_`.
    fn ident(&mut self) -> Ident {
        if self.at(TokenKind::Ident) {
            let token = self.bump();
            return self.intern(token);
        }
        let blank = self.blank_ident();
        self.expected("name");
        self.advance(&[]);
        blank
    }

    /// The name `_` at the current position, standing in for a missing name.
    fn blank_ident(&self) -> Ident {
        Ident {
            name: self.names.get_or_intern_static("_"),
            span: Span {
                hi: self.lo(),
                ..self.here()
            },
        }
    }

    fn name_expr(&mut self, ident: Ident) -> ExprId {
        self.exprs.alloc(Expr {
            kind: ExprKind::Name(ident.name),
            span: ident.span,
        })
    }

    fn ident_list(&mut self, first: Ident) -> Vec<Ident> {
        let mut list = vec![first];
        while self.got(TokenKind::Comma) {
            list.push(self.ident());
        }
        list
    }

    /// Reports at most one syntax error per line, as Go's compiler does: the
    /// errors after the first are mostly its consequences.
    fn error_at(&mut self, code: Code, span: Span, message: impl Into<String>) {
        let line = self.source.line_col(span.lo).line;
        if line == self.last_error_line {
            return;
        }
        self.last_error_line = line;
        self.sink.push(Diagnostic::new(code, span, message));
    }

    /// `syntax error: <message>` at the current token.
    fn syntax_error(&mut self, code: Code, message: &str) {
        self.error_at(code, self.here(), format!("syntax error: {message}"));
    }

    /// `syntax error: unexpected <token><rest>` at the current token.
    fn unexpected(&mut self, code: Code, rest: &str) {
        // An error at the end of the file after another one says nothing new.
        if self.at(TokenKind::Eof) && self.last_error_line != 0 {
            return;
        }
        let message = format!("syntax error: unexpected {}{rest}", self.describe());
        self.error_at(code, self.here(), message);
    }

    fn expected(&mut self, what: &str) {
        self.unexpected(Code::ExpectedFound, &format!(", expected {what}"));
    }

    fn describe(&self) -> String {
        let token = self.tokens[self.pos];
        let text = self.token_text(token);
        match token.kind {
            TokenKind::Ident => format!("name {text}"),
            TokenKind::Int
            | TokenKind::Float
            | TokenKind::Imag
            | TokenKind::Rune
            | TokenKind::String => format!("literal {text}"),
            TokenKind::Semi if text == ";" => "semicolon".to_owned(),
            TokenKind::Semi if (token.lo as usize) < self.text.len() => "newline".to_owned(),
            TokenKind::Semi | TokenKind::Eof => "EOF".to_owned(),
            _ if TokenKind::keyword(text).is_some() => format!("keyword {text}"),
            _ => text.to_owned(),
        }
    }

    /// Skips to the next token of `follow` after an error. Inside a function
    /// a keyword that starts a statement stops it too. With an empty
    /// `follow` it skips one token.
    fn advance(&mut self, follow: &[TokenKind]) {
        if follow.is_empty() {
            self.bump();
            return;
        }
        loop {
            let kind = self.kind();
            let stop = kind == TokenKind::Eof
                || follow.contains(&kind)
                || self.func_depth > 0 && starts_statement(kind);
            if stop {
                break;
            }
            self.bump();
        }
    }

    /// Parses `item`s separated by `separator` up to `close`, which it
    /// consumes. An item returns `true` to end the list. Returns where the
    /// closing token was expected.
    fn list(
        &mut self,
        context: &str,
        separator: TokenKind,
        close: TokenKind,
        mut item: impl FnMut(&mut Self) -> bool,
    ) -> u32 {
        let mut done = false;
        while !self.at(TokenKind::Eof) && !self.at(close) && !done {
            done = item(self);
            if !self.got(separator) && !self.at(close) {
                let code = if self.at(TokenKind::Semi) {
                    Code::MissingCommaNewline
                } else {
                    Code::MissingComma
                };
                let (separator, close_text) = (fixed_text(separator), fixed_text(close));
                let rest = format!(" in {context}; possibly missing {separator} or {close_text}");
                self.unexpected(code, &rest);
                self.advance(&[TokenKind::RParen, TokenKind::RBrack, TokenKind::RBrace]);
                if !self.at(close) {
                    return self.lo();
                }
            }
        }
        let end = self.lo();
        self.want(close);
        end
    }

    /// Enters one more level of nesting. Past the limit it reports the
    /// error once and moves to the end of the file, so that every caller
    /// returns without looking at more input.
    fn enter(&mut self, code: Code, what: &str) -> bool {
        if self.depth >= MAX_DEPTH {
            if !self.at(TokenKind::Eof) {
                let message = format!("syntax error: {what} nested deeper than {MAX_DEPTH} levels");
                self.last_error_line = 0;
                self.error_at(code, self.here(), message);
                self.pos = self.tokens.len() - 1;
            }
            return false;
        }
        self.depth += 1;
        true
    }

    fn leave(&mut self) {
        self.depth -= 1;
    }

    /// LEX-10: macro and attribute names start with a capital letter.
    fn check_macro_name(&mut self, name: Ident) {
        let text = &self.text[name.span.lo as usize..name.span.hi as usize];
        if text.starts_with(|c: char| c.is_lowercase() || c == '_') {
            let message = format!("macro name {text} must start with a capital letter");
            self.error_at(Code::MacroNameLowercase, name.span, message);
        }
    }

    /// `Name` or `pkg.Name` after `#` or `@`.
    fn macro_ref(&mut self) -> MacroRef {
        let first = self.ident();
        let path = if self.got(TokenKind::Dot) {
            MacroRef {
                package: Some(first),
                name: self.ident(),
            }
        } else {
            MacroRef {
                package: None,
                name: first,
            }
        };
        self.check_macro_name(path.name);
        path
    }

    /// `@Name` or `@Name(args)`, at the `@`.
    fn attribute(&mut self) -> Attribute {
        let lo = self.lo();
        self.bump();
        let path = self.macro_ref();
        let args = self.at(TokenKind::LParen).then(|| {
            self.bump();
            self.macro_arguments()
        });
        Attribute {
            path,
            args,
            span: self.span_from(lo),
        }
    }
}

/// How a token of fixed spelling is named in an "expected" message.
fn fixed_text(kind: TokenKind) -> &'static str {
    match kind {
        TokenKind::Ident => "name",
        TokenKind::Comma => "comma",
        TokenKind::Semi => "semicolon or newline",
        TokenKind::Colon => ":",
        TokenKind::Eq => "=",
        TokenKind::LParen => "(",
        TokenKind::RParen => ")",
        TokenKind::LBrack => "[",
        TokenKind::RBrack => "]",
        TokenKind::LBrace => "{",
        TokenKind::RBrace => "}",
        TokenKind::Chan => "chan",
        TokenKind::Struct => "struct",
        TokenKind::Interface => "interface",
        TokenKind::Package => "package",
        TokenKind::Select => "select",
        TokenKind::If => "if",
        TokenKind::For => "for",
        TokenKind::Switch => "switch",
        _ => "token",
    }
}

fn starts_statement(kind: TokenKind) -> bool {
    matches!(
        kind,
        TokenKind::Break
            | TokenKind::Const
            | TokenKind::Continue
            | TokenKind::Defer
            | TokenKind::Fallthrough
            | TokenKind::For
            | TokenKind::Go
            | TokenKind::Goto
            | TokenKind::If
            | TokenKind::Return
            | TokenKind::Select
            | TokenKind::Switch
            | TokenKind::Type
            | TokenKind::Var
    )
}

#[cfg(test)]
mod tests;
