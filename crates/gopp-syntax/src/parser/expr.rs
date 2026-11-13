//! Expressions.

use gopp_diag::Code;
use gopp_span::Span;

use super::Parser;
use crate::ast::*;
use crate::token::TokenKind as T;
use crate::with_stack;

impl Parser<'_> {
    pub(super) fn expr(&mut self) -> ExprId {
        self.binary_expr(None, 0)
    }

    pub(super) fn expr_list(&mut self) -> Vec<ExprId> {
        let mut list = vec![self.expr()];
        while self.got(T::Comma) {
            list.push(self.expr());
        }
        list
    }

    /// Operators that bind tighter than `precedence`, applied to `x` or to
    /// the unary expression that comes next.
    pub(super) fn binary_expr(&mut self, x: Option<ExprId>, precedence: u8) -> ExprId {
        let mut x = match x {
            Some(x) => x,
            None => self.unary_expr(),
        };
        while let Some((op, op_precedence)) = binary_op(self.kind())
            && op_precedence > precedence
        {
            self.bump();
            let y = self.binary_expr(None, op_precedence);
            x = self.alloc_expr(ExprKind::Binary { op, x, y }, self.expr_lo(x));
        }
        x
    }

    fn unary_expr(&mut self) -> ExprId {
        if !self.enter(Code::ExpressionTooDeeplyNested, "expression") {
            return self.bad_expr();
        }
        let x = with_stack(|| self.unary_node());
        self.leave();
        x
    }

    fn unary_node(&mut self) -> ExprId {
        let lo = self.lo();
        let op = match self.kind() {
            T::Star => UnaryOp::Star,
            T::Plus => UnaryOp::Plus,
            T::Minus => UnaryOp::Neg,
            T::Bang => UnaryOp::Not,
            T::Caret => UnaryOp::Xor,
            T::Tilde => UnaryOp::Tilde,
            T::Amp => UnaryOp::Addr,
            T::Arrow => return self.receive_or_chan_type(),
            _ => return self.primary_expr(None),
        };
        self.bump();
        let x = self.unary_expr();
        self.alloc_expr(ExprKind::Unary { op, x }, lo)
    }

    /// `<-x`, or a channel type `<-chan E`, which is first read as the
    /// receive of `chan E`.
    fn receive_or_chan_type(&mut self) -> ExprId {
        let lo = self.lo();
        self.bump();
        let x = self.unary_expr();
        if !matches!(self.exprs[x].kind, ExprKind::ChanType { .. }) {
            let op = UnaryOp::Recv;
            return self.alloc_expr(ExprKind::Unary { op, x }, lo);
        }
        // `<-chan E` is `(<-chan E)`, and `<-chan<- E` is `<-chan (<-E)`:
        // the arrow moves into each channel type that was read as send-only.
        let mut dir = ChanDir::Send;
        let mut inner = x;
        while dir == ChanDir::Send {
            let ExprKind::ChanType { dir: found, elem } = self.exprs[inner].kind else {
                break;
            };
            dir = found;
            if dir == ChanDir::Recv {
                self.syntax_error(Code::ExpectedFound, "unexpected <-, expected chan");
            }
            self.exprs[inner].kind = ExprKind::ChanType {
                dir: ChanDir::Recv,
                elem,
            };
            inner = elem;
        }
        if dir == ChanDir::Send {
            let span = self.exprs[inner].span;
            let text = &self.text[span.lo as usize..span.hi as usize];
            let message = format!("syntax error: unexpected {text}, expected chan");
            self.error_at(Code::ExpectedFound, span, message);
        }
        self.exprs[x].span.lo = lo;
        x
    }

    /// An operand and what follows it: selectors, indices, calls, composite
    /// literal bodies. `x` is an operand that is already parsed.
    pub(super) fn primary_expr(&mut self, x: Option<ExprId>) -> ExprId {
        let mut x = match x {
            Some(x) => x,
            None => self.operand(),
        };
        loop {
            let lo = self.expr_lo(x);
            match self.kind() {
                T::Dot => {
                    self.bump();
                    match self.kind() {
                        T::Ident => {
                            let sel = self.ident();
                            x = self.alloc_expr(ExprKind::Selector { x, sel }, lo);
                        }
                        T::LParen => {
                            self.bump();
                            let kind = if self.got(T::Type) {
                                ExprKind::TypeSwitchGuard { lhs: None, x }
                            } else {
                                let ty = self.type_();
                                ExprKind::Assert { x, ty }
                            };
                            self.want(T::RParen);
                            x = self.alloc_expr(kind, lo);
                        }
                        _ => {
                            self.expected("name or (");
                            self.advance(&[T::Semi, T::RParen]);
                        }
                    }
                }
                T::LBrack => x = self.index_or_slice(x),
                T::LParen => {
                    self.bump();
                    x = self.call(x, false);
                }
                // LEX-5: after an operand, `!` before `(` is the unwrapping mark.
                T::Bang if self.peek(1) == T::LParen => {
                    self.bump();
                    self.bump();
                    x = self.call(x, true);
                }
                T::LBrace => {
                    if !self.may_be_literal_type(x) {
                        break;
                    }
                    if matches!(self.exprs[x].kind, ExprKind::Paren(_)) {
                        let message = "cannot parenthesize type in composite literal";
                        self.syntax_error(Code::ParenComplitType, message);
                    }
                    let elems = self.literal_value();
                    x = self.alloc_expr(ExprKind::CompositeLit { ty: Some(x), elems }, lo);
                }
                _ => break,
            }
        }
        x
    }

    fn call(&mut self, fun: ExprId, unwrap: bool) -> ExprId {
        let (args, spread) = self.arguments();
        let kind = ExprKind::Call {
            fun,
            args,
            spread: spread.is_some(),
            unwrap,
        };
        self.alloc_expr(kind, self.expr_lo(fun))
    }

    fn operand(&mut self) -> ExprId {
        let lo = self.lo();
        match self.kind() {
            T::Ident => {
                let name = self.ident();
                self.name_expr(name)
            }
            T::LParen => {
                self.bump();
                self.expr_level += 1;
                let x = self.expr();
                self.expr_level -= 1;
                self.want(T::RParen);
                self.alloc_expr(ExprKind::Paren(x), lo)
            }
            T::Func => {
                self.bump();
                let (_, ty) = self.func_type("function type", lo);
                if !self.at(T::LBrace) {
                    return ty;
                }
                self.expr_level += 1;
                let body = self.func_body();
                self.expr_level -= 1;
                self.alloc_expr(ExprKind::FuncLit { ty, body }, lo)
            }
            T::LBrack | T::Chan | T::Map | T::Struct | T::Interface => self.type_(),
            T::Hash => self.macro_call(),
            _ => self.literal().unwrap_or_else(|| {
                let bad = self.bad_expr();
                self.expected("expression");
                self.advance(&[T::RParen, T::RBrack, T::RBrace]);
                bad
            }),
        }
    }

    /// A basic literal, if the current token is one.
    pub(super) fn literal(&mut self) -> Option<ExprId> {
        let kind = match self.kind() {
            T::Int => LitKind::Int,
            T::Float => LitKind::Float,
            T::Imag => LitKind::Imag,
            T::Rune => LitKind::Rune,
            T::String => LitKind::String,
            _ => return None,
        };
        let token = self.bump();
        let bad = token.bad;
        Some(self.alloc_expr(ExprKind::BasicLit { kind, bad }, token.lo))
    }

    /// `#Name(args)`, at the `#`.
    pub(super) fn macro_call(&mut self) -> ExprId {
        let lo = self.lo();
        self.bump();
        let path = self.macro_ref();
        let args = if self.got(T::LParen) {
            self.macro_arguments()
        } else {
            self.expected("(");
            Vec::new()
        };
        self.alloc_expr(ExprKind::MacroCall { path, args }, lo)
    }

    /// Call arguments after the `(`, and the `...` that spreads the last one.
    fn arguments(&mut self) -> (Vec<ExprId>, Option<Span>) {
        let mut args = Vec::new();
        let mut spread = None;
        self.expr_level += 1;
        self.list("argument list", T::Comma, T::RParen, |p| {
            args.push(p.expr());
            let here = p.here();
            spread = p.got(T::Ellipsis).then_some(here);
            spread.is_some()
        });
        self.expr_level -= 1;
        (args, spread)
    }

    /// The arguments of a macro call or an attribute after the `(`: a list
    /// of expressions, none of them spread.
    pub(super) fn macro_arguments(&mut self) -> Vec<ExprId> {
        let (args, spread) = self.arguments();
        if let Some(span) = spread {
            let message = "syntax error: unexpected ... in the arguments of a macro";
            self.error_at(Code::Expected, span, message);
        }
        args
    }

    /// `x[i]`, `x[A, B]` or a slice expression, at the `[`.
    fn index_or_slice(&mut self, x: ExprId) -> ExprId {
        let start = self.expr_lo(x);
        self.bump();
        let mut lo = None;
        if !self.at(T::Colon) {
            let (indices, comma) = if self.at(T::RBrack) {
                self.expected("operand");
                (vec![self.bad_expr()], false)
            } else {
                self.type_list(false)
            };
            if comma || self.at(T::RBrack) {
                self.want(T::RBrack);
                return self.alloc_expr(ExprKind::Index { x, indices }, start);
            }
            lo = indices.first().copied();
        }
        let first_colon = self.here();
        if !self.got(T::Colon) {
            self.expected("comma, : or ]");
            self.advance(&[T::Comma, T::Colon, T::RBrack]);
        }
        self.expr_level += 1;
        let (mut hi, mut max, mut full) = (None, None, false);
        if !self.at(T::Colon) && !self.at(T::RBrack) {
            hi = Some(self.expr());
        }
        if self.at(T::Colon) {
            full = true;
            if hi.is_none() {
                let message = "middle index required in 3-index slice";
                self.error_plain(Code::Slice3MiddleIndex, first_colon, message);
            }
            let second_colon = self.here();
            self.bump();
            if self.at(T::RBrack) {
                let message = "final index required in 3-index slice";
                self.error_plain(Code::Slice3FinalIndex, second_colon, message);
            } else {
                max = Some(self.expr());
            }
        }
        self.expr_level -= 1;
        self.want(T::RBrack);
        let kind = ExprKind::Slice {
            x,
            lo,
            hi,
            max,
            full,
        };
        self.alloc_expr(kind, start)
    }

    /// Whether a `{` after `x` starts a composite literal of type `x`. In
    /// the header of `if`, `for` and `switch` a name is not taken as a
    /// type: the `{` opens the block.
    fn may_be_literal_type(&self, x: ExprId) -> bool {
        let unparenthesized = match self.exprs[x].kind {
            ExprKind::Paren(inner) => inner,
            _ => x,
        };
        match self.exprs[unparenthesized].kind {
            ExprKind::Name(_) | ExprKind::Selector { .. } => self.expr_level >= 0,
            ExprKind::Index { .. } => self.expr_level >= 0 && !self.is_value(unparenthesized),
            ExprKind::ArrayType { .. }
            | ExprKind::SliceType { .. }
            | ExprKind::StructType { .. }
            | ExprKind::MapType { .. } => true,
            _ => false,
        }
    }

    /// Whether `x` can only be a value, never a type.
    fn is_value(&self, mut x: ExprId) -> bool {
        loop {
            match &self.exprs[x].kind {
                ExprKind::BasicLit { .. }
                | ExprKind::CompositeLit { .. }
                | ExprKind::FuncLit { .. }
                | ExprKind::Slice { .. }
                | ExprKind::Assert { .. }
                | ExprKind::TypeSwitchGuard { .. }
                | ExprKind::Call { .. }
                | ExprKind::MacroCall { .. }
                | ExprKind::Binary { .. } => return true,
                ExprKind::Unary { op, .. } => return *op != UnaryOp::Star,
                ExprKind::Paren(inner) => x = *inner,
                ExprKind::Index { x: base, indices } => {
                    if let [index] = indices[..]
                        && self.is_value(index)
                    {
                        return true;
                    }
                    x = *base;
                }
                _ => return false,
            }
        }
    }

    /// The elements of a composite literal, at the `{`.
    fn literal_value(&mut self) -> Vec<ExprId> {
        let mut elems = Vec::new();
        self.expr_level += 1;
        self.want(T::LBrace);
        self.list("composite literal", T::Comma, T::RBrace, |p| {
            let mut elem = p.element();
            if p.got(T::Colon) {
                let value = p.element();
                let kind = ExprKind::KeyValue { key: elem, value };
                elem = p.alloc_expr(kind, p.expr_lo(elem));
            }
            elems.push(elem);
            false
        });
        self.expr_level -= 1;
        elems
    }

    /// An element or key: an expression, or a literal body without a type.
    fn element(&mut self) -> ExprId {
        if !self.at(T::LBrace) {
            return self.expr();
        }
        if !self.enter(Code::ExpressionTooDeeplyNested, "expression") {
            return self.bad_expr();
        }
        let lo = self.lo();
        let elems = with_stack(|| self.literal_value());
        self.leave();
        self.alloc_expr(ExprKind::CompositeLit { ty: None, elems }, lo)
    }
}

