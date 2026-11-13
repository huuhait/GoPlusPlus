//! An expression as a message shows it: the tree in one spelling, with
//! what is long shortened.

use gopp_syntax::ast::*;
use lasso::ThreadedRodeo;

pub(crate) struct Writer<'a> {
    pub tree: &'a File,
    pub text: &'a str,
    pub names: &'a ThreadedRodeo,
}

impl Writer<'_> {
    pub(crate) fn expr_string(&self, expr: ExprId) -> String {
        let mut out = String::new();
        self.expr(&mut out, expr);
        out
    }

    fn source(&self, expr: ExprId) -> &str {
        let span = self.tree.exprs[expr].span;
        &self.text[span.lo as usize..span.hi as usize]
    }

    fn expr(&self, out: &mut String, expr: ExprId) {
        gopp_syntax::with_stack(|| self.expr_node(out, expr));
    }

    fn expr_node(&self, out: &mut String, expr: ExprId) {
        match &self.tree.exprs[expr].kind {
            ExprKind::Name(name) => out.push_str(self.names.resolve(name)),
            ExprKind::BasicLit { .. } => out.push_str(self.source(expr)),
            ExprKind::FuncLit { ty, .. } => {
                out.push('(');
                self.expr(out, *ty);
                out.push_str(" literal)");
            }
            ExprKind::CompositeLit { ty, elems } => {
                if let Some(ty) = ty {
                    self.expr(out, *ty);
                }
                out.push('{');
                if !elems.is_empty() {
                    out.push('…');
                }
                out.push('}');
            }
            ExprKind::Paren(x) => {
                out.push('(');
                self.expr(out, *x);
                out.push(')');
            }
            ExprKind::Selector { x, sel } => {
                self.expr(out, *x);
                out.push('.');
                out.push_str(self.names.resolve(&sel.name));
            }
            ExprKind::Index { x, indices } => {
                self.expr(out, *x);
                out.push('[');
                self.list(out, indices);
                out.push(']');
            }
            ExprKind::Slice {
                x,
                lo,
                hi,
                max,
                full,
            } => {
                self.expr(out, *x);
                out.push('[');
                self.optional(out, *lo);
                out.push(':');
                self.optional(out, *hi);
                if *full {
                    out.push(':');
                    self.optional(out, *max);
                }
                out.push(']');
            }
            ExprKind::Assert { x, ty } => {
                self.expr(out, *x);
                out.push_str(".(");
                self.expr(out, *ty);
                out.push(')');
            }
            ExprKind::Call {
                fun,
                args,
                spread,
                unwrap,
            } => {
                self.expr(out, *fun);
                if *unwrap {
                    out.push('!');
                }
                out.push('(');
                self.list(out, args);
                if *spread {
                    out.push_str("...");
                }
                out.push(')');
            }
            ExprKind::Unary { op, x } => {
                out.push_str(unary_text(*op));
                self.expr(out, *x);
            }
            ExprKind::Binary { op, x, y } => {
                self.expr(out, *x);
                out.push(' ');
                out.push_str(binary_text(*op));
                out.push(' ');
                self.expr(out, *y);
            }
            ExprKind::ArrayType { len, elem } => {
                out.push('[');
                match len {
                    Some(len) => self.expr(out, *len),
                    None => out.push_str("..."),
                }
                out.push(']');
                self.expr(out, *elem);
            }
            ExprKind::SliceType { elem } => {
                out.push_str("[]");
                self.expr(out, *elem);
            }
            ExprKind::DotsType { elem } => {
                out.push_str("...");
                self.expr(out, *elem);
            }
            ExprKind::StructType { fields } => {
                out.push_str("struct{");
                for (at, field) in fields.iter().enumerate() {
                    if at > 0 {
                        out.push_str("; ");
                    }
                    let names: Vec<&str> = field
                        .names
                        .iter()
                        .map(|name| self.names.resolve(&name.name))
                        .collect();
                    out.push_str(&names.join(", "));
                    if !names.is_empty() {
                        out.push(' ');
                    }
                    self.expr(out, field.ty);
                }
                out.push('}');
            }
            ExprKind::FuncType { .. } => {
                out.push_str("func");
                self.signature(out, expr);
            }
            ExprKind::InterfaceType { elems } => {
                out.push_str("interface{");
                for (at, elem) in elems.iter().enumerate() {
                    if at > 0 {
                        out.push_str("; ");
                    }
                    match elem {
                        InterfaceElem::Method { name, ty } => {
                            out.push_str(self.names.resolve(&name.name));
                            self.signature(out, *ty);
                        }
                        InterfaceElem::Type(ty) => self.expr(out, *ty),
                    }
                }
                out.push('}');
            }
            ExprKind::MapType { key, value } => {
                out.push_str("map[");
                self.expr(out, *key);
                out.push(']');
                self.expr(out, *value);
            }
            ExprKind::ChanType { dir, elem } => {
                out.push_str(match dir {
                    ChanDir::Send => "chan<- ",
                    ChanDir::Recv => "<-chan ",
                    ChanDir::Both => "chan ",
                });
                self.expr(out, *elem);
            }
            // What has no spelling of its own is shown as it was written.
            ExprKind::Bad
            | ExprKind::KeyValue { .. }
            | ExprKind::TypeSwitchGuard { .. }
            | ExprKind::MacroCall { .. } => out.push_str(self.source(expr)),
        }
    }

    fn optional(&self, out: &mut String, expr: Option<ExprId>) {
        if let Some(expr) = expr {
            self.expr(out, expr);
        }
    }

    fn list(&self, out: &mut String, exprs: &[ExprId]) {
        for (at, &expr) in exprs.iter().enumerate() {
            if at > 0 {
                out.push_str(", ");
            }
            self.expr(out, expr);
        }
    }

    /// `(a, b int) (string, error)` of the function type `ty`.
    fn signature(&self, out: &mut String, ty: ExprId) {
        let ExprKind::FuncType {
            params, results, ..
        } = &self.tree.exprs[ty].kind
        else {
            return;
        };
        out.push('(');
        self.params(out, params);
        out.push(')');
        match &results[..] {
            [] => {}
            [one] if one.name.is_none() => {
                out.push(' ');
                self.expr(out, one.ty);
            }
            many => {
                out.push_str(" (");
                self.params(out, many);
                out.push(')');
            }
        }
    }

    /// Parameters that were written with one type, `a, b int`, are written
    /// so again.
    fn params(&self, out: &mut String, params: &[Param]) {
        for (at, param) in params.iter().enumerate() {
            let shares_next = params.get(at + 1).is_some_and(|next| next.ty == param.ty);
            if let Some(name) = param.name {
                out.push_str(self.names.resolve(&name.name));
                out.push_str(if shares_next { ", " } else { " " });
            }
            if !shares_next || param.name.is_none() {
                self.expr(out, param.ty);
                if at + 1 < params.len() {
                    out.push_str(", ");
                }
            }
        }
    }
}

