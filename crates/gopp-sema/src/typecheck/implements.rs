//! Whether a type implements an interface, said as Go says it.

use super::*;
use crate::types::Lacks;

/// Why a value is no value of a type.
pub(super) struct Cause {
    pub text: String,
    /// The method that the type has, and the method that is wanted.
    pub have_want: Option<(String, String)>,
}

impl<'a> Checker<'a> {
    fn points_to_interface(&self, ty: TypeId) -> bool {
        matches!(
            &*self.types.get(self.types.underlying(ty)),
            Type::Pointer(elem) if self.types.is_interface(*elem)
        )
    }

    /// The method of the interface `iface` that values of the type `v`
    /// lack. `None` also where that is not decided here: a type in error
    /// or written with a type parameter, a constraint.
    pub(super) fn lacking(&self, v: TypeId, iface: TypeId) -> Option<(Method, Lacks)> {
        let pointee = match &*self.types.get(self.types.underlying(v)) {
            Type::Pointer(elem) => *elem,
            _ => v,
        };
        let undecided = [v, pointee, iface].iter().any(|&ty| self.open(ty));
        match undecided {
            true => None,
            false => self.types.missing_method(v, iface, self.names),
        }
    }

    /// `m(int) string`: a method as a message shows it.
    fn method_string(&self, method: &Method) -> String {
        let sig = self.type_string(method.sig);
        let name = self.names.resolve(&method.name);
        format!("{name}{}", sig.strip_prefix("func").unwrap_or(&sig))
    }

    /// `(missing method m)`: what the type `v` lacks of the interface
    /// `iface`.
    pub(super) fn lacking_cause(&self, v: TypeId, iface: TypeId) -> Option<Cause> {
        let (method, lacks) = self.lacking(v, iface)?;
        Some(self.lacks_cause(v, &method, lacks))
    }

    /// `(missing method m)`: that the type `v` lacks `method`, and what it
    /// has in its place.
    pub(super) fn lacks_cause(&self, v: TypeId, method: &Method, lacks: Lacks) -> Cause {
        let name = self.names.resolve(&method.name);
        let shown = self.type_string(v);
        let mut have_want = None;
        let text = match lacks {
            Lacks::Method if self.points_to_interface(v) => {
                format!("(type {shown} is pointer to interface, not interface)")
            }
            Lacks::Method => format!("(missing method {name})"),
            Lacks::Exported => format!("(unexported method {name})"),
            Lacks::OneMember => format!("(ambiguous selector {shown}.{name})"),
            Lacks::ValueReceiver => format!("(method {name} has pointer receiver)"),
            Lacks::Generic => format!("(method {name} has type parameters)"),
            Lacks::NoField => format!("({shown}.{name} is a field, not a method)"),
            Lacks::Name(has) => {
                have_want = Some((self.method_string(&has), self.method_string(method)));
                format!("(missing method {name})")
            }
            Lacks::Type(has) => {
                let (have, want) = (self.method_string(&has), self.method_string(method));
                // Two types of one name say nothing next to each other.
                have_want = (have != want).then_some((have, want));
                format!("(wrong type for method {name})")
            }
        };
        Cause { text, have_want }
    }

    /// Why the operand `x`, which fits no place of the type `to`, does
    /// not, where there is more to say than its type.
    pub(super) fn unfit_cause(&self, x: &Operand, to: TypeId) -> Option<Cause> {
        // An option takes what the type that it holds takes.
        let to = match &*self.types.get(self.types.underlying(to)) {
            Type::Option(elem) => *elem,
            _ => to,
        };
        if self.is_param(x.ty) || self.is_param(to) {
            return self.param_unfit(x, to).flatten().map(|text| Cause {
                text,
                have_want: None,
            });
        }
        let (from, target) = (self.type_string(x.ty), self.type_string(to));
        if self.points_to_interface(to) {
            let text = format!(
                "{from} does not implement {target} (type {target} is pointer to interface, not \
                 interface)"
            );
            return Some(Cause {
                text,
                have_want: None,
            });
        }
        if self.types.is_interface(to) {
            let mut cause = self.lacking_cause(x.ty, to)?;
            cause.text = format!("{from} does not implement {target} {}", cause.text);
            return Some(cause);
        }
        // What is in an interface is taken out of it by an assertion.
        let asserted = self.types.is_interface(x.ty) && self.lacking(to, x.ty).is_none();
        asserted.then(|| Cause {
            text: "need type assertion".to_owned(),
            have_want: None,
        })
    }

