//! Constraints: what may be a term of a union, after `go/types`
//! (union.go), with Go's messages.

use super::*;
use crate::types::Marker;

/// A union has no more terms than this, as in Go.
const MAX_TERMS: usize = 100;

impl<'a> Checker<'a> {
    /// `~p.T`: a term as a message shows it, which is with the package
    /// of its type.
    fn term_string(&self, term: Term) -> String {
        let tilde = if term.tilde { "~" } else { "" };
        format!("{tilde}{}", self.types.display(term.ty, self.names))
    }

    /// The type `ty` of the term that `expr` writes, which is no type
    /// parameter. `tilde` is the whole term if it is written with `~`.
    pub(super) fn no_param_term(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        ty: TypeId,
        tilde: Option<ExprId>,
    ) -> TypeId {
        if !matches!(&*self.types.get(ty), Type::Param(_)) {
            return ty;
        }
        let message = match tilde {
            Some(term) => format!(
                "type in term {} cannot be a type parameter",
                self.expr_string(cx, term)
            ),
            None => "term cannot be a type parameter".to_owned(),
        };
        self.error(Code::UnionTypeParam, cx.tree.exprs[expr].span, message);
        Types::INVALID
    }

    /// The terms of the union that `expr` writes, each with where it is
    /// written.
    pub(super) fn terms(&mut self, cx: &Cx<'a>, expr: ExprId, terms: &mut Vec<(Term, Span)>) {
        let span = cx.tree.exprs[expr].span;
        let (tilde, of) = match cx.tree.exprs[expr].kind {
            ExprKind::Binary {
                op: BinaryOp::Or,
                x,
                y,
            } => {
                self.terms(cx, x, terms);
                return self.terms(cx, y, terms);
            }
            ExprKind::Unary {
                op: UnaryOp::Tilde,
                x,
            } => (true, x),
            _ => (false, expr),
        };
        let ty = self.typ(cx, of);
        let ty = self.no_param_term(cx, of, ty, tilde.then_some(expr));
        if terms.len() == MAX_TERMS {
            let message = format!(
                "cannot handle more than {MAX_TERMS} union terms (implementation limitation)"
            );
            self.error(Code::UnionTooManyTerms, span, message);
        }
        terms.push((Term { tilde, ty }, span));
    }

    /// What is wrong with the terms of a union: `~` on a type that is no
    /// underlying type, an interface that is more than a set of types,
    /// two terms that share a type.
    pub(super) fn union_terms(&mut self, terms: &[(Term, Span)]) {
        // The terms after the limit were reported.
        let terms = &terms[..terms.len().min(MAX_TERMS)];
        // One type without `~` is no union: it is what it is alone.
        if let [(Term { tilde: false, .. }, _)] = terms {
            return;
        }
        for (at, &(term, span)) in terms.iter().enumerate() {
            let under = self.types.underlying(term.ty);
            if under == Types::INVALID {
                continue;
            }
            let shown = self.term_string(term);
            let named = self.type_string(term.ty);
            let is_interface = self.types.is_interface(term.ty);
            let marker = match &*self.types.get(term.ty) {
                Type::Marker(marker) => Some(*marker),
                _ => None,
            };
            let (code, message) = if term.tilde && (is_interface || marker.is_some()) {
                let message = format!("invalid use of ~ ({named} is an interface)");
                (Code::UnionTildeInterface, message)
            } else if term.tilde && under != term.ty {
                let message = format!(
                    "invalid use of ~ (underlying type of {named} is {})",
                    self.type_string(under)
                );
                (Code::UnionTildeUnderlying, message)
            } else if marker.is_some() {
                let message = format!("cannot use {named} in union");
                (Code::UnionComparable, message)
            } else if is_interface {
                // An interface in a union is a set of types and no more.
                let set = self.types.type_set(term.ty).unwrap_or_default();
                if !set.methods.is_empty() {
                    let message = format!("cannot use {shown} in union ({shown} contains methods)");
                    (Code::UnionWithInterface, message)
                } else if set.comparable || set.markers.contains(&Marker::Comparable) {
                    let message =
                        format!("cannot use {shown} in union ({shown} embeds comparable)");
                    (Code::UnionComparable, message)
                } else {
                    continue;
                }
            } else {
                // A term before this one that shares a type with it.
                let plain = |ty: TypeId| !self.types.is_interface(ty);
                let earlier = terms[..at].iter().map(|&(other, _)| other);
                let mut overlapping = earlier.filter(|other| {
                    let valid = self.types.underlying(other.ty) != Types::INVALID;
                    valid && plain(other.ty) && !self.types.disjoint(*other, term)
                });
                let Some(other) = overlapping.next() else {
                    continue;
                };
                let message = format!("overlapping terms {shown} and {}", self.term_string(other));
                (Code::UnionOverlapping, message)
            };
            self.error(code, span, message);
        }
    }
}
