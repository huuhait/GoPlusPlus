//! Statements.

use std::mem;

use gopp_diag::Code;
use gopp_span::Span;

use super::{Parser, fixed_text};
use crate::ast::*;
use crate::token::TokenKind as T;
use crate::with_stack;

/// A simple statement before it is known to be a statement, the condition
/// of a header, or a range clause.
enum Simple {
    Stmt(StmtKind, Span),
    Range(RangeClause),
}

struct RangeClause {
    lhs: Vec<ExprId>,
    define: bool,
    x: ExprId,
}

#[derive(Default)]
struct Header {
    init: Option<StmtId>,
    cond: Option<ExprId>,
    post: Option<StmtId>,
    range: Option<RangeClause>,
}

impl Parser<'_> {
    pub(super) fn func_body(&mut self) -> Block {
        self.func_depth += 1;
        let body = self.block("");
        self.func_depth -= 1;
        body
    }

    /// `{ statements }`. `context` names what the block follows, for the
    /// error when the `{` is missing.
    pub(super) fn block(&mut self, context: &str) -> Block {
        let lo = self.lo();
        if !self.got(T::LBrace) {
            let rest = format!(", expected {{ after {context}");
            self.unexpected(Code::ExpectedLbraceAfter, &rest);
            self.advance(&[T::Ident, T::RBrace]);
            if self.got(T::RBrace) {
                return Block {
                    stmts: Vec::new(),
                    span: self.span_from(lo),
                };
            }
        }
        let stmts = self.stmt_list();
        self.want(T::RBrace);
        Block {
            stmts,
            span: self.span_from(lo),
        }
    }

    fn stmt_list(&mut self) -> Vec<StmtId> {
        let mut list = Vec::new();
        while !matches!(self.kind(), T::Eof | T::RBrace | T::Case | T::Default) {
            let Some(stmt) = self.stmt_or_none() else {
                break;
            };
            list.push(stmt);
            if !self.got(T::Semi) && !self.at(T::RBrace) {
                self.unexpected(Code::Expected, " at end of statement");
                self.advance(&[T::Semi, T::RBrace, T::Case, T::Default]);
                self.got(T::Semi);
            }
        }
        list
    }

    fn stmt_or_none(&mut self) -> Option<StmtId> {
        if !self.enter(Code::NestingTooDeep, "statement") {
            return None;
        }
        let stmt = with_stack(|| self.stmt_node());
        self.leave();
        stmt
    }

    fn stmt_node(&mut self) -> Option<StmtId> {
        while self.at(T::At) {
            let attribute = self.attribute();
            let message = "attributes are not allowed on statements";
            self.error_at(Code::AttrOnStmt, attribute.span, message);
            self.got(T::Semi);
        }
        let lo = self.lo();
        let kind = match self.kind() {
            T::Ident => {
                let lhs = self.expr_list();
                if let [label] = lhs[..]
                    && let ExprKind::Name(name) = self.exprs[label].kind
                    && self.at(T::Colon)
                {
                    let span = self.exprs[label].span;
                    return self.labeled_stmt(Ident { name, span });
                }
                return Some(self.simple(Some(lhs)));
            }
            T::Const | T::Var | T::Type => {
                let keyword = self.bump().kind;
                StmtKind::Decl(self.decl_group(keyword, lo))
            }
            T::LBrace => StmtKind::Block(self.block("")),
            T::Plus | T::Minus | T::Star | T::Amp | T::Caret | T::Bang | T::Arrow => {
                return Some(self.simple(None));
            }
            T::Int | T::Float | T::Imag | T::Rune | T::String | T::Func | T::LParen => {
                return Some(self.simple(None));
            }
            T::LBrack | T::Struct | T::Map | T::Chan | T::Interface => {
                return Some(self.simple(None));
            }
            T::Hash => return Some(self.simple(None)),
            T::For => return Some(self.for_stmt()),
            T::Switch => return Some(self.switch_stmt()),
            T::Select => return Some(self.select_stmt()),
            T::If => return Some(self.if_stmt()),
            T::Fallthrough => {
                self.bump();
                StmtKind::Branch {
                    kind: BranchKind::Fallthrough,
                    label: None,
                }
            }
            T::Break | T::Continue => {
                let kind = if self.bump().kind == T::Break {
                    BranchKind::Break
                } else {
                    BranchKind::Continue
                };
                let label = self.at(T::Ident).then(|| self.ident());
                StmtKind::Branch { kind, label }
            }
            T::Goto => {
                self.bump();
                StmtKind::Branch {
                    kind: BranchKind::Goto,
                    label: Some(self.ident()),
                }
            }
            T::Go | T::Defer => {
                let keyword = self.bump();
                let mut call = self.primary_expr(None);
                if let ExprKind::Paren(inner) = self.exprs[call].kind {
                    let message = format!(
                        "expression in {} must not be parenthesized",
                        self.token_text(keyword)
                    );
                    self.error_at(Code::CallParenthesized, self.exprs[call].span, message);
                    call = inner;
                }
                if keyword.kind == T::Go {
                    StmtKind::Go(call)
                } else {
                    StmtKind::Defer(call)
                }
            }
            T::Return => {
                self.bump();
                let results = if matches!(self.kind(), T::Semi | T::RBrace) {
                    Vec::new()
                } else {
                    self.expr_list()
                };
                StmtKind::Return(results)
            }
            T::Semi => {
                // The statement is the `;` itself. One that stands for a
                // line end has no width.
                let mut span = self.here();
                if !self.text[span.lo as usize..].starts_with(';') {
                    span.hi = span.lo;
                }
                let id = self.alloc_stmt(StmtKind::Empty, lo);
                self.stmts[id].span = span;
                return Some(id);
            }
            _ => return None,
        };
        Some(self.alloc_stmt(kind, lo))
    }

    /// A simple statement as a statement of its own.
    fn simple(&mut self, lhs: Option<Vec<ExprId>>) -> StmtId {
        let simple = self.simple_stmt(lhs, None);
        self.finish_simple(simple)
    }

    fn finish_simple(&mut self, simple: Simple) -> StmtId {
        let (kind, span) = match simple {
            Simple::Stmt(kind, span) => (kind, span),
            // A range clause is only asked for in a `for` header, which
            // takes it apart itself.
            Simple::Range(range) => (StmtKind::Bad, self.exprs[range.x].span),
        };
        self.stmts.alloc(Stmt { kind, span })
    }

    /// An expression, send, increment, assignment or short declaration, and
    /// after `for` also a range clause. `lhs` is a left side that is
    /// already parsed.
    fn simple_stmt(&mut self, lhs: Option<Vec<ExprId>>, keyword: Option<T>) -> Simple {
        if keyword == Some(T::For) && self.at(T::Range) {
            return Simple::Range(self.range_clause(Vec::new(), false));
        }
        let lhs = match lhs {
            Some(lhs) => lhs,
            None => self.expr_list(),
        };
        let first = lhs[0];
        let lo = self.expr_lo(first);

        if lhs.len() == 1 && !matches!(self.kind(), T::Eq | T::ColonEq) {
            let kind = if let Some(op) = assign_op(self.kind()) {
                self.bump();
                let rhs = vec![self.expr()];
                let op = AssignOp::Op(op);
                StmtKind::Assign { op, lhs, rhs }
            } else if matches!(self.kind(), T::PlusPlus | T::MinusMinus) {
                let inc = self.bump().kind == T::PlusPlus;
                StmtKind::IncDec { x: first, inc }
            } else if self.got(T::Arrow) {
                let value = self.expr();
                StmtKind::Send { chan: first, value }
            } else {
                StmtKind::Expr(first)
            };
            return Simple::Stmt(kind, self.span_from(lo));
        }

        if !matches!(self.kind(), T::Eq | T::ColonEq) {
            self.expected(":= or = or comma");
            self.advance(&[T::Semi, T::RBrace]);
            return Simple::Stmt(StmtKind::Expr(first), self.exprs[first].span);
        }
        let define = self.bump().kind == T::ColonEq;
        if keyword == Some(T::For) && self.at(T::Range) {
            return Simple::Range(self.range_clause(lhs, define));
        }
        let rhs = self.expr_list();

        // `switch v := x.(type)`: the guard takes the name.
        if define
            && keyword == Some(T::Switch)
            && let ([left], [right]) = (&lhs[..], &rhs[..])
            && let ExprKind::Name(name) = self.exprs[*left].kind
            && let ExprKind::TypeSwitchGuard { x, .. } = self.exprs[*right].kind
        {
            let name = Ident {
                name,
                span: self.exprs[*left].span,
            };
            let guard = &mut self.exprs[*right];
            guard.kind = ExprKind::TypeSwitchGuard { lhs: Some(name), x };
            guard.span.lo = lo;
            return Simple::Stmt(StmtKind::Expr(*right), guard.span);
        }
        let op = if define {
            AssignOp::Define
        } else {
            AssignOp::Assign
        };
        Simple::Stmt(StmtKind::Assign { op, lhs, rhs }, self.span_from(lo))
    }

    /// `range x`, at the `range`. RNG-1a: an alias variable `&v` needs
    /// `:=`, and the key or index cannot be one.
    fn range_clause(&mut self, lhs: Vec<ExprId>, define: bool) -> RangeClause {
        self.bump();
        let x = self.expr();
        for (index, &var) in lhs.iter().enumerate() {
            let is_alias = matches!(
                self.exprs[var].kind,
                ExprKind::Unary {
                    op: UnaryOp::Addr,
                    ..
                }
            );
            if !is_alias {
                continue;
            }
            let span = self.exprs[var].span;
            if !define {
                let message = "an alias variable (&) in a range clause needs :=";
                self.error_at(Code::RangeAliasAssign, span, message);
            } else if index == 0 && lhs.len() > 1 {
                let message = "the key or index of a range clause cannot be an alias variable (&)";
                self.error_at(Code::RangeAliasKey, span, message);
            }
        }
        RangeClause { lhs, define, x }
    }

    fn labeled_stmt(&mut self, label: Ident) -> Option<StmtId> {
        let lo = label.span.lo;
        let colon = self.here();
        self.want(T::Colon);
        let stmt = if self.at(T::RBrace) {
            // A label before `}` labels an empty statement.
            Some(self.alloc_stmt(StmtKind::Empty, self.lo()))
        } else {
            self.stmt_or_none()
        };
        let Some(stmt) = stmt else {
            let message = "syntax error: missing statement after label";
            self.error_at(Code::Expected, colon, message);
            return None;
        };
        let id = self.alloc_stmt(StmtKind::Labeled { label, stmt }, lo);
        // The empty statement before a `}` lies past the `:`.
        self.stmts[id].span.hi = self.stmts[stmt].span.hi;
        Some(id)
    }

    /// The clauses between `if`, `for` or `switch` and the block.
    fn header(&mut self, keyword: T) -> Header {
        self.want(keyword);
        if self.at(T::LBrace) {
            let mut header = Header::default();
            if keyword == T::If {
                let message = "missing condition in if statement";
                self.syntax_error(Code::MissingIfCondition, message);
                header.cond = Some(self.bad_expr());
            }
            return header;
        }
        let outer = mem::replace(&mut self.expr_level, -1);
        let header = self.header_clauses(keyword);
        self.expr_level = outer;
        header
    }

    fn header_clauses(&mut self, keyword: T) -> Header {
        let name = fixed_text(keyword);
        let mut header = Header::default();
        let mut init = None;
        if !self.at(T::Semi) {
            if self.at(T::Var) {
                let message = format!("var declaration not allowed in {name} initializer");
                self.syntax_error(Code::VarInIfInit, &message);
                self.bump();
            }
            match self.simple_stmt(None, Some(keyword)) {
                Simple::Range(range) => {
                    header.range = Some(range);
                    return header;
                }
                simple => init = Some(simple),
            }
        }

        let mut condition = None;
        // The `;` after the first clause: where it is and how it reads.
        let mut semi = None;
        if self.at(T::LBrace) {
            condition = init.take();
        } else {
            if self.at(T::Semi) {
                semi = Some((self.here(), self.describe()));
                self.bump();
            } else {
                // Asking for `{` gives a better message than asking for `;`.
                self.want(T::LBrace);
                if !self.at(T::LBrace) {
                    self.advance(&[T::LBrace, T::RBrace]);
                }
            }
            if keyword != T::For {
                if !self.at(T::LBrace) {
                    condition = Some(self.simple_stmt(None, Some(keyword)));
                }
            } else if self.at(T::LBrace) {
                self.expected("for loop condition");
            } else {
                if !self.at(T::Semi) {
                    condition = Some(self.simple_stmt(None, None));
                }
                self.want(T::Semi);
                if !self.at(T::LBrace) {
                    let post = self.simple_stmt(None, None);
                    if let Simple::Stmt(StmtKind::Assign { op, .. }, span) = &post
                        && *op == AssignOp::Define
                    {
                        let message = "syntax error: cannot declare in post statement of for loop";
                        self.error_at(Code::Expected, *span, message);
                    }
                    header.post = Some(self.finish_simple(post));
                }
            }
        }

        header.init = init.map(|init| self.finish_simple(init));
        header.cond = match condition {
            Some(Simple::Stmt(StmtKind::Expr(cond), _)) => Some(cond),
            Some(Simple::Stmt(kind, span)) => {
                // Often `=` written for `==`.
                let what = match kind {
                    StmtKind::Assign {
                        op: AssignOp::Assign,
                        ..
                    } => "assignment ",
                    _ => "",
                };
                let text = &self.text[span.lo as usize..span.hi as usize];
                let message = format!("syntax error: cannot use {what}{text} as value");
                self.error_at(Code::Expected, span, message);
                Some(self.exprs.alloc(Expr {
                    kind: ExprKind::Bad,
                    span,
                }))
            }
            Some(Simple::Range(_)) => None,
            None => match semi {
                Some((span, found)) if keyword == T::If => {
                    if found == "semicolon" {
                        let message = "syntax error: missing condition in if statement";
                        self.error_at(Code::MissingIfCondition, span, message);
                    } else {
                        let message = format!(
                            "syntax error: unexpected {found}, expected {{ after {name} clause"
                        );
                        self.error_at(Code::NewlineAfterIfClause, span, message);
                    }
                    Some(self.exprs.alloc(Expr {
                        kind: ExprKind::Bad,
                        span,
                    }))
                }
                _ => None,
            },
        };
        header
    }

    fn for_stmt(&mut self) -> StmtId {
        let lo = self.lo();
        let header = self.header(T::For);
        let body = self.block("for clause");
        let kind = match header.range {
            Some(RangeClause { lhs, define, x }) => StmtKind::Range {
                lhs,
                define,
                x,
                body,
            },
            None => StmtKind::For {
                init: header.init,
                cond: header.cond,
                post: header.post,
                body,
            },
        };
        self.alloc_stmt(kind, lo)
    }

    /// An `if` with its whole `else if` chain. The chain is read in a loop,
    /// so a long one costs no stack.
    fn if_stmt(&mut self) -> StmtId {
        let mut arms = Vec::new();
        let mut otherwise = None;
        loop {
            let lo = self.lo();
            let header = self.header(T::If);
            let cond = match header.cond {
                Some(cond) => cond,
                None => self.bad_expr(),
            };
            let then = self.block("if clause");
            arms.push((lo, header.init, cond, then));
            if !self.got(T::Else) {
                break;
            }
            match self.kind() {
                T::If => continue,
                T::LBrace => {
                    let lo = self.lo();
                    let block = self.block("");
                    otherwise = Some(self.alloc_stmt(StmtKind::Block(block), lo));
                }
                _ => {
                    let message = "else must be followed by if or statement block";
                    self.syntax_error(Code::Expected, message);
                    self.advance(&[T::Ident, T::RBrace]);
                }
            }
            break;
        }
        for (lo, init, cond, then) in arms.into_iter().rev() {
            let kind = StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            };
            otherwise = Some(self.alloc_stmt(kind, lo));
        }
        // The loop above pushes an arm before it can end.
        #[allow(clippy::expect_used)]
        otherwise.expect("an if statement has an arm")
    }

    fn switch_stmt(&mut self) -> StmtId {
        let lo = self.lo();
        let header = self.header(T::Switch);
        if !self.got(T::LBrace) {
            self.syntax_error(Code::ExpectedLbraceAfter, "missing { after switch clause");
            self.advance(&[T::Case, T::Default, T::RBrace]);
        }
        let mut clauses = Vec::new();
        while !matches!(self.kind(), T::Eof | T::RBrace) {
            let lo = self.lo();
            let cases = match self.kind() {
                T::Case => {
                    self.bump();
                    Some(self.expr_list())
                }
                T::Default => {
                    self.bump();
                    None
                }
                _ => {
                    self.expected("case or default or }");
                    self.advance(&[T::Colon, T::Case, T::Default, T::RBrace]);
                    Some(Vec::new())
                }
            };
            let (body, span) = self.clause_body(lo);
            clauses.push(CaseClause { cases, body, span });
        }
        self.want(T::RBrace);
        let kind = StmtKind::Switch {
            init: header.init,
            tag: header.cond,
            clauses,
        };
        self.alloc_stmt(kind, lo)
    }

    /// The `:` and the statements of a clause that started at `lo`. The
    /// clause ends with its last statement, or with the `:`.
    fn clause_body(&mut self, lo: u32) -> (Vec<StmtId>, Span) {
        self.want(T::Colon);
        let mut span = self.span_from(lo);
        let body = self.stmt_list();
        if let Some(&last) = body.last() {
            span.hi = self.stmts[last].span.hi;
        }
        (body, span)
    }

    fn select_stmt(&mut self) -> StmtId {
        let lo = self.lo();
        self.want(T::Select);
        if !self.got(T::LBrace) {
            self.syntax_error(Code::ExpectedLbraceAfter, "missing { after select clause");
            self.advance(&[T::Case, T::Default, T::RBrace]);
        }
        let mut clauses = Vec::new();
        while !matches!(self.kind(), T::Eof | T::RBrace) {
            let lo = self.lo();
            let comm = match self.kind() {
                T::Case => {
                    self.bump();
                    // A send or a receive; anything else is the checker's to refuse.
                    Some(self.simple(None))
                }
                T::Default => {
                    self.bump();
                    None
                }
                _ => {
                    self.expected("case or default or }");
                    self.advance(&[T::Colon, T::Case, T::Default, T::RBrace]);
                    None
                }
            };
            let (body, span) = self.clause_body(lo);
            clauses.push(CommClause { comm, body, span });
        }
        self.want(T::RBrace);
        self.alloc_stmt(StmtKind::Select { clauses }, lo)
    }
}

/// The operator of an assignment token such as `+=`.
fn assign_op(kind: T) -> Option<BinaryOp> {
    Some(match kind {
        T::PlusEq => BinaryOp::Add,
        T::MinusEq => BinaryOp::Sub,
        T::PipeEq => BinaryOp::Or,
        T::CaretEq => BinaryOp::Xor,
        T::StarEq => BinaryOp::Mul,
        T::SlashEq => BinaryOp::Div,
        T::PercentEq => BinaryOp::Rem,
        T::ShlEq => BinaryOp::Shl,
        T::ShrEq => BinaryOp::Shr,
        T::AmpEq => BinaryOp::And,
        T::AmpCaretEq => BinaryOp::AndNot,
        _ => return None,
    })
}