    /// `x.(T)`, or the case `T` of a type switch on `x`: a value of the
    /// interface type of `x` must be able to have the type `ty`. `at` is
    /// what is written, `of` is `x`.
    pub(super) fn assertable(
        &mut self,
        cx: &Cx<'a>,
        at: ExprId,
        (of, x): (ExprId, &Operand),
        ty: TypeId,
        case: bool,
    ) {
        if x.is_value() && !self.types.is_interface(x.ty) {
            return self.downcast(cx, at, x, ty);
        }
        if !x.is_value() || self.types.is_interface(ty) {
            return;
        }
        // No interface value holds what cannot be copied (TYP-9).
        if self.own_rules(cx)
            && let Some((part, why)) = self.types.not_copied(ty, true)
            && !self.open(part)
        {
            let message = format!(
                "impossible type {}: no interface value holds a {}: {}",
                if case { "switch case" } else { "assertion" },
                self.type_string(ty),
                self.not_copied_text(part, why, true)
            );
            let span = cx.tree.exprs[at].span;
            return self.error(Code::OwnBoxNeedsOwnership, span, message);
        }
        if self.own_rules(cx) && self.owned_by_interface(ty, x.ty) {
            return;
        }
        let Some(cause) = self.lacking_cause(ty, x.ty) else {
            return;
        };
        let (written, shown) = (self.expr_string(cx, at), self.type_string(ty));
        let (message, why) = match case {
            true => (
                format!("impossible type switch case: {written}"),
                format!(
                    "{} cannot have dynamic type {shown}",
                    self.describe(cx, of, x)
                ),
            ),
            false => (
                format!("impossible type assertion: {written}"),
                format!("{shown} does not implement {}", self.type_string(x.ty)),
            ),
        };
        let span = cx.tree.exprs[at].span;
        if !self.reported.insert((span, Code::TypeAssertionImpossible)) {
            return;
        }
        let mut diagnostic = Diagnostic::new(Code::TypeAssertionImpossible, span, message)
            .with_note(
                Code::NoteMissingMethod,
                None,
                format!("{why} {}", cause.text),
            );
        if let Some((have, want)) = cause.have_want {
            diagnostic = diagnostic
                .with_note(Code::NoteHave, None, format!("have {have}"))
                .with_note(Code::NoteWant, None, format!("want {want}"));
        }
        self.sink.push(diagnostic);
    }

    /// Notes that the value `x` of `expr`, which fits a place of the type
    /// `to`, is put into an interface there. `writes` says that the place
    /// is on the left of `=`, `how` how the value is given. Whether the
    /// interface does not take the value, which is reported.
    pub(super) fn note_interface(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &Operand,
        to: TypeId,
        writes: bool,
        how: Option<super::place::Given>,
    ) -> bool {
        if self.memo.is_none() || !x.is_value() || self.is_untyped(x.ty) {
            return false;
        }
        let place = to;
        let fit = match self.own_rules(cx) {
            true => self.fit_own(x, to, writes),
            false => Some((x.ty, to)),
        };
        // An option of an interface holds the interface.
        let held = |mut ty: TypeId| {
            while let Type::Option(elem) = &*self.types.get(self.types.underlying(ty)) {
                ty = *elem;
            }
            ty
        };
        if let Some((from, to)) = fit.map(|(from, to)| (held(from), held(to)))
            && self.types.is_interface(to)
            && !self.types.is_interface(from)
            && !self.open(from)
        {
            self.iface_conversions.insert(expr, (from, to));
            return self.own_rules(cx) && self.boxed(cx, (expr, x), (from, to), place, how);
        }
        false
    }

