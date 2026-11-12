//! Types, parameter lists, struct and interface bodies.

use gopp_diag::Code;
use gopp_span::Span;

use super::Parser;
use crate::ast::*;
use crate::constraint::is_build;
use crate::token::TokenKind as T;
use crate::with_stack;

/// A parameter before its group is resolved: `a, b int` gives `a` no type yet.
struct RawParam {
    name: Option<Ident>,
    ty: Option<ExprId>,
}

impl Parser<'_> {
    /// A type; if there is none, an error and a `Bad` node.
    pub(super) fn type_(&mut self) -> ExprId {
        if let Some(ty) = self.type_or_none() {
            return ty;
        }
        let bad = self.bad_expr();
        if self.at(T::Hash) {
            let message = "a macro invocation cannot stand where a type is expected";
            self.syntax_error(Code::Expected, message);
        } else {
            self.expected("type");
        }
        self.advance(&[T::Comma, T::Colon, T::Semi, T::RParen, T::RBrack, T::RBrace]);
        bad
    }

    pub(super) fn type_or_none(&mut self) -> Option<ExprId> {
        if !self.enter(Code::NestingTooDeep, "type") {
            return Some(self.bad_expr());
        }
        let ty = with_stack(|| self.type_node());
        self.leave();
        ty
    }

    fn type_node(&mut self) -> Option<ExprId> {
        let lo = self.lo();
        Some(match self.kind() {
            T::Star => {
                self.bump();
                let x = self.type_();
                self.alloc_expr(
                    ExprKind::Unary {
                        op: UnaryOp::Star,
                        x,
                    },
                    lo,
                )
            }
            T::Arrow => {
                self.bump();
                self.want(T::Chan);
                let elem = self.chan_elem();
                let dir = ChanDir::Recv;
                self.alloc_expr(ExprKind::ChanType { dir, elem }, lo)
            }
            T::Func => {
                self.bump();
                self.func_type("function type", lo).1
            }
            T::LBrack => {
                self.bump();
                if self.got(T::RBrack) {
                    self.slice_type(lo)
                } else {
                    self.array_type(lo, None)
                }
            }
            T::Chan => {
                self.bump();
                let dir = if self.got(T::Arrow) {
                    ChanDir::Send
                } else {
                    ChanDir::Both
                };
                let elem = self.chan_elem();
                self.alloc_expr(ExprKind::ChanType { dir, elem }, lo)
            }
            T::Map => {
                self.bump();
                self.want(T::LBrack);
                let key = self.type_();
                self.want(T::RBrack);
                let value = self.type_();
                self.alloc_expr(ExprKind::MapType { key, value }, lo)
            }
            T::Struct => self.struct_type(),
            T::Interface => self.interface_type(),
            T::Ident => self.qualified_name(None),
            T::LParen => {
                self.bump();
                let ty = self.type_();
                self.want(T::RParen);
                self.alloc_expr(ExprKind::Paren(ty), lo)
            }
            _ => return None,
        })
    }

    pub(super) fn slice_type(&mut self, lo: u32) -> ExprId {
        let elem = self.type_();
        self.alloc_expr(ExprKind::SliceType { elem }, lo)
    }

    /// The rest of an array type after `[`, or after `[ len` when the
    /// length is already parsed.
    pub(super) fn array_type(&mut self, lo: u32, len: Option<ExprId>) -> ExprId {
        let len = match len {
            Some(len) => Some(len),
            None if self.got(T::Ellipsis) => None,
            None => {
                self.expr_level += 1;
                let len = self.expr();
                self.expr_level -= 1;
                Some(len)
            }
        };
        if self.at(T::Comma) {
            // A type parameter list may end in a comma; an array length may not.
            self.syntax_error(Code::UnexpectedCommaRbrack, "unexpected comma; expecting ]");
            self.bump();
        }
        self.want(T::RBrack);
        let elem = self.type_();
        self.alloc_expr(ExprKind::ArrayType { len, elem }, lo)
    }

    fn chan_elem(&mut self) -> ExprId {
        self.type_or_none().unwrap_or_else(|| {
            let bad = self.bad_expr();
            self.syntax_error(Code::Expected, "missing channel element type");
            bad
        })
    }

    /// `Name`, `pkg.Name`, and either with type arguments.
    pub(super) fn qualified_name(&mut self, name: Option<Ident>) -> ExprId {
        let name = match name {
            Some(name) => name,
            None if self.at(T::Ident) => self.ident(),
            None => {
                let blank = self.blank_ident();
                self.expected("name");
                self.advance(&[T::Dot, T::Semi, T::RBrace]);
                blank
            }
        };
        let lo = name.span.lo;
        let mut x = self.name_expr(name);
        if self.got(T::Dot) {
            let sel = self.ident();
            x = self.alloc_expr(ExprKind::Selector { x, sel }, lo);
        }
        if self.at(T::LBrack) {
            self.bump();
            x = self.type_instance(x);
        }
        x
    }

    /// `x[T, ...]` after the `[`.
    fn type_instance(&mut self, x: ExprId) -> ExprId {
        let lo = self.expr_lo(x);
        let indices = if self.at(T::RBrack) {
            self.expected("type argument list");
            vec![self.bad_expr()]
        } else {
            self.type_list(true).0
        };
        self.want(T::RBrack);
        self.alloc_expr(ExprKind::Index { x, indices }, lo)
    }

    /// A comma-separated list of types that may end in a comma; without
    /// `strict` the first may be any expression, and in a Go++ file every
    /// one: `m[i, j+1]` is an index list (EXP-22). Says whether a comma
    /// was seen.
    pub(super) fn type_list(&mut self, strict: bool) -> (Vec<ExprId>, bool) {
        self.expr_level += 1;
        let mut list = vec![if strict { self.type_() } else { self.expr() }];
        let mut comma = false;
        while self.got(T::Comma) {
            comma = true;
            if !strict && self.gopp {
                if self.at(T::RBrack) {
                    break;
                }
                list.push(self.expr());
                continue;
            }
            match self.type_or_none() {
                Some(ty) => list.push(ty),
                None => break,
            }
        }
        self.expr_level -= 1;
        (list, comma)
    }

    /// After a name and `[`-ahead: a slice type, an array type, or the
    /// index part of an instantiation. Returns `Err(indices)` for the last.
    fn array_or_type_args(&mut self) -> Result<ExprId, Vec<ExprId>> {
        let lo = self.lo();
        self.want(T::LBrack);
        if self.got(T::RBrack) {
            return Ok(self.slice_type(lo));
        }
        let (mut list, comma) = self.type_list(false);
        self.want(T::RBrack);
        if !comma && let Some(elem) = self.type_or_none() {
            let len = list.pop();
            return Ok(self.alloc_expr(ExprKind::ArrayType { len, elem }, lo));
        }
        Err(list)
    }

    /// The type of a function after `func` and, for a declaration, its
    /// name: optional type parameters, `!`, parameters and results.
    /// `context` names a form that takes no type parameters.
    pub(super) fn func_type(&mut self, context: &str, lo: u32) -> (Vec<TypeParam>, ExprId) {
        let mut type_params = Vec::new();
        if self.at(T::LBrack) {
            let code = match context {
                "function type" => Some(Code::FuncTypeTypeParams),
                "interface method" => Some(Code::InterfaceMethodTypeParams),
                "method" if !self.gopp => Some(Code::MethodTypeParams),
                _ => None,
            };
            if let Some(code) = code {
                let message = format!("{context} must have no type parameters");
                self.syntax_error(code, &message);
            }
            self.bump();
            if self.at(T::RBrack) {
                self.syntax_error(Code::EmptyTypeParams, "empty type parameter list");
                self.bump();
            } else {
                type_params = self.type_param_list(None, None);
            }
        }
        let fails = self.gopp && self.got(T::Bang);
        self.want(T::LParen);
        let params = self.param_list(T::RParen);
        let results = self.func_result();
        let ty = self.alloc_expr(
            ExprKind::FuncType {
                params,
                results,
                fails,
            },
            lo,
        );
        (type_params, ty)
    }

    fn func_result(&mut self) -> Vec<Param> {
        if self.got(T::LParen) {
            return self.param_list(T::RParen);
        }
        match self.type_or_none() {
            Some(ty) => vec![Param { name: None, ty }],
            None => Vec::new(),
        }
    }

    /// Parameters up to `close`, after the opening token.
    pub(super) fn param_list(&mut self, close: T) -> Vec<Param> {
        let raw = self.raw_param_list(None, None, close, false);
        raw.into_iter()
            .filter_map(|param| {
                Some(Param {
                    name: param.name,
                    ty: param.ty?,
                })
            })
            .collect()
    }

    /// Type parameters up to `]`, after the `[`. `name` and `ty` are the
    /// start of the first one when the caller had to read them to know that
    /// this is a type parameter list.
    pub(super) fn type_param_list(
        &mut self,
        name: Option<Ident>,
        ty: Option<ExprId>,
    ) -> Vec<TypeParam> {
        let raw = self.raw_param_list(name, ty, T::RBrack, true);
        raw.into_iter()
            .filter_map(|param| {
                Some(TypeParam {
                    name: param.name?,
                    bound: param.ty?,
                })
            })
            .collect()
    }

    /// Go's rule for a parameter list: either every parameter has a name
    /// or none has, and a name without a type takes the type that follows.
    fn raw_param_list(
        &mut self,
        mut name: Option<Ident>,
        mut ty: Option<ExprId>,
        close: T,
        require_names: bool,
    ) -> Vec<RawParam> {
        let mut list: Vec<RawParam> = Vec::new();
        if name.is_some() && ty.is_some() && self.at(close) {
            self.bump();
            return vec![RawParam { name, ty }];
        }
        let context = if require_names {
            "type parameter list"
        } else {
            "parameter list"
        };
        let end = self.list(context, T::Comma, close, |p| {
            let param = match ty.take() {
                Some(ty) => Some(RawParam {
                    name: name.take(),
                    ty: Some(ty),
                }),
                None => p.param_decl(name.take(), close),
            };
            list.extend(param);
            false
        });
        let named = list
            .iter()
            .filter(|p| p.name.is_some() && p.ty.is_some())
            .count();
        let typed = list.iter().filter(|p| p.ty.is_some()).count();
        if list.is_empty() {
            return list;
        }

        if named == 0 && !require_names {
            // No parameter is named, so every lone name is a type.
            for param in &mut list {
                if let Some(name) = param.name.take() {
                    param.ty = Some(self.name_expr(name));
                }
            }
        } else if named != list.len() {
            let mut error_at = None;
            let mut current = None;
            for param in list.iter_mut().rev() {
                if let Some(ty) = param.ty {
                    current = Some(ty);
                    if param.name.is_none() {
                        let span = self.exprs[ty].span;
                        error_at = Some(span.lo);
                        param.name = Some(Ident {
                            name: self.names.get_or_intern_static("_"),
                            span: Span {
                                hi: span.lo,
                                ..span
                            },
                        });
                    }
                } else if let Some(ty) = current {
                    param.ty = Some(ty);
                } else if let Some(name) = param.name {
                    error_at = Some(name.span.lo);
                    param.ty = Some(self.exprs.alloc(Expr {
                        kind: ExprKind::Bad,
                        span: name.span,
                    }));
                }
            }
            if let Some(mut at) = error_at {
                // With as many typed as named parameters, the ones without a
                // type are at the end of the list.
                let (code, message) = match (named == typed, require_names) {
                    (true, true) => (Code::MissingTypeConstraint, "missing type constraint"),
                    (true, false) => (Code::MissingParamType, "missing parameter type"),
                    (false, true) if list.len() == 1 => (
                        Code::MissingTypeParamNameOrLen,
                        "missing type parameter name or invalid array length",
                    ),
                    (false, true) => (Code::MissingTypeParamName, "missing type parameter name"),
                    (false, false) => (Code::MissingParamName, "missing parameter name"),
                };
                if named == typed {
                    at = end;
                }
                let span = Span {
                    file: self.id,
                    lo: at,
                    hi: at,
                };
                self.error_at(code, span, format!("syntax error: {message}"));
            }
        }
        list
    }

    /// One entry of a parameter list: `name`, `Type`, `name Type`,
    /// `name ...Type`, and in a type parameter list also a type set.
    fn param_decl(&mut self, name: Option<Ident>, close: T) -> Option<RawParam> {
        let type_sets = close == T::RBrack;
        let mut param = RawParam {
            name: None,
            ty: None,
        };
        if name.is_none() && type_sets && self.at(T::Tilde) {
            param.ty = Some(self.type_set(None));
            return Some(param);
        }

        if self.at(T::Ident) || name.is_some() {
            let name = match name {
                Some(name) => name,
                None => self.ident(),
            };
            if self.at(T::LBrack) {
                // `name [n]E` is a named parameter, `name[T]` a type.
                let ty = match self.array_or_type_args() {
                    Ok(ty) => {
                        param.name = Some(name);
                        ty
                    }
                    Err(indices) => {
                        let x = self.name_expr(name);
                        self.alloc_expr(ExprKind::Index { x, indices }, name.span.lo)
                    }
                };
                param.ty = Some(self.type_set_tail(ty, type_sets));
                return Some(param);
            }
            if self.at(T::Dot) {
                let ty = self.qualified_name(Some(name));
                param.ty = Some(self.type_set_tail(ty, type_sets));
                return Some(param);
            }
            if type_sets && self.at(T::Pipe) {
                let ty = self.name_expr(name);
                param.ty = Some(self.type_set(Some(ty)));
                return Some(param);
            }
            param.name = Some(name);
        }

        if self.at(T::Ellipsis) {
            let lo = self.lo();
            self.bump();
            let elem = self.type_or_none().unwrap_or_else(|| {
                let bad = self.bad_expr();
                self.syntax_error(Code::DotsInvalid, "... is missing type");
                bad
            });
            param.ty = Some(self.alloc_expr(ExprKind::DotsType { elem }, lo));
            return Some(param);
        }
        if type_sets && self.at(T::Tilde) {
            param.ty = Some(self.type_set(None));
            return Some(param);
        }
        param.ty = self.type_or_none();
        if let Some(ty) = param.ty {
            param.ty = Some(self.type_set_tail(ty, type_sets));
        }
        if param.name.is_some() || param.ty.is_some() {
            return Some(param);
        }
        self.expected(super::fixed_text(close));
        self.advance(&[T::Comma, close]);
        None
    }

    fn type_set_tail(&mut self, first: ExprId, type_sets: bool) -> ExprId {
        if type_sets && self.at(T::Pipe) {
            self.type_set(Some(first))
        } else {
            first
        }
    }

    /// `~T | U | ...`; `first` is a term that is already parsed.
    fn type_set(&mut self, first: Option<ExprId>) -> ExprId {
        let mut x = match first {
            Some(first) => first,
            None => self.type_term(),
        };
        while self.got(T::Pipe) {
            let y = self.type_term();
            let op = BinaryOp::Or;
            x = self.alloc_expr(ExprKind::Binary { op, x, y }, self.expr_lo(x));
        }
        x
    }

    fn type_term(&mut self) -> ExprId {
        let lo = self.lo();
        if self.got(T::Tilde) {
            let x = self.type_();
            let op = UnaryOp::Tilde;
            return self.alloc_expr(ExprKind::Unary { op, x }, lo);
        }
        self.type_or_none().unwrap_or_else(|| {
            let bad = self.bad_expr();
            self.expected("~ term or type");
            self.advance(&[T::Pipe, T::Semi, T::RParen, T::RBrack, T::RBrace]);
            bad
        })
    }

    fn struct_type(&mut self) -> ExprId {
        let lo = self.lo();
        self.want(T::Struct);
        self.want(T::LBrace);
        let mut fields = Vec::new();
        self.list("struct type", T::Semi, T::RBrace, |p| {
            fields.extend(p.field_decl());
            false
        });
        self.alloc_expr(ExprKind::StructType { fields }, lo)
    }

    fn field_decl(&mut self) -> Option<Field> {
        let lo = self.lo();
        let (names, ty) = match self.kind() {
            T::Ident => {
                let name = self.ident();
                if matches!(
                    self.kind(),
                    T::Dot | T::String | T::Semi | T::RBrace | T::At
                ) {
                    (Vec::new(), self.qualified_name(Some(name)))
                } else {
                    let names = self.ident_list(name);
                    if names.len() == 1 && self.at(T::LBrack) {
                        // `T[P]` embeds an instantiated type; `f [n]E` is a field.
                        match self.array_or_type_args() {
                            Ok(ty) => (names, ty),
                            Err(indices) => {
                                let x = self.name_expr(name);
                                let kind = ExprKind::Index { x, indices };
                                (Vec::new(), self.alloc_expr(kind, lo))
                            }
                        }
                    } else {
                        (names, self.type_())
                    }
                }
            }
            T::Star => {
                self.bump();
                let parenthesized = self.at(T::LParen);
                if parenthesized {
                    let message = "cannot parenthesize embedded type";
                    self.syntax_error(Code::ParenEmbeddedType, message);
                    self.bump();
                }
                let x = self.qualified_name(None);
                if parenthesized {
                    self.got(T::RParen);
                }
                let op = UnaryOp::Star;
                (Vec::new(), self.alloc_expr(ExprKind::Unary { op, x }, lo))
            }
            T::LParen => {
                self.syntax_error(Code::ParenEmbeddedType, "cannot parenthesize embedded type");
                self.bump();
                let ty = if self.at(T::Star) {
                    let star = self.lo();
                    self.bump();
                    let x = self.qualified_name(None);
                    let op = UnaryOp::Star;
                    self.alloc_expr(ExprKind::Unary { op, x }, star)
                } else {
                    self.qualified_name(None)
                };
                self.got(T::RParen);
                (Vec::new(), ty)
            }
            _ => {
                self.expected("field name or embedded type");
                self.advance(&[T::Semi, T::RBrace]);
                return None;
            }
        };
        if names.is_empty() && self.gopp {
            let span = self.span_from(lo);
            let message = "struct embedding is not part of Go++: use `@Extends` to inherit from \
                           a struct, or give the field a name";
            self.error_at(Code::StructEmbedding, span, message);
        }
        let mut attrs = Vec::new();
        while self.gopp && self.at(T::At) {
            let attr = self.attribute();
            if is_build(&attr, self.text) {
                let message = "@Build is for a file or a top level declaration, not for a field";
                self.error_at(Code::BadBuildConstraint, attr.span, message);
            }
            attrs.push(attr);
        }
        let tag = self.literal();
        Some(Field {
            names,
            ty,
            attrs,
            tag,
            span: self.span_from(lo),
        })
    }

    fn interface_type(&mut self) -> ExprId {
        let lo = self.lo();
        self.want(T::Interface);
        self.want(T::LBrace);
        let mut elems = Vec::new();
        self.list("interface type", T::Semi, T::RBrace, |p| {
            elems.push(p.interface_elem());
            false
        });
        self.alloc_expr(ExprKind::InterfaceType { elems }, lo)
    }

    fn interface_elem(&mut self) -> InterfaceElem {
        if !self.at(T::Ident) {
            return InterfaceElem::Type(self.type_set(None));
        }
        let lo = self.lo();
        let name = self.ident();
        let ty = match self.kind() {
            T::LParen => return self.method_elem(name),
            T::Bang if self.gopp && self.peek(1) == T::LParen => return self.method_elem(name),
            T::LBrack => {
                // A generic method `m[T C](x T)` (an error) or an embedded
                // instantiated type `T[P1, P2]`.
                let bracket = self.here();
                self.bump();
                if self.at(T::RBrack) {
                    let empty = self.here();
                    self.bump();
                    if self.at(T::LParen) {
                        let message = "syntax error: empty type parameter list";
                        self.error_at(Code::EmptyTypeParams, empty, message);
                        return self.method_elem(name);
                    }
                    let message = "syntax error: empty type argument list";
                    self.error_at(Code::EmptyTypeParams, empty, message);
                    self.name_expr(name)
                } else {
                    let list = self.raw_param_list(None, None, T::RBrack, false);
                    if list.first().is_some_and(|param| param.name.is_some()) {
                        let message = "syntax error: interface method must have no type parameters";
                        let elem = self.method_elem(name);
                        self.error_at(Code::InterfaceMethodTypeParams, bracket, message);
                        return elem;
                    }
                    if list.is_empty() && self.at(T::LParen) {
                        return self.method_elem(name);
                    }
                    let x = self.name_expr(name);
                    if list.is_empty() {
                        x
                    } else {
                        let indices = list.into_iter().filter_map(|param| param.ty).collect();
                        self.alloc_expr(ExprKind::Index { x, indices }, lo)
                    }
                }
            }
            _ => self.qualified_name(Some(name)),
        };
        InterfaceElem::Type(self.type_set(Some(ty)))
    }

    fn method_elem(&mut self, name: Ident) -> InterfaceElem {
        let (_, ty) = self.func_type("interface method", name.span.lo);
        InterfaceElem::Method { name, ty }
    }
}