/// The binary operator a token stands for and its precedence, 1 to 5.
fn binary_op(kind: T) -> Option<(BinaryOp, u8)> {
    Some(match kind {
        T::PipePipe => (BinaryOp::OrOr, 1),
        T::AmpAmp => (BinaryOp::AndAnd, 2),
        T::EqEq => (BinaryOp::Eq, 3),
        T::NotEq => (BinaryOp::Ne, 3),
        T::Lt => (BinaryOp::Lt, 3),
        T::LtEq => (BinaryOp::Le, 3),
        T::Gt => (BinaryOp::Gt, 3),
        T::GtEq => (BinaryOp::Ge, 3),
        T::Plus => (BinaryOp::Add, 4),
        T::Minus => (BinaryOp::Sub, 4),
        T::Pipe => (BinaryOp::Or, 4),
        T::Caret => (BinaryOp::Xor, 4),
        T::Star => (BinaryOp::Mul, 5),
        T::Slash => (BinaryOp::Div, 5),
        T::Percent => (BinaryOp::Rem, 5),
        T::Shl => (BinaryOp::Shl, 5),
        T::Shr => (BinaryOp::Shr, 5),
        T::Amp => (BinaryOp::And, 5),
        T::AmpCaret => (BinaryOp::AndNot, 5),
        _ => return None,
    })
}
