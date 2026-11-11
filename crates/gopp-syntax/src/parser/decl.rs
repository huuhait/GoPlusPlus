//! Files and declarations.

use gopp_diag::Code;
use gopp_span::Span;

use super::Parser;
use crate::ast::*;
use crate::constraint::{condition, is_build};
use crate::token::TokenKind as T;
use crate::with_stack;

/// The tokens a top-level declaration starts with.
const TOP_LEVEL: &[T] = &[
    T::Import,
    T::Const,
    T::Type,
    T::Var,
    T::Func,
    T::Macro,
    T::Hash,
    T::At,
];

const MACRO_KINDS: &[&str] = &[
    "expr", "stmt", "block", "type", "ident", "lit", "decl", "tokens",
];

impl Parser<'_> {
    pub(super) fn file(&mut self) -> (Vec<Attribute>, Option<Ident>, Vec<DeclId>) {
        let mut file_attrs = Vec::new();
        while self.at(T::At) {
            let attr = self.attribute();
            self.got(T::Semi);
            if !is_build(&attr, self.text) {
                let message = "only @Build may stand above the package clause";
                self.error_at(Code::Expected, attr.span, message);
            }
            file_attrs.push(attr);
        }
        self.check_build(&file_attrs);
        let package = if self.got(T::Package) {
            let name = self.ident();
            self.want(T::Semi);
            Some(name)
        } else {
            self.syntax_error(Code::Expected, "package statement must be first");
            None
        };

        let mut decls = Vec::new();
        let mut attrs: Vec<Attribute> = Vec::new();
        // Said once for the imports that follow each other.
        let mut after_import = true;
        while !self.at(T::Eof) {
            if self.at(T::Import) && !after_import {
                let message = "imports must appear before other declarations";
                self.syntax_error(Code::ImportAfterDecl, message);
            }
            if !self.at(T::At) {
                after_import = self.at(T::Import);
            }

            let lo = self.lo();
            let first = decls.len();
            let mut takes_attributes = true;
            let mut is_declaration = true;
            match self.kind() {
                T::Import | T::Const | T::Type | T::Var => {
                    let keyword = self.bump().kind;
                    decls.extend(self.decl_group(keyword, lo));
                    let other = attrs.iter().find(|attr| !is_build(attr, self.text));
                    if keyword == T::Import
                        && let Some(other) = other
                    {
                        let message = "the only attribute of an import is @Build";
                        self.error_at(Code::AttrOnImport, other.span, message);
                        attrs.clear();
                    }
                }
                T::Func => {
                    self.bump();
                    decls.push(self.func_decl(lo, false));
                }
                T::Macro => {
                    decls.push(self.macro_decl());
                    takes_attributes = false;
                }
                T::Hash => {
                    let call = self.macro_call();
                    decls.push(self.alloc_decl(DeclKind::MacroCall(call), lo, None));
                    takes_attributes = false;
                }
                T::At => {
                    // LEX-6: the line end after an attribute is a `;`.
                    attrs.push(self.attribute());
                    self.got(T::Semi);
                    continue;
                }
                _ => {
                    let func_without_body = decls.last().is_some_and(|&last| {
                        matches!(self.decls[last].kind, DeclKind::Func { body: None, .. })
                    });
                    if self.at(T::LBrace) && func_without_body {
                        let message = "unexpected semicolon or newline before {";
                        self.syntax_error(Code::NewlineBeforeLbrace, message);
                    } else {
                        let message = "non-declaration statement outside function body";
                        self.syntax_error(Code::StmtOutsideFunc, message);
                    }
                    self.advance(TOP_LEVEL);
                    takes_attributes = false;
                    is_declaration = false;
                }
            }

            if !attrs.is_empty() {
                if takes_attributes && decls.len() > first {
                    self.check_build(&attrs);
                    for &decl in &decls[first..] {
                        self.decls[decl].attrs = attrs.clone();
                    }
                } else if takes_attributes {
                    let message = "an attribute before an empty group has no declaration";
                    self.error_at(Code::Expected, attrs[0].span, message);
                } else {
                    let message = "an attribute must be followed by a const, var, type or func \
                                   declaration";
                    self.error_at(Code::Expected, attrs[0].span, message);
                }
                attrs.clear();
            }
            // A group may be empty, so the number of declarations does not
            // tell whether one was read.
            if is_declaration && !self.at(T::Eof) && !self.got(T::Semi) {
                self.unexpected(Code::Expected, " after top level declaration");
                self.advance(TOP_LEVEL);
            }
        }
        if let Some(attribute) = attrs.first() {
            let message = "an attribute must be followed by a const, var, type or func declaration";
            self.error_at(Code::Expected, attribute.span, message);
        }
        (file_attrs, package, decls)
    }

    /// `@Build` is checked while parsing, before any other attribute means
    /// something: it decides what the rest of the compiler gets to see.
    fn check_build(&mut self, attrs: &[Attribute]) {
        let mut seen = false;
        for attr in attrs.iter().filter(|attr| is_build(attr, self.text)) {
            if seen {
                let message = "@Build is written once; join the conditions with &&";
                self.error_at(Code::BadBuildConstraint, attr.span, message);
            }
            seen = true;
            let not_a_condition = match attr.args.as_deref() {
                Some(&[x]) => condition(&self.exprs, x, &mut |_| true).err(),
                _ => Some(attr.span),
            };
            if let Some(span) = not_a_condition {
                let message = "a build condition is made of names, &&, || and !, as in \
                               @Build(linux && !arm64)";
                self.error_at(Code::BadBuildConstraint, span, message);
            }
        }
    }

    fn alloc_decl(&mut self, kind: DeclKind, lo: u32, group: Option<u32>) -> DeclId {
        let span = self.span_from(lo);
        self.decls.alloc(Decl {
            kind,
            span,
            attrs: Vec::new(),
            group,
        })
    }

    /// The declarations after `import`, `const`, `var` or `type`: one, or
    /// a parenthesized group. `lo` is the start of the keyword.
    pub(super) fn decl_group(&mut self, keyword: T, lo: u32) -> Vec<DeclId> {
        let mut decls = Vec::new();
        if self.got(T::LParen) {
            self.groups += 1;
            let group = Some(self.groups);
            self.list("declaration", T::Semi, T::RParen, |p| {
                let lo = p.lo();
                let kind = p.decl_spec(keyword);
                decls.push(p.alloc_decl(kind, lo, group));
                false
            });
        } else {
            let kind = self.decl_spec(keyword);
            decls.push(self.alloc_decl(kind, lo, None));
        }
        decls
    }

    fn decl_spec(&mut self, keyword: T) -> DeclKind {
        match keyword {
            T::Import => self.import_spec(),
            T::Const => self.const_spec(),
            T::Var => self.var_spec(),
            _ => self.type_spec(),
        }
    }

    fn import_spec(&mut self) -> DeclKind {
        let name = match self.kind() {
            T::Ident => Some(self.ident()),
            T::Dot => {
                let dot = self.bump();
                Some(self.intern(dot))
            }
            _ => None,
        };
        let Some(path) = self.literal() else {
            self.syntax_error(Code::MissingImportPath, "missing import path");
            self.advance(&[T::Semi, T::RParen]);
            let path = self.bad_expr();
            return DeclKind::Import { name, path };
        };
        let node = &mut self.exprs[path];
        if let ExprKind::BasicLit { kind, bad: false } = node.kind
            && kind != LitKind::String
        {
            node.kind = ExprKind::BasicLit { kind, bad: true };
            let span = node.span;
            let message = "syntax error: import path must be a string";
            self.error_at(Code::ImportPathNotString, span, message);
        }
        DeclKind::Import { name, path }
    }

    fn const_spec(&mut self) -> DeclKind {
        let first = self.ident();
        let names = self.ident_list(first);
        let (mut ty, mut values) = (None, Vec::new());
        if !matches!(self.kind(), T::Eof | T::Semi | T::RParen) {
            ty = self.type_or_none();
            if self.got_assign() {
                values = self.expr_list();
            }
        }
        DeclKind::Const { names, ty, values }
    }

    fn var_spec(&mut self) -> DeclKind {
        let first = self.ident();
        let names = self.ident_list(first);
        let (mut ty, mut values) = (None, Vec::new());
        if self.got_assign() {
            values = self.expr_list();
        } else {
            ty = Some(self.type_());
            if self.got_assign() {
                values = self.expr_list();
            }
        }
        DeclKind::Var { names, ty, values }
    }

    /// `=`; a `:=` in its place is taken as one, with an error.
    fn got_assign(&mut self) -> bool {
        if self.at(T::ColonEq) {
            self.expected("=");
            self.bump();
            return true;
        }
        self.got(T::Eq)
    }

    fn type_spec(&mut self) -> DeclKind {
        let name = self.ident();
        let mut type_params = Vec::new();
        let (alias, ty) = if self.at(T::LBrack) {
            // `type T [N]E` or `type T[P any] ...`: only what follows the
            // `[` tells an array length from a type parameter.
            let lo = self.lo();
            self.bump();
            match self.kind() {
                T::Ident => {
                    let first = self.ident();
                    let mut x = self.name_expr(first);
                    if !self.at(T::LBrack) {
                        self.expr_level += 1;
                        let operand = self.primary_expr(Some(x));
                        x = self.binary_expr(Some(operand), 0);
                        self.expr_level -= 1;
                    }
                    match self.split_type_param(x, self.at(T::Comma)) {
                        Some((param, bound)) if bound.is_some() || !self.at(T::RBrack) => {
                            type_params = self.type_param_list(Some(param), bound);
                            self.type_definition()
                        }
                        _ => (false, Some(self.array_type(lo, Some(x)))),
                    }
                }
                T::RBrack => {
                    self.bump();
                    (false, Some(self.slice_type(lo)))
                }
                _ => (false, Some(self.array_type(lo, None))),
            }
        } else {
            self.type_definition()
        };
        let ty = ty.unwrap_or_else(|| {
            let bad = self.bad_expr();
            self.unexpected(Code::Expected, " in type declaration");
            self.advance(&[T::Semi, T::RParen]);
            bad
        });
        DeclKind::Type {
            name,
            type_params,
            alias,
            ty,
        }
    }

    /// What follows the name and type parameters of a type declaration:
    /// `= Type` or `Type`. Returns the alias mark and the type.
    fn type_definition(&mut self) -> (bool, Option<ExprId>) {
        let alias = self.got_assign();
        (alias, self.type_or_none())
    }

    /// Splits `x`, read as an expression after `type T[`, into a type
    /// parameter name and its constraint: `P *C` was read as a product and
    /// `P C|D` as a union. Without `force` the split needs a part that can
    /// only be a type.
    fn split_type_param(&mut self, x: ExprId, force: bool) -> Option<(Ident, Option<ExprId>)> {
        let span = self.exprs[x].span;
        match self.exprs[x].kind {
            ExprKind::Name(name) => Some((Ident { name, span }, None)),
            ExprKind::Binary {
                op: BinaryOp::Mul,
                x: left,
                y,
            } => {
                let ExprKind::Name(name) = self.exprs[left].kind else {
                    return None;
                };
                if !force && !self.is_type_elem(y) {
                    return None;
                }
                let name = Ident {
                    name,
                    span: self.exprs[left].span,
                };
                let between = &self.text[name.span.hi as usize..self.exprs[y].span.lo as usize];
                let star = name.span.hi + between.find('*').unwrap_or(0) as u32;
                let pointer = self.exprs.alloc(Expr {
                    kind: ExprKind::Unary {
                        op: UnaryOp::Star,
                        x: y,
                    },
                    span: Span { lo: star, ..span },
                });
                Some((name, Some(pointer)))
            }
            ExprKind::Binary {
                op: BinaryOp::Or,
                x: left,
                y,
            } => {
                let force = force || self.is_type_elem(y);
                let (name, bound) = with_stack(|| self.split_type_param(left, force))?;
                let bound = bound?;
                let lo = self.exprs[bound].span.lo;
                let union = self.exprs.alloc(Expr {
                    kind: ExprKind::Binary {
                        op: BinaryOp::Or,
                        x: bound,
                        y,
                    },
                    span: Span { lo, ..span },
                });
                Some((name, Some(union)))
            }
            ExprKind::Call {
                fun,
                ref args,
                spread: false,
                unwrap: false,
            } if args.len() == 1 => {
                let arg = args[0];
                let ExprKind::Name(name) = self.exprs[fun].kind else {
                    return None;
                };
                let name = Ident {
                    name,
                    span: self.exprs[fun].span,
                };
                (force || self.is_type_elem(arg)).then_some((name, Some(arg)))
            }
            _ => None,
        }
    }

    /// Whether `x` can only be a type or a type set, never a value.
    fn is_type_elem(&self, x: ExprId) -> bool {
        match self.exprs[x].kind {
            ExprKind::ArrayType { .. }
            | ExprKind::StructType { .. }
            | ExprKind::FuncType { .. }
            | ExprKind::InterfaceType { .. }
            | ExprKind::SliceType { .. }
            | ExprKind::MapType { .. }
            | ExprKind::ChanType { .. } => true,
            ExprKind::Unary { op, x } => {
                op == UnaryOp::Tilde || with_stack(|| self.is_type_elem(x))
            }
            ExprKind::Binary { x, y, .. } => {
                with_stack(|| self.is_type_elem(x) || self.is_type_elem(y))
            }
            ExprKind::Paren(x) => with_stack(|| self.is_type_elem(x)),
            _ => false,
        }
    }

    /// A function or method after `func`; `lo` is where the declaration
    /// starts.
    fn func_decl(&mut self, lo: u32, is_macro: bool) -> DeclId {
        let mut receiver = None;
        let mut context = "";
        if self.at(T::LParen) {
            context = "method";
            let at = self.here();
            self.bump();
            let receivers = self.param_list(T::RParen);
            match receivers.last() {
                None => self.error_at(Code::ReceiverMissing, at, "method has no receiver"),
                Some(last) if receivers.len() > 1 => {
                    let at = last.name.map_or(self.exprs[last.ty].span, |name| name.span);
                    self.error_at(Code::ReceiverCount, at, "method has multiple receivers");
                }
                Some(_) => {}
            }
            receiver = receivers.into_iter().next();
        }

        let (name, type_params, ty) = if self.at(T::Ident) {
            let name = self.ident();
            let (type_params, ty) = self.func_type(context, lo);
            (name, type_params, ty)
        } else {
            let name = self.blank_ident();
            self.expected(if receiver.is_some() {
                "name"
            } else {
                "name or ("
            });
            self.advance(&[T::LBrace, T::Semi]);
            let ty = ExprKind::FuncType {
                params: Vec::new(),
                results: Vec::new(),
                fails: false,
            };
            (name, Vec::new(), self.alloc_expr(ty, lo))
        };
        if is_macro {
            self.check_macro_name(name);
        }
        let body = self.at(T::LBrace).then(|| self.func_body());
        let kind = DeclKind::Func {
            is_macro,
            receiver,
            name,
            type_params,
            ty,
            body,
        };
        self.alloc_decl(kind, lo, None)
    }

    /// `macro Name(p kind, ...) { ... }` or `macro func Name(...) ...`, at
    /// the `macro`.
    fn macro_decl(&mut self) -> DeclId {
        let lo = self.lo();
        self.bump();
        if self.got(T::Func) {
            return self.func_decl(lo, true);
        }
        let name = self.ident();
        self.check_macro_name(name);
        let mut params = Vec::new();
        self.want(T::LParen);
        self.list("macro parameter list", T::Comma, T::RParen, |p| {
            let name = p.ident();
            let variadic = p.got(T::Ellipsis);
            // The fragment kind `type` is a keyword everywhere else.
            let kind = if p.at(T::Type) {
                let token = p.bump();
                p.intern(token)
            } else {
                p.ident()
            };
            let text = &p.text[kind.span.lo as usize..kind.span.hi as usize];
            if !MACRO_KINDS.contains(&text) {
                let message = format!(
                    "unknown macro fragment kind {text}; the kinds are {}",
                    MACRO_KINDS.join(", ")
                );
                p.error_at(Code::UnknownMacroKind, kind.span, message);
            }
            params.push(MacroParam {
                name,
                variadic,
                kind,
            });
            false
        });
        let body = self.func_body();
        self.alloc_decl(DeclKind::Macro { name, params, body }, lo, None)
    }
}
