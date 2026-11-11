//! What the printer knows about a file besides its tree: the tokens, the
//! line table, the comments, and the top-level declarations as written.

use std::borrow::Cow;
use std::collections::{HashMap, HashSet};

use gopp_diag::Sink;
use gopp_span::{Dialect, SourceFile};
use lasso::ThreadedRodeo;

use super::printer::Pos;
use crate::ast::*;
use crate::lexer;
use crate::token::{Token, TokenKind as T};

/// A comment and where the printer takes it to be: sorting imports moves
/// the comments of the import lines with them.
#[derive(Clone, Copy)]
pub(super) struct Comment {
    pub pos: u32,
    pub lo: u32,
    pub hi: u32,
}

/// Comments with no token and no blank line between them: `first..end` in
/// the comment list.
#[derive(Clone, Copy)]
pub(super) struct Group {
    pub first: usize,
    pub end: usize,
}

/// One `import`, `const`, `var` or `type` declaration with its
/// specifications; the tree keeps only the specifications.
pub(super) struct GenDecl {
    pub keyword: T,
    pub pos: u32,
    /// Where `(` and `)` are, for a group.
    pub parens: Option<(u32, u32)>,
    pub specs: Vec<Spec>,
    /// Attributes before a group without specifications, which the tree
    /// does not have: their source text.
    pub orphans: Vec<(u32, u32)>,
}

#[derive(Clone, Copy)]
pub(super) struct Spec {
    pub decl: DeclId,
    pub pos: u32,
    pub end: u32,
    /// An import that took the place of another when its run was sorted.
    pub moved: bool,
}

pub(super) enum Item {
    Gen(GenDecl),
    /// A function, macro or macro call.
    Other(DeclId),
}

pub(super) struct Cx<'a> {
    pub file: &'a File,
    pub text: &'a str,
    pub names: &'a ThreadedRodeo,
    pub gopp: bool,
    pub tokens: Vec<Token>,
    /// Where each line starts.
    pub lines: Vec<u32>,
    pub comments: Vec<Comment>,
    pub groups: Vec<Group>,
    /// Tokens with a comment that ends on the line before them: the index
    /// of each.
    lead: HashSet<usize>,
    /// Tokens reached over a comment that ends the line of the token
    /// before: the index of each, and its comments.
    line_comments: HashMap<usize, Group>,
    pub items: Vec<Item>,
}

impl<'a> Cx<'a> {
    pub fn new(file: &'a File, source: &'a SourceFile, names: &'a ThreadedRodeo) -> Self {
        let text = source.text();
        // The file was parsed before, so its lexical errors are known.
        let lexed = lexer::lex(file.id, source, &Sink::new(0));
        let line_starts = text.match_indices('\n').map(|(at, _)| at as u32 + 1);
        let comments = lexed.comments.iter().map(|comment| Comment {
            pos: comment.lo,
            lo: comment.lo,
            hi: comment.hi,
        });
        let mut cx = Self {
            file,
            text,
            names,
            gopp: source.dialect() == Dialect::Gopp,
            tokens: lexed.tokens,
            lines: std::iter::once(0).chain(line_starts).collect(),
            comments: comments.collect(),
            groups: Vec::new(),
            lead: HashSet::new(),
            line_comments: HashMap::new(),
            items: Vec::new(),
        };
        cx.group_comments();
        cx.collect_items();
        super::imports::sort(&mut cx);
        cx
    }

    pub fn line(&self, offset: u32) -> i32 {
        self.lines.partition_point(|&start| start <= offset) as i32
    }

    pub fn pos(&self, offset: u32) -> Pos {
        let line = self.line(offset);
        Pos {
            offset: i64::from(offset),
            line,
            column: (offset - self.lines[line as usize - 1]) as i32 + 1,
        }
    }

    /// Joins `line` with the line after it.
    pub fn merge_line(&mut self, line: i32) {
        if line >= 1 && (line as usize) < self.lines.len() {
            self.lines.remove(line as usize);
        }
    }