    /// The value `x` of `expr`, read as the type `from`, is put into a
    /// value of the interface type `iface`, in a place of the type
    /// `place`. The interface holds a copy, so the type is one that is
    /// copied (TYP-6); and what is kept in an interface may be given to
    /// another goroutine, unless the place is a `Local` (CON-12). Whether
    /// an argument is kept, the function that takes it says.
    pub(super) fn boxed(
        &mut self,
        cx: &Cx<'a>,
        (expr, x): (ExprId, &Operand),
        (from, iface): (TypeId, TypeId),
        place: TypeId,
        how: Option<super::place::Given>,
    ) -> bool {
        use super::place::Given;
        use crate::types::Marker;
        let span = cx.tree.exprs[expr].span;
        let shown = self.describe(cx, expr, x);
        let to = self.type_string(iface);
        if let Some((part, why)) = self.types.not_copied(from, true)
            && !self.open(part)
        {
            let message = format!(
                "cannot convert {shown} to {to}: an interface value holds a copy, and {}",
                self.not_copied_text(part, why, true)
            );
            let text = self.expr_string(cx, expr);
            let fix = match x.mode {
                Mode::Variable => {
                    format!("put it in a Shared, or pass &{text} for the length of a call")
                }
                _ => format!("put it in a Shared: NewShared({text})"),
            };
            let fix = (Code::NoteOwnFixAddress, fix);
            self.error_with_fix(Code::OwnBoxNeedsOwnership, span, message, fix);
            return true;
        }
        match how {
            Some(Given::Passed) => {
                self.iface_args.insert(expr);
                return false;
            }
            Some(_) => {}
            None => return false,
        }
        let mut local = place;
        let is_local = loop {
            local = match &*self.types.get(self.types.underlying(local)) {
                Type::Local(_) => break true,
                Type::Option(elem) => *elem,
                _ => break false,
            };
        };
        if is_local {
            return false;
        }
        let labels = [
            (Marker::Send, "Send", true),
            (Marker::Sync, "Sync", self.types.labels(iface).sync),
        ];
        for (label, asked, needed) in labels {
            let Some((part, why)) = self.types.unlabeled(from, label).filter(|_| needed) else {
                continue;
            };
            if self.open(part) {
                return false;
            }
            let goes = match label {
                Marker::Send => "may be given to another goroutine",
                _ => "may be shared between goroutines",
            };
            let message = format!(
                "cannot keep {shown} in a value of type {to}: an interface value {goes}, and {}",
                self.unlabeled_text(part, why, asked)
            );
            let fix = (
                Code::NoteOwnFixOption,
                format!("keep it in a Local[{to}], or hold a Shared"),
            );
            self.error_with_fix(Code::IfaceNotSend, span, message, fix);
            return true;
        }
        false
    }

    /// A variable, a parameter, a field or an element has no constraint
    /// as its type.
    pub(super) fn no_constraint(&mut self, ty: TypeId, span: Span) {
        use crate::types::{Constraint, Marker};
        let constraint = match &*self.types.get(ty) {
            Type::Marker(Marker::Comparable) => Some(Constraint::Comparable),
            Type::Marker(_) => Some(Constraint::Marker),
            _ => self.types.constraint_kind(ty),
        };
        let (code, why) = match constraint {
            Some(Constraint::Comparable) => (
                Code::InterfaceIsComparable,
                "interface is (or embeds) comparable",
            ),
            Some(Constraint::Terms) => (
                Code::InterfaceContainsTypeConstraints,
                "interface contains type constraints",
            ),
            // What the other predeclared constraints allow is decided
            // with type parameters.
            Some(Constraint::Marker) | None => return,
        };
        let message = format!(
            "cannot use type {} outside a type constraint: {why}",
            self.type_string(ty)
        );
        self.error(code, span, message);
    }

    /// Reports `message` with the cause, which is in parentheses, after
    /// it.
    pub(super) fn error_with_cause_after(
        &mut self,
        code: Code,
        span: Span,
        message: String,
        cause: Cause,
    ) {
        if !self.reported.insert((span, code)) {
            return;
        }
        let mut diagnostic = Diagnostic::new(code, span, format!("{message} {}", cause.text));
        if let Some((have, want)) = cause.have_want {
            diagnostic = diagnostic
                .with_note(Code::NoteHave, None, format!("have {have}"))
                .with_note(Code::NoteWant, None, format!("want {want}"));
        }
        self.sink.push(diagnostic);
    }

    /// Reports `message`, with the cause after it.
    pub(super) fn error_with_cause(
        &mut self,
        code: Code,
        span: Span,
        message: String,
        cause: Option<Cause>,
    ) {
        if !self.reported.insert((span, code)) {
            return;
        }
        let Some(cause) = cause else {
            return self.sink.push(Diagnostic::new(code, span, message));
        };
        let mut diagnostic = Diagnostic::new(code, span, format!("{message}: {}", cause.text));
        if let Some((have, want)) = cause.have_want {
            diagnostic = diagnostic
                .with_note(Code::NoteHave, None, format!("have {have}"))
                .with_note(Code::NoteWant, None, format!("want {want}"));
        }
        self.sink.push(diagnostic);
    }
}
