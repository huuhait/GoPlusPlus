//! Expressions, types, parameter lists and the bodies of functions.

use super::printer::{BinaryShape, INFINITY, Node, Printer, Ws};
use super::printer::{NO_EXTRA_BLANK, NO_EXTRA_LINEBREAK};
use super::tabwriter;
use crate::ast::*;
use crate::token::TokenKind as T;
use crate::with_stack;

/// The list may end in a comma.
pub(super) const COMMA_TERM: u8 = 1;
/// The lines after the first are not indented.
pub(super) const NO_INDENT: u8 = 2;

const LOWEST_PREC: i32 = 0;
const UNARY_PREC: i32 = 6;
const HIGHEST_PREC: i32 = 7;
/// Longer than any line of an expression list.
const LIST_INFINITY: i32 = 1_000_000;

/// An element of a list of expressions or names.
#[derive(Clone, Copy)]
pub(super) enum Elem {
    Expr(ExprId),
    Ident(Ident),
}

/// Parameters declared together: `a, b int`.
pub(super) struct ParamGroup {
    pub names: Vec<Ident>,
    pub ty: ParamType,
    pub pos: u32,
    pub end: u32,
}

#[derive(Clone, Copy, PartialEq)]
pub(super) enum ParamType {
    Expr(ExprId),
    /// The fragment kind of a macro parameter.
    Fragment {
        variadic: bool,
        kind: Ident,
    },
}

#[derive(Clone, Copy, PartialEq)]
pub(super) enum ParamMode {
    Func,
    FuncTypeParams,
    TypeTypeParams,
}

fn precedence(op: BinaryOp) -> i32 {
    match op {
        BinaryOp::OrOr => 1,
        BinaryOp::AndAnd => 2,
        BinaryOp::Eq | BinaryOp::Ne | BinaryOp::Lt | BinaryOp::Le | BinaryOp::Gt | BinaryOp::Ge => {
            3
        }
        BinaryOp::Add | BinaryOp::Sub | BinaryOp::Or | BinaryOp::Xor => 4,
        BinaryOp::Mul
        | BinaryOp::Div
        | BinaryOp::Rem
        | BinaryOp::Shl
        | BinaryOp::Shr
        | BinaryOp::And
        | BinaryOp::AndNot => 5,
    }
}

pub(super) fn binary_token(op: BinaryOp) -> T {
    match op {
        BinaryOp::OrOr => T::PipePipe,
        BinaryOp::AndAnd => T::AmpAmp,
        BinaryOp::Eq => T::EqEq,
        BinaryOp::Ne => T::NotEq,
        BinaryOp::Lt => T::Lt,
        BinaryOp::Le => T::LtEq,
        BinaryOp::Gt => T::Gt,
        BinaryOp::Ge => T::GtEq,
        BinaryOp::Add => T::Plus,
        BinaryOp::Sub => T::Minus,
        BinaryOp::Or => T::Pipe,
        BinaryOp::Xor => T::Caret,
        BinaryOp::Mul => T::Star,
        BinaryOp::Div => T::Slash,
        BinaryOp::Rem => T::Percent,
        BinaryOp::Shl => T::Shl,
        BinaryOp::Shr => T::Shr,
        BinaryOp::And => T::Amp,
        BinaryOp::AndNot => T::AmpCaret,
    }
}

fn unary_token(op: UnaryOp) -> T {
    match op {
        UnaryOp::Plus => T::Plus,
        UnaryOp::Neg => T::Minus,
        UnaryOp::Not => T::Bang,
        UnaryOp::Xor => T::Caret,
        UnaryOp::Star => T::Star,
        UnaryOp::Addr => T::Amp,
        UnaryOp::Recv => T::Arrow,
        UnaryOp::Tilde => T::Tilde,
    }
}

fn literal_token(kind: LitKind) -> T {
    match kind {
        LitKind::Int => T::Int,
        LitKind::Float => T::Float,
        LitKind::Imag => T::Imag,
        LitKind::Rune => T::Rune,
        LitKind::String => T::String,
    }
}

fn reduce_depth(depth: i32) -> i32 {
    (depth - 1).max(1)
}

/// A number literal with a lower-case prefix and exponent, and an integer
/// imaginary literal without leading zeros; `None` if it is written so.
pub(crate) fn normalized_number(text: &str) -> Option<String> {
    if text.len() < 2 || !text.starts_with(|c: char| c.is_ascii_digit() || c == '.') {
        return None;
    }
    let lower = |text: &str, letter: char| {
        let at = text.rfind(letter)?;
        let mut text = text.to_owned();
        text.replace_range(at..=at, &letter.to_ascii_lowercase().to_string());
        Some(text)
    };
    match &text[..2] {
        "0X" => {
            let text = format!("0x{}", &text[2..]);
            Some(lower(&text, 'P').unwrap_or(text))
        }
        "0x" => lower(text, 'P'),
        "0O" => Some(format!("0o{}", &text[2..])),
        "0B" => Some(format!("0b{}", &text[2..])),
        "0o" | "0b" => None,
        _ => {
            if let Some(text) = lower(text, 'E') {
                return Some(text);
            }
            if text.ends_with('i') && !text.contains(['.', 'e']) {
                let digits = text.trim_start_matches(['0', '_']);
                let digits = if digits == "i" { "0i" } else { digits };
                return (digits != text).then(|| digits.to_owned());
            }
            None
        }
    }
}

impl<'a> Printer<'a> {
    // ---- shape of nodes