    pub fn source(&self, lo: u32, hi: u32) -> &'a str {
        &self.text[lo as usize..hi as usize]
    }

    pub fn name(&self, ident: Ident) -> &'a str {
        self.names.resolve(&ident.name)
    }

    // ---- tokens

    /// The index of the first token that starts at `offset` or later.
    pub fn token_at(&self, offset: u32) -> usize {
        self.tokens.partition_point(|token| token.lo < offset)
    }

    pub fn kind(&self, index: usize) -> T {
        self.tokens.get(index).map_or(T::Eof, |token| token.kind)
    }

    pub fn lo(&self, index: usize) -> u32 {
        let last = self.tokens.len() - 1;
        self.tokens[index.min(last)].lo
    }

    /// Where the first `kind` token at `offset` or later starts. It is
    /// looked for among the next few tokens only: every caller knows it to
    /// be there.
    pub fn find(&self, offset: u32, kind: T) -> u32 {
        let from = self.token_at(offset);
        let found = (from..from + 4).find(|&index| self.kind(index) == kind);
        self.lo(found.unwrap_or(from))
    }

    /// The index of the token that closes a list whose last element ends
    /// at `end`: the list may end in a comma.
    pub fn closing(&self, end: u32) -> usize {
        let mut index = self.token_at(end);
        while matches!(self.kind(index), T::Comma | T::Semi) {
            index += 1;
        }
        index
    }

    fn is_explicit_semi(&self, token: Token) -> bool {
        token.hi > token.lo && self.text.as_bytes()[token.lo as usize] == b';'
    }

    // ---- comments

    pub fn is_line_comment(&self, comment: Comment) -> bool {
        self.text.as_bytes()[comment.lo as usize + 1] == b'/'
    }

    /// The comment without carriage returns.
    pub fn comment_text(&self, comment: Comment) -> Cow<'a, str> {
        let text = self.source(comment.lo, comment.hi);
        if text.contains('\r') {
            Cow::Owned(text.replace('\r', ""))
        } else {
            Cow::Borrowed(text)
        }
    }

    pub fn group_has_newline(&self, group: Group) -> bool {
        let comments = &self.comments[group.first..group.end];
        let line = self.line(comments[0].pos);
        comments.iter().any(|&comment| {
            self.line(comment.pos) != line
                || self.is_line_comment(comment)
                || self.source(comment.lo, comment.hi).contains('\n')
        })
    }

    /// Where the comment ends when its carriage returns do not count, as
    /// `gofmt` has it.
    pub fn comment_end(&self, comment: Comment) -> u32 {
        comment.pos + self.comment_text(comment).len() as u32
    }

    pub fn group_pos(&self, group: Group) -> u32 {
        self.comments[group.first].pos
    }

    pub fn group_end(&self, group: Group) -> u32 {
        let last = self.comments[group.end - 1];
        last.pos + (last.hi - last.lo)
    }

    /// Whether the node whose first token starts at `offset` has a
    /// documentation comment: one that ends on the line before it.
    pub fn has_doc(&self, offset: u32) -> bool {
        self.lead.contains(&self.token_at(offset))
    }

    /// The comment that ends the line of a specification or field ending
    /// at `end`.
    pub fn line_comment(&self, end: u32) -> Option<Group> {
        let index = self.token_at(end);
        let token = *self.tokens.get(index)?;
        if token.kind != T::Semi {
            return None;
        }
        let reached = index + usize::from(self.is_explicit_semi(token));
        self.line_comments.get(&reached).copied()
    }

    /// Groups the comments and finds which are documentation or end a
    /// line, by the rules of Go's parser: `gofmt` aligns by them.
    fn group_comments(&mut self) {
        let mut next = 0;
        for index in 0..self.tokens.len() {
            let token = self.tokens[index];
            let first = next;
            // The `;` a block comment with a line end stands for is at the
            // start of that comment, and comes after it.
            let stands_for = |comment: &Comment| {
                comment.lo == token.lo && token.kind == T::Semi && token.lo == token.hi
            };
            while self
                .comments
                .get(next)
                .is_some_and(|comment| comment.lo < token.lo || stands_for(comment))
            {
                next += 1;
            }
            if first == next {
                continue;
            }

            let token_line = self.line(token.lo);
            let mut at = first;
            let same_line = index > 0
                && self.line(self.comments[first].lo) == self.line(self.tokens[index - 1].lo);
            if same_line {
                let (end, end_line) = self.group(at, next, 0);
                let group = Group { first: at, end };
                self.groups.push(group);
                let ends_line = if end < next {
                    self.line(self.comments[end].lo) != end_line
                } else {
                    token_line != end_line || matches!(token.kind, T::Semi | T::Eof)
                };
                if ends_line {
                    self.line_comments.insert(index, group);
                }
                at = end;
            }
            let mut end_line = -1;
            while at < next {
                let (end, line) = self.group(at, next, 1);
                self.groups.push(Group { first: at, end });
                end_line = line;
                at = end;
            }
            if end_line + 1 == token_line {
                self.lead.insert(index);
            }
        }
    }

    /// The comments from `first` on that follow each other within `gap`
    /// lines: where they end in the list, and the line the last one ends on.
    fn group(&self, first: usize, limit: usize, gap: i32) -> (usize, i32) {
        let mut end = first;
        let mut end_line = self.line(self.comments[first].lo);
        while end < limit && self.line(self.comments[end].lo) <= end_line + gap {
            let comment = self.comments[end];
            let text = self.source(comment.lo, comment.hi);
            end_line = self.line(comment.lo) + text.matches('\n').count() as i32;
            end += 1;
        }
        (end, end_line)
    }

    // ---- declarations

    fn collect_items(&mut self) {
        let file = self.file;
        let mut items = Vec::new();
        let mut scanned = match file.package {
            Some(package) => self.token_at(package.span.hi),
            None => 0,
        };
        let mut index = 0;
        while index < file.decls.len() {
            let id = file.decls[index];
            let decl = &file.decl_nodes[id];
            let keyword = match decl.kind {
                DeclKind::Import { .. } => T::Import,
                DeclKind::Const { .. } => T::Const,
                DeclKind::Var { .. } => T::Var,
                DeclKind::Type { .. } => T::Type,
                _ => {
                    let start = decl.attrs.first().map_or(decl.span.lo, |attr| attr.span.lo);
                    self.empty_groups(scanned, self.token_at(start), &mut items);
                    scanned = self.token_at(decl.span.hi);
                    items.push(Item::Other(id));
                    index += 1;
                    continue;
                }
            };

            let mut last = index + 1;
            let (pos, parens) = if decl.group.is_some() {
                while file
                    .decls
                    .get(last)
                    .is_some_and(|&next| file.decl_nodes[next].group == decl.group)
                {
                    last += 1;
                }
                let open = self.token_at(decl.span.lo).saturating_sub(1);
                let end = file.decl_nodes[file.decls[last - 1]].span.hi;
                let close = self.closing(end);
                let parens = (self.lo(open), self.lo(close));
                (self.lo(open.saturating_sub(1)), Some(parens))
            } else {
                (decl.span.lo, None)
            };
            let start = decl.attrs.first().map_or(pos, |attr| attr.span.lo);
            self.empty_groups(scanned, self.token_at(start), &mut items);
            scanned = match parens {
                Some((_, close)) => self.token_at(close) + 1,
                None => self.token_at(decl.span.hi),
            };
            let specs = file.decls[index..last].iter().map(|&decl| {
                let span = file.decl_nodes[decl].span;
                Spec {
                    decl,
                    pos: span.lo,
                    end: span.hi,
                    moved: false,
                }
            });
            items.push(Item::Gen(GenDecl {
                keyword,
                pos,
                parens,
                specs: specs.collect(),
                orphans: Vec::new(),
            }));
            index = last;
        }
        self.empty_groups(scanned, self.tokens.len(), &mut items);
        self.items = items;
    }

    /// The groups without specifications among the tokens `from..to`,
    /// which lie between two declarations of the tree.
    fn empty_groups(&self, from: usize, to: usize, items: &mut Vec<Item>) {
        let mut orphans = Vec::new();
        let mut index = from;
        while index < to {
            let keyword = self.kind(index);
            match keyword {
                T::Import | T::Const | T::Var | T::Type
                    if self.kind(index + 1) == T::LParen && self.kind(index + 2) == T::RParen =>
                {
                    items.push(Item::Gen(GenDecl {
                        keyword,
                        pos: self.lo(index),
                        parens: Some((self.lo(index + 1), self.lo(index + 2))),
                        specs: Vec::new(),
                        orphans: std::mem::take(&mut orphans),
                    }));
                    index += 3;
                }
                T::At => {
                    let start = index;
                    index += 2;
                    if self.kind(index) == T::Dot {
                        index += 2;
                    }
                    if self.kind(index) == T::LParen {
                        let mut depth = 0;
                        while index < to {
                            match self.kind(index) {
                                T::LParen => depth += 1,
                                T::RParen => depth -= 1,
                                _ => {}
                            }
                            index += 1;
                            if depth == 0 {
                                break;
                            }
                        }
                    }
                    orphans.push((self.lo(start), self.tokens[index - 1].hi));
                }
                _ => index += 1,
            }
        }
    }

    /// The declarations of a `const`, `var` or `type` statement.
    pub fn statement_decl(&self, stmt: &Stmt, decls: &[DeclId]) -> GenDecl {
        let file = self.file;
        let first = self.token_at(stmt.span.lo);
        let grouped = match decls.first() {
            Some(&decl) => file.decl_nodes[decl].group.is_some(),
            None => true,
        };
        let specs = decls.iter().map(|&decl| {
            let span = file.decl_nodes[decl].span;
            Spec {
                decl,
                pos: span.lo,
                end: span.hi,
                moved: false,
            }
        });
        GenDecl {
            keyword: self.kind(first),
            pos: stmt.span.lo,
            parens: grouped.then(|| (self.lo(first + 1), stmt.span.hi - 1)),
            specs: specs.collect(),
            orphans: Vec::new(),
        }
    }
}