pub(crate) fn unary_text(op: UnaryOp) -> &'static str {
    match op {
        UnaryOp::Plus => "+",
        UnaryOp::Neg => "-",
        UnaryOp::Not => "!",
        UnaryOp::Xor => "^",
        UnaryOp::Star => "*",
        UnaryOp::Addr => "&",
        UnaryOp::Recv => "<-",
        UnaryOp::Tilde => "~",
    }
}

pub(crate) fn binary_text(op: BinaryOp) -> &'static str {
    match op {
        BinaryOp::OrOr => "||",
        BinaryOp::AndAnd => "&&",
        BinaryOp::Eq => "==",
        BinaryOp::Ne => "!=",
        BinaryOp::Lt => "<",
        BinaryOp::Le => "<=",
        BinaryOp::Gt => ">",
        BinaryOp::Ge => ">=",
        BinaryOp::Add => "+",
        BinaryOp::Sub => "-",
        BinaryOp::Or => "|",
        BinaryOp::Xor => "^",
        BinaryOp::Mul => "*",
        BinaryOp::Div => "/",
        BinaryOp::Rem => "%",
        BinaryOp::Shl => "<<",
        BinaryOp::Shr => ">>",
        BinaryOp::And => "&",
        BinaryOp::AndNot => "&^",
    }
}
