//! Constraints: what may be a term of a union, after `go/types`
//! (union.go), with Go's messages.

use super::implements::Cause;
use super::*;
use crate::types::Marker;

/// A generic type or function with type arguments, which are held to the
/// constraints once every type is known.
#[derive(Clone)]
pub(super) struct Instance {
    pub params: Vec<TypeId>,
    pub args: Vec<TypeId>,
    /// Where each type argument that is written is.
    pub spans: Vec<Span>,
    /// Where the instance is, for a type argument that is not written.
    pub at: Span,
    /// Written in the signature of a method of a Go file, where Go does
    /// not look for instantiation cycles.
    pub skip_cycle: bool,
}

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

    /// Whether a part of what a union or an interface is written with is
    /// the type `ty`, in its type set or not.
    fn mentions(&self, constraint: TypeId, ty: TypeId) -> bool {
        match &*self.types.get(constraint) {
            Type::Interface { embedded, .. } => {
                embedded.iter().any(|&elem| self.mentions(elem, ty))
            }
            Type::Union(terms) => terms.iter().any(|term| self.mentions(term.ty, ty)),
            _ => self.types.identical(constraint, ty),
        }
    }

    /// Why the type `v` does not satisfy the constraint `bound`. `None`
    /// if it does, and where that is not decided here: a type in error,
    /// a constraint of Go++. With `elements`, a method of the constraint
    /// may be met as an operator element (GEN-13).
    pub(super) fn unsatisfied(&self, v: TypeId, bound: TypeId, elements: bool) -> Option<Cause> {
        let invalid = |ty: TypeId| self.types.underlying(ty) == Types::INVALID;
        let pointee_invalid = matches!(
            &*self.types.get(self.types.underlying(v)),
            Type::Pointer(elem) if invalid(*elem)
        );
        if invalid(v) || invalid(bound) || pointee_invalid {
            return None;
        }
        let wanted = self.types.type_set(bound)?;
        let (shown, constraint) = (self.type_string(v), self.type_string(bound));
        let cause = |text: String| {
            Some(Cause {
                text,
                have_want: None,
            })
        };
        // A constraint with `@Extends` asks for its parents among the
        // ancestors of the type (GEN-12).
        for parent in self.types.parents(bound) {
            if self.types.ancestor_path(v, parent).is_none() {
                return cause(format!(
                    "{shown} does not satisfy {constraint} ({shown} does not extend {})",
                    self.type_string(parent)
                ));
            }
        }
        // Any type satisfies a constraint that asks nothing.
        if wanted.constraint().is_none() && wanted.methods.is_empty() {
            return None;
        }
        // What is known of `v` if it stands for other types: a type
        // parameter, or an interface.
        let is_param = matches!(&*self.types.get(v), Type::Param(_));
        let own = match is_param {
            true => Some(self.types.param_set(v)?),
            false if self.types.is_interface(v) => Some(self.types.type_set(v)?),
            false => None,
        };
        if own.as_ref().is_some_and(|own| own.is_empty()) {
            return None;
        }
        if wanted.is_empty() {
            return cause(format!("cannot satisfy {constraint} (empty type set)"));
        }
        let mut same = |a, b| self.types.identical(a, b);
        let mut met = |method: &Method, same: &mut dyn FnMut(TypeId, TypeId) -> bool| {
            self.element_met(v, method, same).filter(|_| elements)
        };
        let lacks = self
            .types
            .lacks_with(v, &wanted.methods, self.names, &mut same, &mut met);
        if let Some((method, lacks)) = lacks {
            let mut why = self.lacks_cause(v, &method, lacks);
            why.text = format!("{shown} does not satisfy {constraint} {}", why.text);
            return Some(why);
        }
        // The values of a type compare if they may panic doing so, but
        // those of a type parameter only if they never do.
        let incomparable = || {
            let compares = match &wanted.terms {
                Some(terms) => terms
                    .iter()
                    .all(|term| self.types.strictly_comparable(term.ty)),
                None => wanted.comparable,
            };
            // Asked only of a constraint that compares: the walk is as
            // long as the type is large.
            let fits = || match is_param {
                true => self.types.strictly_comparable(v),
                false => self.types.comparable(v),
            };
            match compares && !fits() {
                true => cause(format!("{shown} does not satisfy comparable")),
                false => None,
            }
        };
        let Some(terms) = &wanted.terms else {
            return incomparable();
        };
        if let Some(own) = own {
            return match self.types.subset(&own.terms, &wanted.terms) {
                true => incomparable(),
                false => cause(format!("{shown} does not satisfy {constraint}")),
            };
        }
        if terms.iter().any(|&term| self.types.in_term(term, v)) {
            return incomparable();
        }
        // A term that would hold the type if it were written with `~`.
        let nearly = terms.iter().find(|term| {
            let with_tilde = Term {
                tilde: true,
                ..**term
            };
            let is_underlying = self.types.underlying(term.ty) == term.ty;
            !term.tilde && is_underlying && self.types.in_term(with_tilde, v)
        });
        let detail = match nearly {
            Some(term) => format!(
                "possibly missing ~ for {} in {constraint}",
                self.type_string(term.ty)
            ),
            None if self.mentions(self.types.underlying(bound), v) => format!(
                "{constraint} mentions {shown}, but {shown} is not in the type set of \
                 {constraint}"
            ),
            None => {
                let listed: Vec<String> = terms.iter().map(|&t| self.term_string(t)).collect();
                format!("{shown} missing in {}", listed.join(" | "))
            }
        };
        cause(format!("{shown} does not satisfy {constraint} ({detail})"))
    }

    /// Why the type argument `arg` of the type parameter `param` is not
    /// copied as the constraint `bound` asks: by its bytes for `Copy`,
    /// at all for `Clone`. A constraint that lists only types that are
    /// copied one of these ways asks it too, of the declared types that
    /// a term `~T` holds (GEN-1, GEN-2).
    fn not_copied(&self, param: TypeId, arg: TypeId, bound: TypeId) -> Option<String> {
        use crate::types::Category;
        let set = self.types.type_set(bound)?;
        let wanted = self.types.constraint_category(&set);
        // A constraint is no type argument, which was said. A type
        // parameter that no type satisfies stands for none that is copied
        // otherwise.
        let said = self.open(arg) || self.types.is_constraint(arg);
        let none = self.types.param_set(arg).is_some_and(|set| set.is_empty());
        if said || none || self.types.category(arg) <= wanted {
            return None;
        }
        let at_all = wanted == Category::Copyable;
        let (part, why) = self.types.not_copied(arg, at_all)?;
        let shown = self.type_string(arg);
        let asked = match at_all {
            true => "Clone",
            false => "Copy",
        };
        let why = self.not_copied_text(part, why, at_all);
        let written = match at_all {
            true => set.markers.contains(&Marker::Clone),
            false => set.markers.contains(&Marker::Copy),
        };
        // Asked by the types that the constraint lists.
        let terms: Vec<String> = match (&set.terms, written) {
            (Some(terms), false) => terms.iter().map(|&term| self.term_string(term)).collect(),
            _ => return Some(format!("{shown} does not satisfy {asked} ({why})")),
        };
        Some(format!(
            "{shown} does not satisfy {asked}, which the type set of {} ({}) implies: {why}",
            self.type_string(param),
            terms.join(" | ")
        ))
    }

    /// Why the type argument `arg` lacks a label that the constraint
    /// `bound` names: `Send`, `Sync` (GEN-11).
    fn unlabeled(&self, arg: TypeId, bound: TypeId) -> Option<String> {
        let set = self.types.type_set(bound)?;
        if self.open(arg) || self.types.is_constraint(arg) {
            return None;
        }
        for (label, asked) in [(Marker::Send, "Send"), (Marker::Sync, "Sync")] {
            if !set.markers.contains(&label) {
                continue;
            }
            let Some((part, why)) = self.types.unlabeled(arg, label) else {
                continue;
            };
            let why = self.unlabeled_text(part, why, asked);
            return Some(format!(
                "{} does not satisfy {asked} ({why})",
                self.type_string(arg)
            ));
        }
        None
    }

    /// Why the type `part` lacks the label `asked`, as a message says it.
    pub(super) fn unlabeled_text(
        &self,
        part: TypeId,
        why: crate::types::Unlabeled,
        asked: &str,
    ) -> String {
        use crate::types::Unlabeled;
        let held = self.type_string(part);
        match why {
            Unlabeled::Borrow => format!("a {held} borrows what it points to"),
            Unlabeled::Local => format!("a {held} stays with one goroutine"),
            Unlabeled::SharedUnsync => format!("what a {held} holds is not Sync"),
            Unlabeled::Interface => format!("{held} does not embed Sync"),
            Unlabeled::Param => format!("{held} is not constrained by {asked}"),
        }
    }

    /// Why the type `part` is not copied by its bytes, or not at all if
    /// `at_all`, as a message says it.
    pub(super) fn not_copied_text(
        &self,
        part: TypeId,
        why: crate::types::NotCopied,
        at_all: bool,
    ) -> String {
        why.text(&self.type_string(part), at_all)
    }

    /// Holds the type arguments of an instance to the constraints of the
    /// type parameters: the first that does not satisfy its constraint is
    /// reported.
    pub(super) fn verify(&mut self, instance: &Instance) {
        if !instance.skip_cycle {
            self.mono_record(
                &instance.params,
                &instance.args,
                &instance.spans,
                instance.at,
            );
        }
        for (at, (&param, &arg)) in instance.params.iter().zip(&instance.args).enumerate() {
            // The generic type or function holds a value of this
            // parameter, and an abstract type has none (TYP-19).
            if self.types.is_held(param) && self.types.is_abstract(arg, self.names) {
                let span = instance.spans.get(at).copied().unwrap_or(instance.at);
                let shown = self.type_string(arg);
                let message = format!(
                    "cannot use {shown} for {}: a value of it is held, and {shown} is abstract",
                    self.type_string(param)
                );
                self.error(Code::AbstractInstance, span, message);
                return;
            }
            // The generic type or function writes `Option[P]` for this
            // parameter.
            if self.types.is_optional(param) && self.has_own_nil(arg) {
                let span = instance.spans.get(at).copied().unwrap_or(instance.at);
                let shown = self.type_string(arg);
                let message = format!(
                    "cannot use {shown} for {}: an Option of it is written, and {shown} has nil \
                     already",
                    self.type_string(param)
                );
                self.error(Code::InstOption, span, message);
                return;
            }
            // The generic type has a `@Required` field of this
            // parameter, and an option may be nil (ADR-0016).
            if self.types.is_required(param) && self.types.is_option(arg) {
                let span = instance.spans.get(at).copied().unwrap_or(instance.at);
                let shown = self.type_string(arg);
                let message = format!(
                    "cannot use {shown} for {}: a field of it is @Required, and an option may \
                     be nil",
                    self.type_string(param)
                );
                self.error(Code::OptionAndRequired, span, message);
                return;
            }
            let Some(constraint) = self.types.constraint(param) else {
                continue;
            };
            let bound = self
                .types
                .substitute(constraint, &instance.params, &instance.args);
            let elements = self.types.has_elements(param);
            let unsatisfied = self.unsatisfied(arg, bound, elements).or_else(|| {
                let text = self
                    .not_copied(param, arg, bound)
                    .or_else(|| self.unlabeled(arg, bound))?;
                Some(Cause {
                    text,
                    have_want: None,
                })
            });
            if let Some(cause) = unsatisfied {
                let span = instance.spans.get(at).copied().unwrap_or(instance.at);
                let mut diagnostic = Diagnostic::new(Code::TypeArgUnsatisfied, span, cause.text);
                if let Some((have, want)) = cause.have_want {
                    diagnostic = diagnostic
                        .with_note(Code::NoteHave, None, format!("have {have}"))
                        .with_note(Code::NoteWant, None, format!("want {want}"));
                }
                if self.report_once((span, Code::TypeArgUnsatisfied)) {
                    self.emit(diagnostic);
                }
                return;
            }
        }
    }
}
