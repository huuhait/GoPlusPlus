//! Statements.

use super::exprs::{Elem, binary_token};
use super::printer::{Printer, Ws};
use crate::ast::*;
use crate::token::TokenKind as T;
use crate::with_stack;

fn assign_token(op: AssignOp) -> T {
    match op {
        AssignOp::Assign => T::Eq,
        AssignOp::Define => T::ColonEq,
        AssignOp::Op(op) => match binary_token(op) {
            T::Plus => T::PlusEq,
            T::Minus => T::MinusEq,
            T::Pipe => T::PipeEq,
            T::Caret => T::CaretEq,
            T::Star => T::StarEq,
            T::Slash => T::SlashEq,
            T::Percent => T::PercentEq,
            T::Shl => T::ShlEq,
            T::Shr => T::ShrEq,
            T::Amp => T::AmpEq,
            _ => T::AmpCaretEq,
        },
    }
}

#[derive(Clone, Copy)]
enum Clauses<'a> {
    Case(&'a [CaseClause]),
    Comm(&'a [CommClause]),
}

impl<'a> Printer<'a> {
    pub(super) fn stmt(&mut self, id: StmtId, next_is_rbrace: bool) {
        if !self.aborted {
            with_stack(|| self.stmt_node(id, next_is_rbrace));
        }
    }

    /// The statements of a block, each on a line of its own. Blank lines
    /// between them are kept, one at most. `indent` is off for the bodies
    /// of `switch` and `select`.
    fn stmt_list(&mut self, list: &[StmtId], indent: bool, next_is_rbrace: bool) {
        let cx = self.cx;
        if indent {
            self.ws(Ws::Indent);
        }
        let slot = self.new_line_slot();
        let mut printed = 0;
        for &id in list {
            if self.aborted {
                break;
            }
            let stmt = &cx.file.stmts[id];
            if matches!(stmt.kind, StmtKind::Empty) {
                continue;
            }
            if !self.output.is_empty() {
                // The columns of a statement over several lines end with it.
                let new_section = printed == 0 || self.lines_from(slot) > 0;
                self.linebreak(cx.line(stmt.span.lo), 1, Ws::Ignore, new_section);
            }
            self.record_line(slot);
            self.stmt(id, next_is_rbrace && printed == list.len() - 1);
            // A label is on a line of its own, before its statement.
            let mut inner = stmt;
            while let StmtKind::Labeled { stmt, .. } = inner.kind {
                self.add_to_line_slot(slot, 1);
                inner = &cx.file.stmts[stmt];
            }
            printed += 1;
        }
        self.drop_line_slot(slot);
        if indent {
            self.ws(Ws::Unindent);
        }
    }

    pub(super) fn block(&mut self, block: &Block, indent: bool) {
        let rbrace = block.span.hi - 1;
        self.set_pos(block.span.lo);
        self.tok(T::LBrace);
        self.stmt_list(&block.stmts, indent, true);
        self.linebreak(self.cx.line(rbrace), 1, Ws::Ignore, true);
        self.set_pos(rbrace);
        self.tok(T::RBrace);
    }

    /// The clauses of a `switch` or `select` between their braces.
    fn clauses(&mut self, lbrace: u32, clauses: Clauses<'a>, rbrace: u32) {
        let cx = self.cx;
        self.set_pos(lbrace);
        self.tok(T::LBrace);
        let count = match clauses {
            Clauses::Case(list) => list.len(),
            Clauses::Comm(list) => list.len(),
        };
        for index in 0..count {
            if self.aborted {
                break;
            }
            let (span, last) = match clauses {
                Clauses::Case(list) => (list[index].span, index == count - 1),
                Clauses::Comm(list) => (list[index].span, index == count - 1),
            };
            self.linebreak(cx.line(span.lo), 1, Ws::Ignore, true);
            self.set_pos(span.lo);
            let after_keyword = cx.find(span.lo + 1, T::Colon);
            let (colon, body) = match clauses {
                Clauses::Case(list) => {
                    let clause = &list[index];
                    let colon = match clause.cases.as_deref() {
                        Some(cases @ [.., last]) => {
                            let colon = cx.find(self.expr_end(*last), T::Colon);
                            self.tok(T::Case);
                            self.ws(Ws::Blank);
                            self.expr_list(Some(span.lo), cases, 1, 0, Some(colon));
                            colon
                        }
                        Some([]) => {
                            self.tok(T::Case);
                            after_keyword
                        }
                        None => {
                            self.tok(T::Default);
                            after_keyword
                        }
                    };
                    (colon, &clause.body)
                }
                Clauses::Comm(list) => {
                    let clause = &list[index];
                    let colon = match clause.comm {
                        Some(comm) => {
                            self.tok(T::Case);
                            self.ws(Ws::Blank);
                            self.stmt(comm, false);
                            cx.find(cx.file.stmts[comm].span.hi, T::Colon)
                        }
                        None => {
                            self.tok(T::Default);
                            after_keyword
                        }
                    };
                    (colon, &clause.body)
                }
            };
            self.set_pos(colon);
            self.tok(T::Colon);
            self.stmt_list(body, true, last);
        }
        self.linebreak(cx.line(rbrace), 1, Ws::Ignore, true);
        self.set_pos(rbrace);
        self.tok(T::RBrace);
    }

    /// The header of `if`, `for` and `switch`: `init; cond; post`.
    fn control_clause(
        &mut self,
        is_for: bool,
        init: Option<StmtId>,
        cond: Option<ExprId>,
        post: Option<StmtId>,
    ) {
        self.ws(Ws::Blank);
        let mut needs_blank = false;
        if init.is_none() && post.is_none() {
            if let Some(cond) = cond {
                self.expr(self.strip_parens(cond));
                needs_blank = true;
            }
        } else {
            if let Some(init) = init {
                self.stmt(init, false);
            }
            self.tok(T::Semi);
            self.ws(Ws::Blank);
            if let Some(cond) = cond {
                self.expr(self.strip_parens(cond));
                needs_blank = true;
            }
            if is_for {
                self.tok(T::Semi);
                self.ws(Ws::Blank);
                needs_blank = false;
                if let Some(post) = post {
                    self.stmt(post, false);
                    needs_blank = true;
                }
            }
        }
        if needs_blank {
            self.ws(Ws::Blank);
        }
    }

    /// Whether the results of a `return` read better indented as a whole:
    /// when more than one is over several lines, or one does not start on
    /// the line where the one before it ends.
    fn indent_list(&self, list: &[ExprId]) -> bool {
        let cx = self.cx;
        let (Some(&first), Some(&last), true) = (list.first(), list.last(), list.len() >= 2) else {
            return false;
        };
        let begin = cx.line(self.expr_pos(first));
        if begin >= cx.line(self.expr_end(last)) {
            return false;
        }
        let mut multi_line = 0;
        let mut line = begin;
        for &x in list {
            let (x_begin, x_end) = (cx.line(self.expr_pos(x)), cx.line(self.expr_end(x)));
            if line < x_begin {
                return true;
            }
            multi_line += i32::from(x_begin < x_end);
            line = x_end;
        }
        multi_line > 1
    }

    fn stmt_node(&mut self, id: StmtId, next_is_rbrace: bool) {
        let cx = self.cx;
        let stmt = &cx.file.stmts[id];
        let span = stmt.span;
        let after = |end: u32| cx.lo(cx.token_at(end));
        self.set_pos(span.lo);
        match &stmt.kind {
            StmtKind::Bad => self.literal(T::String, cx.source(span.lo, span.hi)),
            StmtKind::Empty => {}
            &StmtKind::Labeled { label, stmt: inner } => {
                // The unindent moves before the line end that is pending,
                // which puts the label one level out.
                self.ws(Ws::Unindent);
                self.name(label);
                self.set_pos(cx.find(label.span.hi, T::Colon));
                self.tok(T::Colon);
                self.ws(Ws::Indent);
                let inner_stmt = &cx.file.stmts[inner];
                if matches!(inner_stmt.kind, StmtKind::Empty) {
                    if !next_is_rbrace {
                        self.ws(Ws::Newline);
                        self.set_pos(inner_stmt.span.lo);
                        self.tok(T::Semi);
                        return;
                    }
                } else {
                    self.linebreak(cx.line(inner_stmt.span.lo), 1, Ws::Ignore, true);
                }
                self.stmt(inner, next_is_rbrace);
            }
            StmtKind::Block(block) => self.block(block, true),
            &StmtKind::Expr(x) => self.expr0(x, 1),
            &StmtKind::Send { chan, value } => {
                self.expr0(chan, 1);
                self.ws(Ws::Blank);
                self.set_pos(after(self.expr_end(chan)));
                self.tok(T::Arrow);
                self.ws(Ws::Blank);
                self.expr0(value, 1);
            }
            StmtKind::Decl(decls) => {
                let decl = cx.statement_decl(stmt, decls);
                self.gen_decl(&decl, &[]);
            }
            StmtKind::Assign { op, lhs, rhs } => {
                let depth = if lhs.len() > 1 && rhs.len() > 1 { 2 } else { 1 };
                let operator = match lhs.last() {
                    Some(&last) => after(self.expr_end(last)),
                    None => span.lo,
                };
                self.expr_list(Some(span.lo), lhs, depth, 0, Some(operator));
                self.ws(Ws::Blank);
                self.set_pos(operator);
                self.tok(assign_token(*op));
                self.ws(Ws::Blank);
                self.expr_list(Some(operator), rhs, depth, 0, None);
            }
            &StmtKind::IncDec { x, inc } => {
                self.expr0(x, 2);
                self.set_pos(after(self.expr_end(x)));
                self.tok(if inc { T::PlusPlus } else { T::MinusMinus });
            }
            &StmtKind::Branch { kind, label } => {
                self.tok(match kind {
                    BranchKind::Break => T::Break,
                    BranchKind::Continue => T::Continue,
                    BranchKind::Goto => T::Goto,
                    BranchKind::Fallthrough => T::Fallthrough,
                });
                if let Some(label) = label {
                    self.ws(Ws::Blank);
                    self.name(label);
                }
            }
            &StmtKind::Go(call) => {
                self.tok(T::Go);
                self.ws(Ws::Blank);
                self.expr(call);
            }
            &StmtKind::Defer(call) => {
                self.tok(T::Defer);
                self.ws(Ws::Blank);
                self.expr(call);
            }
            StmtKind::Return(results) => {
                self.tok(T::Return);
                if !results.is_empty() {
                    self.ws(Ws::Blank);
                    // No position before the results: a line end there
                    // would end the statement.
                    if self.indent_list(results) {
                        self.ws(Ws::Indent);
                        self.expr_list(None, results, 1, super::exprs::NO_INDENT, None);
                        self.ws(Ws::Unindent);
                    } else {
                        self.expr_list(None, results, 1, 0, None);
                    }
                }
            }
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => {
                self.tok(T::If);
                self.control_clause(false, *init, Some(*cond), None);
                self.block(then, true);
                if let Some(otherwise) = *otherwise {
                    self.ws(Ws::Blank);
                    self.tok(T::Else);
                    self.ws(Ws::Blank);
                    self.stmt(otherwise, next_is_rbrace);
                }
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                self.tok(T::For);
                self.control_clause(true, *init, *cond, *post);
                self.block(body, true);
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                self.tok(T::For);
                self.ws(Ws::Blank);
                if let Some(&last) = lhs.last() {
                    for (index, &var) in lhs.iter().enumerate() {
                        if index > 0 {
                            self.set_pos(self.expr_pos(var));
                            self.tok(T::Comma);
                            self.ws(Ws::Blank);
                        }
                        self.expr(var);
                    }
                    self.ws(Ws::Blank);
                    self.set_pos(after(self.expr_end(last)));
                    self.tok(if *define { T::ColonEq } else { T::Eq });
                    self.ws(Ws::Blank);
                }
                self.tok(T::Range);
                self.ws(Ws::Blank);
                self.expr(self.strip_parens(*x));
                self.ws(Ws::Blank);
                self.block(body, true);
            }
            StmtKind::Switch { init, tag, clauses } => {
                self.tok(T::Switch);
                let guard = tag.and_then(|tag| match cx.file.exprs[tag].kind {
                    ExprKind::TypeSwitchGuard { lhs, .. } => Some((tag, lhs)),
                    _ => None,
                });
                match guard {
                    Some((guard, lhs)) => {
                        if let Some(init) = *init {
                            self.ws(Ws::Blank);
                            self.stmt(init, false);
                            self.tok(T::Semi);
                        }
                        self.ws(Ws::Blank);
                        match lhs {
                            Some(lhs) => {
                                let define = after(lhs.span.hi);
                                let name = [Elem::Ident(lhs)];
                                self.set_pos(lhs.span.lo);
                                self.elem_list(Some(lhs.span.lo), &name, 1, 0, Some(define));
                                self.ws(Ws::Blank);
                                self.set_pos(define);
                                self.tok(T::ColonEq);
                                self.ws(Ws::Blank);
                                self.elem_list(Some(define), &[Elem::Expr(guard)], 1, 0, None);
                            }
                            None => {
                                self.set_pos(self.expr_pos(guard));
                                self.expr0(guard, 1);
                            }
                        }
                        self.ws(Ws::Blank);
                    }
                    None => self.control_clause(false, *init, *tag, None),
                }
                let header_end = match (*tag, *init) {
                    (Some(tag), _) => self.expr_end(tag),
                    (None, Some(init)) => cx.file.stmts[init].span.hi,
                    (None, None) => span.lo + 1,
                };
                let lbrace = cx.lo(cx.closing(header_end));
                self.clauses(lbrace, Clauses::Case(clauses), span.hi - 1);
            }
            StmtKind::Select { clauses } => {
                self.tok(T::Select);
                self.ws(Ws::Blank);
                let lbrace = cx.find(span.lo + 1, T::LBrace);
                let rbrace = span.hi - 1;
                if clauses.is_empty() && !self.comment_before(cx.pos(rbrace)) {
                    self.set_pos(lbrace);
                    self.tok(T::LBrace);
                    self.set_pos(rbrace);
                    self.tok(T::RBrace);
                } else {
                    self.clauses(lbrace, Clauses::Comm(clauses), rbrace);
                }
            }
        }
    }
}