    fn kind(&self, id: ExprId) -> &'a ExprKind {
        &self.cx.file.exprs[id].kind
    }

    /// Where the expression starts. A type switch guard is printed as the
    /// statement it is, so as an expression it is its `x.(type)` only.
    pub(super) fn expr_pos(&self, id: ExprId) -> u32 {
        let exprs = &self.cx.file.exprs;
        match exprs[id].kind {
            ExprKind::TypeSwitchGuard { x, .. } => exprs[x].span.lo,
            _ => exprs[id].span.lo,
        }
    }

    pub(super) fn expr_end(&self, id: ExprId) -> u32 {
        self.cx.file.exprs[id].span.hi
    }

    fn elem_pos(&self, elem: Elem) -> u32 {
        match elem {
            Elem::Expr(id) => self.expr_pos(id),
            Elem::Ident(ident) => ident.span.lo,
        }
    }

    fn elem_end(&self, elem: Elem) -> u32 {
        match elem {
            Elem::Expr(id) => self.expr_end(id),
            Elem::Ident(ident) => ident.span.hi,
        }
    }

    fn strip_parens_always(&self, mut id: ExprId) -> ExprId {
        while let ExprKind::Paren(inner) = *self.kind(id) {
            id = inner;
        }
        id
    }

    /// Without the parentheses that the header of a statement does not
    /// need. They stay around a composite literal that starts with a type
    /// name, which would otherwise read as the block of the statement.
    pub(super) fn strip_parens(&self, id: ExprId) -> ExprId {
        match *self.kind(id) {
            ExprKind::Paren(inner) if !self.has_bare_literal(inner) => self.strip_parens(inner),
            _ => id,
        }
    }

    fn is_type_name(&self, id: ExprId) -> bool {
        match *self.kind(id) {
            ExprKind::Name(_) => true,
            ExprKind::Selector { x, .. } => self.is_type_name(x),
            _ => false,
        }
    }

    /// Whether a composite literal with a type name is in `id`, outside
    /// every pair of parentheses.
    fn has_bare_literal(&self, id: ExprId) -> bool {
        with_stack(|| self.bare_literal_in(id))
    }

    fn bare_literal_in(&self, id: ExprId) -> bool {
        let any = |list: &[ExprId]| list.iter().any(|&x| self.has_bare_literal(x));
        let opt = |x: Option<ExprId>| x.is_some_and(|x| self.has_bare_literal(x));
        let params = |list: &[Param]| list.iter().any(|param| self.has_bare_literal(param.ty));
        match self.kind(id) {
            ExprKind::Paren(_) => false,
            ExprKind::CompositeLit { ty, .. } => ty.is_some_and(|ty| self.is_type_name(ty)),
            ExprKind::Bad | ExprKind::Name(_) | ExprKind::BasicLit { .. } => false,
            ExprKind::KeyValue { key, value } => any(&[*key, *value]),
            ExprKind::FuncLit { ty, body } => {
                self.has_bare_literal(*ty) || self.bare_literal_in_stmts(&body.stmts)
            }
            ExprKind::Selector { x, .. }
            | ExprKind::TypeSwitchGuard { x, .. }
            | ExprKind::Unary { x, .. } => self.has_bare_literal(*x),
            ExprKind::Index { x, indices } => self.has_bare_literal(*x) || any(indices),
            ExprKind::Slice { x, lo, hi, max, .. } => {
                self.has_bare_literal(*x) || opt(*lo) || opt(*hi) || opt(*max)
            }
            ExprKind::Assert { x, ty } => any(&[*x, *ty]),
            ExprKind::Binary { x, y, .. } => any(&[*x, *y]),
            ExprKind::Call { fun, args, .. } => self.has_bare_literal(*fun) || any(args),
            ExprKind::MacroCall { args, .. } => any(args),
            ExprKind::ArrayType { len, elem } => opt(*len) || self.has_bare_literal(*elem),
            ExprKind::SliceType { elem }
            | ExprKind::DotsType { elem }
            | ExprKind::ChanType { elem, .. } => self.has_bare_literal(*elem),
            ExprKind::StructType { fields } => fields.iter().any(|field| {
                self.has_bare_literal(field.ty) || opt(field.tag) || self.in_attrs(&field.attrs)
            }),
            ExprKind::InterfaceType { elems } => elems.iter().any(|elem| match elem {
                InterfaceElem::Method { ty, .. } | InterfaceElem::Type(ty) => {
                    self.has_bare_literal(*ty)
                }
            }),
            ExprKind::FuncType {
                params: list,
                results,
                ..
            } => params(list) || params(results),
            ExprKind::MapType { key, value } => any(&[*key, *value]),
        }
    }

    fn in_attrs(&self, attrs: &[Attribute]) -> bool {
        let args = attrs.iter().filter_map(|attr| attr.args.as_ref()).flatten();
        args.into_iter().any(|&arg| self.has_bare_literal(arg))
    }

    fn bare_literal_in_stmts(&self, stmts: &[StmtId]) -> bool {
        stmts
            .iter()
            .any(|&stmt| with_stack(|| self.bare_literal_in_stmt(stmt)))
    }

    fn bare_literal_in_stmt(&self, id: StmtId) -> bool {
        let file = self.cx.file;
        let any = |list: &[ExprId]| list.iter().any(|&x| self.has_bare_literal(x));
        let opt = |x: Option<ExprId>| x.is_some_and(|x| self.has_bare_literal(x));
        let stmt = |s: Option<StmtId>| s.is_some_and(|s| self.bare_literal_in_stmts(&[s]));
        match &file.stmts[id].kind {
            StmtKind::Bad | StmtKind::Empty | StmtKind::Branch { .. } => false,
            StmtKind::Labeled { stmt: inner, .. } => stmt(Some(*inner)),
            StmtKind::Block(block) => self.bare_literal_in_stmts(&block.stmts),
            StmtKind::Expr(x) | StmtKind::Go(x) | StmtKind::Defer(x) => self.has_bare_literal(*x),
            StmtKind::IncDec { x, .. } => self.has_bare_literal(*x),
            StmtKind::Send { chan, value } => any(&[*chan, *value]),
            StmtKind::Decl(decls) => decls.iter().any(|&decl| match &file.decl_nodes[decl].kind {
                DeclKind::Const { ty, values, .. } | DeclKind::Var { ty, values, .. } => {
                    opt(*ty) || any(values)
                }
                DeclKind::Type {
                    type_params, ty, ..
                } => {
                    self.has_bare_literal(*ty)
                        || type_params
                            .iter()
                            .any(|param| self.has_bare_literal(param.bound))
                }
                _ => false,
            }),
            StmtKind::Assign { lhs, rhs, .. } => any(lhs) || any(rhs),
            StmtKind::Return(results) => any(results),
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => {
                stmt(*init)
                    || self.has_bare_literal(*cond)
                    || self.bare_literal_in_stmts(&then.stmts)
                    || stmt(*otherwise)
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                stmt(*init) || opt(*cond) || stmt(*post) || self.bare_literal_in_stmts(&body.stmts)
            }
            StmtKind::Range { lhs, x, body, .. } => {
                any(lhs) || self.has_bare_literal(*x) || self.bare_literal_in_stmts(&body.stmts)
            }
            StmtKind::Switch { init, tag, clauses } => {
                stmt(*init)
                    || opt(*tag)
                    || clauses.iter().any(|clause| {
                        clause.cases.as_deref().is_some_and(any)
                            || self.bare_literal_in_stmts(&clause.body)
                    })
            }
            StmtKind::Select { clauses } => clauses
                .iter()
                .any(|clause| stmt(clause.comm) || self.bare_literal_in_stmts(&clause.body)),
        }
    }

    // ---- line breaks

    /// Breaks the line to reach source line `line`, with at least `min`
    /// line ends and at most one blank line; `ws` goes before the first.
    /// With `new_section` the first line end closes all columns. Returns 0
    /// for no line end, 1 for one newline, more for a formfeed or more
    /// than one newline.
    pub(super) fn linebreak(&mut self, line: i32, min: i32, ws: Ws, new_section: bool) -> i32 {
        let mut count = (line - self.pos.line).min(2).max(min);
        let mut breaks = 0;
        if count > 0 {
            self.ws(ws);
            if new_section {
                self.ws(Ws::Formfeed);
                count -= 1;
                breaks = 2;
            }
            breaks += count;
            for _ in 0..count {
                self.ws(Ws::Newline);
            }
        }
        breaks
    }

    /// A node that only has a documentation or line comment in a measuring
    /// run, where there are no other comments. Such a comment ends a line,
    /// so the node has no one-line size.
    pub(super) fn set_comment(&mut self, present: bool) {
        self.aborted |= present && self.measure.is_some();
    }

    // ---- lists

    pub(super) fn ident_list(&mut self, names: &[Ident], indent: bool) {
        let list: Vec<Elem> = names.iter().map(|&name| Elem::Ident(name)).collect();
        let mode = if indent { 0 } else { NO_INDENT };
        self.elem_list(None, &list, 1, mode, None);
    }

    pub(super) fn expr_list(
        &mut self,
        prev: Option<u32>,
        list: &[ExprId],
        depth: i32,
        mode: u8,
        next: Option<u32>,
    ) {
        let list: Vec<Elem> = list.iter().map(|&x| Elem::Expr(x)).collect();
        self.elem_list(prev, &list, depth, mode, next);
    }

    fn elem(&mut self, elem: Elem, depth: i32) {
        match elem {
            Elem::Expr(id) => self.expr0(id, depth),
            Elem::Ident(ident) => self.name(ident),
        }
    }

    pub(super) fn name(&mut self, ident: Ident) {
        self.set_pos(ident.span.lo);
        self.ident(self.cx.name(ident));
    }

    /// A list of expressions. Where the list is on several lines in the
    /// source, its line breaks are kept. `prev` and `next` are the tokens
    /// around the list, when their place matters.
    pub(super) fn elem_list(
        &mut self,
        prev: Option<u32>,
        list: &[Elem],
        depth: i32,
        mode: u8,
        next: Option<u32>,
    ) {
        let cx = self.cx;
        let (Some(&first), Some(&last)) = (list.first(), list.last()) else {
            return;
        };
        let mut prev_line = prev.map_or(0, |prev| cx.line(prev));
        let mut line = cx.line(self.elem_pos(first));
        let end_line = cx.line(self.elem_end(last));

        if prev.is_some() && prev_line == line && line == end_line {
            for (index, &x) in list.iter().enumerate() {
                if index > 0 {
                    // The comma takes the place of the expression after
                    // it, so that a comment before that one stays there.
                    self.set_pos(self.elem_pos(x));
                    self.tok(T::Comma);
                    self.ws(Ws::Blank);
                }
                self.elem(x, depth);
            }
            return;
        }

        let mut ws = if mode & NO_INDENT == 0 {
            Ws::Indent
        } else {
            Ws::Ignore
        };
        // The index of the last element after a line break.
        let mut prev_break: i64 = -1;
        if prev.is_some() && prev_line < line && self.linebreak(line, 0, ws, true) > 0 {
            ws = Ws::Ignore;
            prev_break = 0;
        }

        // The size of an element, or of its key, when it fits on a line;
        // else 0. Consecutive lines whose sizes are close share columns.
        let mut size = 0;
        let mut log_sum = 0.0_f64;
        let mut count = 0;
        for (index, &x) in list.iter().enumerate() {
            if self.aborted {
                return;
            }
            line = cx.line(self.elem_pos(x));
            let pair = match x {
                Elem::Expr(id) => match *self.kind(id) {
                    ExprKind::KeyValue { key, value } => Some((key, value)),
                    _ => None,
                },
                Elem::Ident(_) => None,
            };

            let prev_size = size;
            size = 0;
            if let (Some(_), Some(_), Elem::Expr(id)) = (prev, next, x) {
                let whole = self.node_size(Node::Expr(id), LIST_INFINITY);
                if whole > LIST_INFINITY && self.measure.is_some() {
                    // The element has a line end in it, so the node that
                    // is measured has one too: no need to print it.
                    self.aborted = true;
                    return;
                }
                if whole <= LIST_INFINITY {
                    size = match pair {
                        Some((key, _)) => self.node_size(Node::Expr(key), LIST_INFINITY),
                        None => whole,
                    };
                }
            }

            let mut use_formfeed = true;
            if prev_size > 0 && size > 0 {
                const SMALL: i32 = 40;
                if count == 0 || prev_size <= SMALL && size <= SMALL {
                    use_formfeed = false;
                } else {
                    const THRESHOLD: f64 = 2.5;
                    let mean = (log_sum / f64::from(count)).exp();
                    let ratio = f64::from(size) / mean;
                    use_formfeed = THRESHOLD * ratio <= 1.0 || THRESHOLD <= ratio;
                }
            }

            let needs_linebreak = 0 < prev_line && prev_line < line;
            if index > 0 {
                if !needs_linebreak {
                    self.set_pos(self.elem_pos(x));
                }
                self.tok(T::Comma);
                let mut needs_blank = true;
                if needs_linebreak {
                    let new_section = use_formfeed || prev_break + 1 < index as i64;
                    let breaks = self.linebreak(line, 0, ws, new_section);
                    if breaks > 0 {
                        ws = Ws::Ignore;
                        prev_break = index as i64;
                        needs_blank = false;
                    }
                    if breaks > 1 {
                        log_sum = 0.0;
                        count = 0;
                    }
                }
                if needs_blank {
                    self.ws(Ws::Blank);
                }
            }

            match pair {
                Some((key, value)) if list.len() > 1 && size > 0 && needs_linebreak => {
                    // The key gets a column of its own.
                    self.expr(key);
                    self.set_pos(cx.find(self.expr_end(key), T::Colon));
                    self.tok(T::Colon);
                    self.ws(Ws::Vtab);
                    self.expr(value);
                }
                _ => self.elem(x, depth),
            }

            if size > 0 {
                log_sum += f64::from(size).ln();
                count += 1;
            }
            prev_line = line;
        }

        let indented = ws == Ws::Ignore && mode & NO_INDENT == 0;
        let next_line = next.map(|next| cx.line(next));
        if mode & COMMA_TERM != 0 && next_line.is_some_and(|next| self.pos.line < next) {
            // The closing token is on a line of its own.
            self.tok(T::Comma);
            if indented {
                self.ws(Ws::Unindent);
            }
            self.ws(Ws::Formfeed);
            return;
        }
        if indented {
            self.ws(Ws::Unindent);
        }
    }

    // ---- sizes

    /// The length of `node` printed on one line, if that is at most `max`;
    /// else more than `max`.
    pub(super) fn node_size(&mut self, node: Node, max: i32) -> i32 {
        if let Some(&size) = self.sizes.get(&node) {
            return size;
        }
        let mut size = max + 1;
        self.sizes.insert(node, size);

        let mut measurer = Printer::new(self.cx, Some(max));
        measurer.sizes = std::mem::take(&mut self.sizes);
        measurer.shapes = std::mem::take(&mut self.shapes);
        match node {
            Node::Expr(id) => measurer.expr(id),
            Node::Stmt(id) => {
                let labeled = matches!(self.cx.file.stmts[id].kind, StmtKind::Labeled { .. });
                measurer.indent = i32::from(labeled);
                measurer.stmt(id, false);
            }
        }
        measurer.finish();
        self.sizes = std::mem::take(&mut measurer.sizes);
        self.shapes = std::mem::take(&mut measurer.shapes);
        self.steps += measurer.steps;

        if !measurer.aborted {
            let (text, has_newline) = tabwriter::trim(&measurer.output);
            if !has_newline && text.len() as i64 <= i64::from(max) {
                size = text.len() as i32;
                self.sizes.insert(node, size);
            }
        }
        size
    }

    // ---- parameters

    pub(super) fn param_groups(&self, params: &[Param]) -> Vec<ParamGroup> {
        let mut groups: Vec<ParamGroup> = Vec::new();
        for param in params {
            let ty = ParamType::Expr(param.ty);
            match (param.name, groups.last_mut()) {
                (Some(name), Some(last)) if !last.names.is_empty() && last.ty == ty => {
                    last.names.push(name);
                }
                _ => groups.push(ParamGroup {
                    names: param.name.into_iter().collect(),
                    ty,
                    pos: param
                        .name
                        .map_or(self.expr_pos(param.ty), |name| name.span.lo),
                    end: self.expr_end(param.ty),
                }),
            }
        }
        groups
    }

    pub(super) fn type_param_groups(&self, params: &[TypeParam]) -> Vec<ParamGroup> {
        let mut groups: Vec<ParamGroup> = Vec::new();
        for param in params {
            let ty = ParamType::Expr(param.bound);
            match groups.last_mut() {
                Some(last) if last.ty == ty => last.names.push(param.name),
                _ => groups.push(ParamGroup {
                    names: vec![param.name],
                    ty,
                    pos: param.name.span.lo,
                    end: self.expr_end(param.bound),
                }),
            }
        }
        groups
    }

    fn param_type(&mut self, ty: ParamType) {
        match ty {
            ParamType::Expr(id) => self.expr(self.strip_parens_always(id)),
            ParamType::Fragment { variadic, kind } => {
                if variadic {
                    self.tok(T::Ellipsis);
                }
                self.name(kind);
            }
        }
    }

    /// `x` after a name reads as an expression: `P *T` as a product.
    fn combines_with_name(&self, x: ExprId) -> bool {
        match *self.kind(x) {
            ExprKind::Unary {
                op: UnaryOp::Star,
                x,
            } => !self.is_type_elem(x),
            ExprKind::Binary { x, y, .. } => self.combines_with_name(x) && !self.is_type_elem(y),
            ExprKind::Paren(x) => !self.is_type_elem(x),
            _ => false,
        }
    }

    fn is_type_elem(&self, x: ExprId) -> bool {
        match *self.kind(x) {
            ExprKind::ArrayType { .. }
            | ExprKind::SliceType { .. }
            | ExprKind::StructType { .. }
            | ExprKind::FuncType { .. }
            | ExprKind::InterfaceType { .. }
            | ExprKind::MapType { .. }
            | ExprKind::ChanType { .. } => true,
            ExprKind::Unary { op, .. } => op == UnaryOp::Tilde,
            ExprKind::Binary { x, y, .. } => self.is_type_elem(x) || self.is_type_elem(y),
            ExprKind::Paren(x) => self.is_type_elem(x),
            _ => false,
        }
    }

    /// A parameter list between the tokens at `opening` and `closing`.
    pub(super) fn parameters(
        &mut self,
        opening: u32,
        groups: &[ParamGroup],
        closing: u32,
        mode: ParamMode,
    ) {
        let cx = self.cx;
        let (open, close) = match mode {
            ParamMode::Func => (T::LParen, T::RParen),
            _ => (T::LBrack, T::RBrack),
        };
        self.set_pos(opening);
        self.tok(open);
        if !groups.is_empty() {
            let mut prev_line = cx.line(opening);
            let mut ws = Ws::Indent;
            for (index, group) in groups.iter().enumerate() {
                let (first_line, last_line) = (cx.line(group.pos), cx.line(group.end));
                let needs_linebreak = 0 < prev_line && prev_line < first_line;
                if index > 0 {
                    if !needs_linebreak {
                        self.set_pos(group.pos);
                    }
                    self.tok(T::Comma);
                }
                if needs_linebreak && self.linebreak(first_line, 0, ws, true) > 0 {
                    ws = Ws::Ignore;
                } else if index > 0 {
                    self.ws(Ws::Blank);
                }
                if !group.names.is_empty() {
                    // Names over several lines are indented here only if
                    // no line break has indented the list yet.
                    self.ident_list(&group.names, ws == Ws::Indent);
                    self.ws(Ws::Blank);
                }
                self.param_type(group.ty);
                prev_line = last_line;
            }

            let closing_line = cx.line(closing);
            let single = groups.len() == 1 && groups[0].names.len() <= 1;
            if 0 < prev_line && prev_line < closing_line {
                self.tok(T::Comma);
                self.linebreak(closing_line, 0, Ws::Ignore, true);
            } else if let (ParamMode::TypeTypeParams, true, ParamType::Expr(ty)) =
                (mode, single, groups[0].ty)
                && self.combines_with_name(self.strip_parens_always(ty))
            {
                // `[P *T]` after a type name would read as an array length.
                self.tok(T::Comma);
            }
            if ws == Ws::Ignore {
                self.ws(Ws::Unindent);
            }
        }
        self.set_pos(closing);
        self.tok(close);
    }

    /// The type parameters, `!`, parameters and results of a function,
    /// whose tokens start at or after `from`.
    pub(super) fn signature(&mut self, from: u32, type_params: &[TypeParam], ty: ExprId) {
        let cx = self.cx;
        let ExprKind::FuncType {
            params,
            results,
            fails,
        } = self.kind(ty)
        else {
            return;
        };
        let mut index = cx.token_at(from);
        if let Some(last) = type_params.last() {
            let close = cx.closing(self.expr_end(last.bound));
            let groups = self.type_param_groups(type_params);
            let mode = ParamMode::FuncTypeParams;
            self.parameters(cx.lo(index), &groups, cx.lo(close), mode);
            index = close + 1;
        }
        if *fails {
            self.tok(T::Bang);
            index += 1;
        }
        let close = match params.last() {
            Some(last) => cx.closing(self.expr_end(last.ty)),
            None => index + 1,
        };
        let groups = self.param_groups(params);
        self.parameters(cx.lo(index), &groups, cx.lo(close), ParamMode::Func);

        let Some(last) = results.last() else {
            return;
        };
        self.ws(Ws::Blank);
        if let [only] = results[..]
            && only.name.is_none()
        {
            self.expr(self.strip_parens_always(only.ty));
            return;
        }
        let groups = self.param_groups(results);
        let closing = cx.lo(cx.closing(self.expr_end(last.ty)));
        self.parameters(cx.lo(close + 1), &groups, closing, ParamMode::Func);
    }

    // ---- struct and interface bodies

    fn names_size(&self, names: &[Ident], max: i32) -> i32 {
        let mut size = 0;
        for (index, &name) in names.iter().enumerate() {
            if index > 0 {
                size += 2;
            }
            size += self.cx.name(name).chars().count() as i32;
            if size >= max {
                break;
            }
        }
        size
    }

    /// Whether a struct or interface with this one member, of type `ty`,
    /// is short enough for one line.
    fn fits_on_one_line(&mut self, names: &[Ident], ty: ExprId) -> bool {
        const MAX: i32 = 30;
        let names_size = self.names_size(names, MAX).min(1);
        names_size + self.node_size(Node::Expr(ty), MAX) <= MAX
    }

    fn struct_type(&mut self, id: ExprId, fields: &'a [Field]) {
        let cx = self.cx;
        let span = cx.file.exprs[id].span;
        let lbrace = cx.find(span.lo + 1, T::LBrace);
        let rbrace = span.hi - 1;
        let has_comments = self.comment_before(cx.pos(rbrace));
        let on_one_line = cx.line(lbrace) == cx.line(rbrace);

        if !has_comments && on_one_line {
            let Some(field) = fields.first() else {
                self.set_pos(lbrace);
                self.tok(T::LBrace);
                self.set_pos(rbrace);
                self.tok(T::RBrace);
                return;
            };
            let plain = fields.len() == 1
                && field.tag.is_none()
                && field.attrs.is_empty()
                && cx.line_comment(field.span.hi).is_none();
            if plain && self.fits_on_one_line(&field.names, field.ty) {
                self.set_pos(lbrace);
                self.tok(T::LBrace);
                self.ws(Ws::Blank);
                for (index, &name) in field.names.iter().enumerate() {
                    if index > 0 {
                        self.tok(T::Comma);
                        self.ws(Ws::Blank);
                    }
                    self.name(name);
                }
                if !field.names.is_empty() {
                    self.ws(Ws::Blank);
                }
                self.expr(field.ty);
                self.ws(Ws::Blank);
                self.set_pos(rbrace);
                self.tok(T::RBrace);
                return;
            }
        }

        self.ws(Ws::Blank);
        self.set_pos(lbrace);
        self.tok(T::LBrace);
        self.ws(Ws::Indent);
        if has_comments || !fields.is_empty() {
            self.ws(Ws::Formfeed);
        }
        let separator = if fields.len() == 1 {
            Ws::Blank
        } else {
            Ws::Vtab
        };
        let slot = self.new_line_slot();
        for (index, field) in fields.iter().enumerate() {
            if self.aborted {
                break;
            }
            if index > 0 {
                let new_section = self.lines_from(slot) > 0;
                self.linebreak(cx.line(field.span.lo), 1, Ws::Ignore, new_section);
            }
            self.set_comment(cx.has_doc(field.span.lo));
            self.record_line(slot);
            // How many columns a comment after the field has to skip.
            let mut extra_tabs = if field.names.is_empty() {
                self.expr(field.ty);
                2
            } else {
                self.ident_list(&field.names, false);
                self.ws(separator);
                self.expr(field.ty);
                1
            };
            for attr in &field.attrs {
                self.ws(Ws::Blank);
                self.attribute(attr);
            }
            if let Some(tag) = field.tag {
                if !field.names.is_empty() && separator == Ws::Vtab {
                    self.ws(separator);
                }
                self.ws(separator);
                self.expr(tag);
                extra_tabs = 0;
            }
            if cx.line_comment(field.span.hi).is_some() {
                for _ in 0..extra_tabs {
                    self.ws(separator);
                }
                self.set_comment(true);
            }
        }
        self.drop_line_slot(slot);
        self.ws(Ws::Unindent);
        self.ws(Ws::Formfeed);
        self.set_pos(rbrace);
        self.tok(T::RBrace);
    }

    fn interface_elem(&mut self, elem: &InterfaceElem) {
        match *elem {
            InterfaceElem::Method { name, ty } => {
                self.name(name);
                self.signature(name.span.hi, &[], ty);
            }
            InterfaceElem::Type(ty) => self.expr(ty),
        }
    }

    fn interface_type(&mut self, id: ExprId, elems: &'a [InterfaceElem]) {
        let cx = self.cx;
        let span = cx.file.exprs[id].span;
        let lbrace = cx.find(span.lo + 1, T::LBrace);
        let rbrace = span.hi - 1;
        let has_comments = self.comment_before(cx.pos(rbrace));
        let on_one_line = cx.line(lbrace) == cx.line(rbrace);
        let bounds = |elem: &InterfaceElem| match *elem {
            InterfaceElem::Method { name, ty } => (name.span.lo, self.expr_end(ty)),
            InterfaceElem::Type(ty) => (self.expr_pos(ty), self.expr_end(ty)),
        };
        let bounds: Vec<(u32, u32)> = elems.iter().map(bounds).collect();

        if !has_comments && on_one_line {
            let Some(elem) = elems.first() else {
                self.set_pos(lbrace);
                self.tok(T::LBrace);
                self.set_pos(rbrace);
                self.tok(T::RBrace);
                return;
            };
            let (names, ty) = match *elem {
                InterfaceElem::Method { name, ty } => (vec![name], ty),
                InterfaceElem::Type(ty) => (Vec::new(), ty),
            };
            let plain = elems.len() == 1 && cx.line_comment(bounds[0].1).is_none();
            if plain && self.fits_on_one_line(&names, ty) {
                self.set_pos(lbrace);
                self.tok(T::LBrace);
                self.ws(Ws::Blank);
                self.interface_elem(elem);
                self.ws(Ws::Blank);
                self.set_pos(rbrace);
                self.tok(T::RBrace);
                return;
            }
        }

        self.ws(Ws::Blank);
        self.set_pos(lbrace);
        self.tok(T::LBrace);
        self.ws(Ws::Indent);
        if has_comments || !elems.is_empty() {
            self.ws(Ws::Formfeed);
        }
        let slot = self.new_line_slot();
        for (index, elem) in elems.iter().enumerate() {
            if self.aborted {
                break;
            }
            let (pos, end) = bounds[index];
            if index > 0 {
                let new_section = self.lines_from(slot) > 0;
                self.linebreak(cx.line(pos), 1, Ws::Ignore, new_section);
            }
            self.set_comment(cx.has_doc(pos));
            self.record_line(slot);
            self.interface_elem(elem);
            self.set_comment(cx.line_comment(end).is_some());
        }
        self.drop_line_slot(slot);
        self.ws(Ws::Unindent);
        self.ws(Ws::Formfeed);
        self.set_pos(rbrace);
        self.tok(T::RBrace);
    }

    // ---- attributes and macro calls

    fn macro_ref(&mut self, path: MacroRef) {
        if let Some(package) = path.package {
            self.name(package);
            self.tok(T::Dot);
        }
        self.name(path.name);
    }

    /// The arguments of a macro call or attribute, which end at `rparen`.
    fn macro_args(&mut self, path: MacroRef, args: &[ExprId], rparen: u32) {
        let cx = self.cx;
        let lparen = cx.find(path.name.span.hi, T::LParen);
        let depth = if args.len() > 1 { 2 } else { 1 };
        self.set_pos(lparen);
        self.tok(T::LParen);
        // The tree does not say whether the last argument is spread.
        let last = args.last().map(|&last| cx.token_at(self.expr_end(last)));
        match last.filter(|&last| cx.kind(last) == T::Ellipsis) {
            Some(ellipsis) => {
                let ellipsis = cx.lo(ellipsis);
                self.expr_list(Some(lparen), args, depth, 0, Some(ellipsis));
                self.set_pos(ellipsis);
                self.tok(T::Ellipsis);
                if cx.line(ellipsis) < cx.line(rparen) {
                    self.tok(T::Comma);
                    self.ws(Ws::Formfeed);
                }
            }
            None => self.expr_list(Some(lparen), args, depth, COMMA_TERM, Some(rparen)),
        }
        self.set_pos(rparen);
        self.tok(T::RParen);
    }

    pub(super) fn attribute(&mut self, attr: &Attribute) {
        self.set_pos(attr.span.lo);
        self.tok(T::At);
        self.macro_ref(attr.path);
        if let Some(args) = &attr.args {
            self.macro_args(attr.path, args, attr.span.hi - 1);
        }
    }

    // ---- binary expressions

    /// Which operators `id` has outside parentheses, and whether one of
    /// them would join with the operator of its right operand.
    fn binary_shape(&mut self, id: ExprId) -> BinaryShape {
        if let Some(&shape) = self.shapes.get(&id) {
            return shape;
        }
        self.steps += 1;
        let ExprKind::Binary { op, x, y } = *self.kind(id) else {
            return (false, false, 0);
        };
        let prec = precedence(op);
        let (mut has4, mut has5, mut problem) = (prec == 4, prec == 5, 0);
        if let ExprKind::Binary { op: left, .. } = *self.kind(x)
            && precedence(left) >= prec
        {
            let (left4, left5, left_problem) = with_stack(|| self.binary_shape(x));
            (has4, has5, problem) = (has4 || left4, has5 || left5, problem.max(left_problem));
        }
        match *self.kind(y) {
            ExprKind::Binary { op: right, .. } if precedence(right) > prec => {
                let (right4, right5, right_problem) = with_stack(|| self.binary_shape(y));
                (has4, has5, problem) =
                    (has4 || right4, has5 || right5, problem.max(right_problem));
            }
            ExprKind::Unary { op: right, .. } => match (op, right) {
                (BinaryOp::Div, UnaryOp::Star)
                | (BinaryOp::And, UnaryOp::Addr)
                | (BinaryOp::And, UnaryOp::Xor) => problem = 5,
                (BinaryOp::Add, UnaryOp::Plus) | (BinaryOp::Sub, UnaryOp::Neg) => {
                    problem = problem.max(4);
                }
                _ => {}
            },
            _ => {}
        }
        self.shapes.insert(id, (has4, has5, problem));
        (has4, has5, problem)
    }

    /// The precedence below which operators get blanks around them. At
    /// depth 1 operators are spaced, unless both `+` and `*` levels are
    /// there: then only the `+` level is. Deeper, none is.
    fn cutoff(&mut self, id: ExprId, depth: i32) -> i32 {
        let (has4, has5, problem) = self.binary_shape(id);
        if problem > 0 {
            return problem + 1;
        }
        match (has4 && has5, depth == 1) {
            (true, true) => 5,
            (false, true) => 6,
            (_, false) => 4,
        }
    }

    fn binary_expr(&mut self, id: ExprId, prec1: i32, cutoff: i32, depth: i32) {
        let cx = self.cx;
        let ExprKind::Binary { op, x, y } = *self.kind(id) else {
            return;
        };
        let prec = precedence(op);
        if prec < prec1 {
            // The tree groups tighter than the operators do.
            self.tok(T::LParen);
            self.expr0(id, reduce_depth(depth));
            self.tok(T::RParen);
            return;
        }
        let mut blank = prec < cutoff;
        let mut ws = Ws::Indent;
        let same = |operand: ExprId| match *self.kind(operand) {
            ExprKind::Binary { op, .. } => precedence(op) == prec,
            _ => false,
        };
        let x_depth = depth + i32::from(!same(x));
        self.expr1(x, prec, x_depth);
        if blank {
            self.ws(Ws::Blank);
        }
        let x_line = self.pos.line;
        let y_line = cx.line(self.expr_pos(y));
        self.set_pos(cx.lo(cx.token_at(self.expr_end(x))));
        self.tok(binary_token(op));
        if x_line != y_line && x_line > 0 && y_line > 0 && self.linebreak(y_line, 1, ws, true) > 0 {
            ws = Ws::Ignore;
            blank = false;
        }
        if blank {
            self.ws(Ws::Blank);
        }
        self.expr1(y, prec + 1, depth + 1);
        if ws == Ws::Ignore {
            self.ws(Ws::Unindent);
        }
    }

    // ---- expressions

    pub(super) fn expr(&mut self, id: ExprId) {
        self.expr1(id, LOWEST_PREC, 1);
    }

    pub(super) fn expr0(&mut self, id: ExprId, depth: i32) {
        self.expr1(id, LOWEST_PREC, depth);
    }

    fn expr1(&mut self, id: ExprId, prec1: i32, depth: i32) {
        if !self.aborted {
            with_stack(|| self.expr_node(id, prec1, depth));
        }
    }

    /// A selector, and whether its name is on a line after its operand: a
    /// method call then stays indented up to the end of its arguments.
    fn selector(&mut self, x: ExprId, sel: Ident, depth: i32, is_method: bool) -> bool {
        self.expr1(x, HIGHEST_PREC, depth);
        self.tok(T::Dot);
        let line = self.cx.line(sel.span.lo);
        if self.pos.line > 0 && self.pos.line < line {
            self.ws(Ws::Indent);
            self.ws(Ws::Newline);
            self.name(sel);
            if !is_method {
                self.ws(Ws::Unindent);
            }
            return true;
        }
        self.name(sel);
        false
    }

    fn expr_node(&mut self, id: ExprId, prec1: i32, depth: i32) {
        let cx = self.cx;
        let span = cx.file.exprs[id].span;
        self.set_pos(self.expr_pos(id));
        match self.kind(id) {
            ExprKind::Bad => self.literal(T::String, cx.source(span.lo, span.hi)),
            ExprKind::Name(name) => self.ident(cx.names.resolve(name)),
            ExprKind::Binary { .. } => {
                let cutoff = self.cutoff(id, depth.max(1));
                self.binary_expr(id, prec1, cutoff, depth.max(1));
            }
            &ExprKind::KeyValue { key, value } => {
                self.expr(key);
                self.set_pos(cx.find(self.expr_end(key), T::Colon));
                self.tok(T::Colon);
                self.ws(Ws::Blank);
                self.expr(value);
            }
            &ExprKind::Unary { op, x } => {
                if UNARY_PREC < prec1 {
                    self.tok(T::LParen);
                    if op == UnaryOp::Star {
                        self.tok(T::Star);
                        self.expr(x);
                    } else {
                        self.expr(id);
                    }
                    self.tok(T::RParen);
                } else if op == UnaryOp::Star {
                    self.tok(T::Star);
                    self.expr(x);
                } else {
                    self.tok(unary_token(op));
                    self.expr1(x, UNARY_PREC, depth);
                }
            }
            &ExprKind::BasicLit { kind, .. } => {
                let text = cx.source(span.lo, span.hi);
                let raw_with_cr = text.starts_with('`') && text.contains('\r');
                let changed = if raw_with_cr {
                    Some(text.replace('\r', ""))
                } else if kind == LitKind::String || kind == LitKind::Rune {
                    None
                } else {
                    normalized_number(text)
                };
                self.literal(literal_token(kind), changed.as_deref().unwrap_or(text));
            }
            ExprKind::FuncLit { ty, body } => {
                self.tok(T::Func);
                let start_column = self.out.column - 4;
                self.signature(span.lo + 4, &[], *ty);
                let header = self.distance_from(span.lo, start_column);
                self.func_body(header, Ws::Blank, Some(body));
            }
            &ExprKind::Paren(x) => {
                if matches!(self.kind(x), ExprKind::Paren(_)) {
                    self.expr0(x, depth);
                } else {
                    self.tok(T::LParen);
                    self.expr0(x, reduce_depth(depth));
                    self.set_pos(span.hi - 1);
                    self.tok(T::RParen);
                }
            }
            &ExprKind::Selector { x, sel } => {
                self.selector(x, sel, depth, false);
            }
            &ExprKind::Assert { x, ty } => {
                self.expr1(x, HIGHEST_PREC, depth);
                self.tok(T::Dot);
                self.set_pos(cx.find(self.expr_end(x), T::LParen));
                self.tok(T::LParen);
                self.expr(ty);
                self.set_pos(span.hi - 1);
                self.tok(T::RParen);
            }
            &ExprKind::TypeSwitchGuard { x, .. } => {
                self.expr1(x, HIGHEST_PREC, depth);
                self.tok(T::Dot);
                self.set_pos(cx.find(self.expr_end(x), T::LParen));
                self.tok(T::LParen);
                self.tok(T::Type);
                self.set_pos(span.hi - 1);
                self.tok(T::RParen);
            }
            ExprKind::Index { x, indices } => {
                self.expr1(*x, HIGHEST_PREC, 1);
                let lbrack = cx.find(self.expr_end(*x), T::LBrack);
                let rbrack = span.hi - 1;
                self.set_pos(lbrack);
                self.tok(T::LBrack);
                if let [index] = indices[..] {
                    self.expr0(index, depth + 1);
                } else {
                    self.expr_list(Some(lbrack), indices, depth + 1, COMMA_TERM, Some(rbrack));
                }
                self.set_pos(rbrack);
                self.tok(T::RBrack);
            }
            &ExprKind::Slice { x, lo, hi, max, .. } => {
                self.expr1(x, HIGHEST_PREC, 1);
                self.set_pos(cx.find(self.expr_end(x), T::LBrack));
                self.tok(T::LBrack);
                let mut indices = vec![lo, hi];
                if max.is_some() {
                    indices.push(max);
                }
                // With a binary expression among several indices, the
                // colons get blanks around them.
                let present = indices.iter().flatten();
                let binaries = present
                    .clone()
                    .any(|&index| matches!(self.kind(index), ExprKind::Binary { .. }));
                let blanks = depth <= 1 && present.count() > 1 && binaries;
                for (number, &index) in indices.iter().enumerate() {
                    if number > 0 {
                        if indices[number - 1].is_some() && blanks {
                            self.ws(Ws::Blank);
                        }
                        self.tok(T::Colon);
                        if index.is_some() && blanks {
                            self.ws(Ws::Blank);
                        }
                    }
                    if let Some(index) = index {
                        self.expr0(index, depth + 1);
                    }
                }
                self.set_pos(span.hi - 1);
                self.tok(T::RBrack);
            }
            ExprKind::Call {
                fun,
                args,
                spread,
                unwrap,
            } => {
                let depth = depth + i32::from(args.len() > 1);
                // A conversion to a function type or to a receive-only
                // channel type needs parentheses around the type.
                let paren = match self.kind(*fun) {
                    ExprKind::FuncType { .. } => true,
                    ExprKind::ChanType { dir, .. } => *dir == ChanDir::Recv,
                    _ => false,
                };
                if paren {
                    self.tok(T::LParen);
                }
                let indented = match *self.kind(*fun) {
                    ExprKind::Selector { x, sel } => self.selector(x, sel, depth, true),
                    _ => {
                        self.expr1(*fun, HIGHEST_PREC, depth);
                        false
                    }
                };
                if paren {
                    self.tok(T::RParen);
                }
                if *unwrap {
                    self.tok(T::Bang);
                }
                let lparen = cx.find(self.expr_end(*fun), T::LParen);
                let rparen = span.hi - 1;
                self.set_pos(lparen);
                self.tok(T::LParen);
                match args.last() {
                    Some(&last) if *spread => {
                        let ellipsis = cx.find(self.expr_end(last), T::Ellipsis);
                        self.expr_list(Some(lparen), args, depth, 0, Some(ellipsis));
                        self.set_pos(ellipsis);
                        self.tok(T::Ellipsis);
                        if cx.line(ellipsis) < cx.line(rparen) {
                            self.tok(T::Comma);
                            self.ws(Ws::Formfeed);
                        }
                    }
                    _ => self.expr_list(Some(lparen), args, depth, COMMA_TERM, Some(rparen)),
                }
                self.set_pos(rparen);
                self.tok(T::RParen);
                if indented {
                    self.ws(Ws::Unindent);
                }
            }
            ExprKind::MacroCall { path, args } => {
                self.tok(T::Hash);
                self.macro_ref(*path);
                self.macro_args(*path, args, span.hi - 1);
            }
            ExprKind::CompositeLit { ty, elems } => {
                if let Some(ty) = *ty {
                    self.expr1(ty, HIGHEST_PREC, depth);
                }
                self.level += 1;
                let lbrace = match *ty {
                    Some(ty) => cx.find(self.expr_end(ty), T::LBrace),
                    None => span.lo,
                };
                let rbrace = span.hi - 1;
                self.set_pos(lbrace);
                self.tok(T::LBrace);
                self.expr_list(Some(lbrace), elems, 1, COMMA_TERM, Some(rbrace));
                // A block comment before the `}` gets no line end after
                // it, which would need a comma, and no blank unless the
                // literal is empty. The indent is for a comment alone
                // between the braces.
                let mode = if elems.is_empty() {
                    NO_EXTRA_LINEBREAK
                } else {
                    NO_EXTRA_LINEBREAK | NO_EXTRA_BLANK
                };
                self.ws(Ws::Indent);
                self.ws(Ws::Unindent);
                self.toggle(mode);
                self.set_pos(rbrace);
                self.tok(T::RBrace);
                self.toggle(mode);
                self.level -= 1;
            }
            &ExprKind::DotsType { elem } => {
                self.tok(T::Ellipsis);
                self.expr(elem);
            }
            &ExprKind::ArrayType { len, elem } => {
                self.tok(T::LBrack);
                match len {
                    Some(len) => self.expr(len),
                    None => {
                        self.set_pos(cx.find(span.lo + 1, T::Ellipsis));
                        self.tok(T::Ellipsis);
                    }
                }
                self.tok(T::RBrack);
                self.expr(elem);
            }
            &ExprKind::SliceType { elem } => {
                self.tok(T::LBrack);
                self.tok(T::RBrack);
                self.expr(elem);
            }
            ExprKind::StructType { fields } => {
                self.tok(T::Struct);
                self.struct_type(id, fields);
            }
            ExprKind::FuncType { .. } => {
                self.tok(T::Func);
                self.signature(span.lo + 4, &[], id);
            }
            ExprKind::InterfaceType { elems } => {
                self.tok(T::Interface);
                self.interface_type(id, elems);
            }
            &ExprKind::MapType { key, value } => {
                self.tok(T::Map);
                self.tok(T::LBrack);
                self.expr(key);
                self.tok(T::RBrack);
                self.expr(value);
            }
            &ExprKind::ChanType { dir, elem } => {
                match dir {
                    ChanDir::Both => self.tok(T::Chan),
                    ChanDir::Recv => {
                        self.tok(T::Arrow);
                        self.tok(T::Chan);
                    }
                    ChanDir::Send => {
                        self.tok(T::Chan);
                        self.set_pos(cx.find(span.lo + 4, T::Arrow));
                        self.tok(T::Arrow);
                    }
                }
                self.ws(Ws::Blank);
                self.expr(elem);
            }
        }
    }

    // ---- function bodies

    /// The columns from `start_column` to here, if the token at `start` is
    /// on this line; else no limit holds.
    pub(super) fn distance_from(&self, start: u32, start_column: i32) -> i32 {
        if self.pos.line > 0 && self.cx.line(start) == self.pos.line {
            self.out.column - start_column
        } else {
            INFINITY
        }
    }

    /// An estimate of the size of a body on one line; over `max` if it
    /// cannot be on one.
    fn body_size(&mut self, body: &Block, max: i32) -> i32 {
        let cx = self.cx;
        let rbrace = body.span.hi - 1;
        if cx.line(body.span.lo) != cx.line(rbrace) || body.stmts.len() > 5 {
            return max + 1;
        }
        let mut size = self.comment_size_before(cx.pos(rbrace));
        for (index, &stmt) in body.stmts.iter().enumerate() {
            if size > max {
                break;
            }
            if index > 0 {
                size += 2;
            }
            size += self.node_size(Node::Stmt(stmt), max);
        }
        size
    }

    /// The body of a function after a header of `header_size` columns: on
    /// the header's line if both are short and the source has it there.
    pub(super) fn func_body(&mut self, header_size: i32, separator: Ws, body: Option<&'a Block>) {
        let Some(body) = body else { return };
        let level = std::mem::take(&mut self.level);
        const MAX: i32 = 100;
        if header_size.saturating_add(self.body_size(body, MAX)) <= MAX {
            self.ws(separator);
            self.set_pos(body.span.lo);
            self.tok(T::LBrace);
            if !body.stmts.is_empty() {
                self.ws(Ws::Blank);
                for (index, &stmt) in body.stmts.iter().enumerate() {
                    if index > 0 {
                        self.tok(T::Semi);
                        self.ws(Ws::Blank);
                    }
                    self.stmt(stmt, index == body.stmts.len() - 1);
                }
                self.ws(Ws::Blank);
            }
            self.toggle(NO_EXTRA_LINEBREAK);
            self.set_pos(body.span.hi - 1);
            self.tok(T::RBrace);
            self.toggle(NO_EXTRA_LINEBREAK);
        } else {
            if separator != Ws::Ignore {
                self.ws(Ws::Blank);
            }
            self.block(body, true);
        }
        self.level = level;
    }
}
