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
    /// or written with a type parameter, a constraint, a type with
    /// parents.
    pub(super) fn lacking(&self, v: TypeId, iface: TypeId) -> Option<(Method, Lacks)> {
        let pointee = match &*self.types.get(self.types.underlying(v)) {
            Type::Pointer(elem) => *elem,
            _ => v,
        };
        let undecided = [v, pointee, iface].iter().any(|&ty| self.open(ty))
            || self.types.extends(v)
            || self.types.extends(pointee);
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
            Lacks::NoField => format!("({shown}.{name} is a field, not a method)"),
            Lacks::Name(has) => {
                have_want = Some((self.method_string(&has), self.method_string(&method)));
                format!("(missing method {name})")
            }
            Lacks::Type(has) => {
                let (have, want) = (self.method_string(&has), self.method_string(&method));
                // Two types of one name say nothing next to each other.
                have_want = (have != want).then_some((have, want));
                format!("(wrong type for method {name})")
            }
        };
        Some(Cause { text, have_want })
    }

    /// Why the operand `x`, which fits no place of the type `to`, does
    /// not, where there is more to say than its type.
    pub(super) fn unfit_cause(&self, x: &Operand, to: TypeId) -> Option<Cause> {
        // An option takes what the type that it holds takes.
        let to = match &*self.types.get(self.types.underlying(to)) {
            Type::Option(elem) => *elem,
            _ => to,
        };
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
        if !x.is_value() || self.types.is_interface(ty) {
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
    /// is on the left of `=`.
    pub(super) fn note_interface(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &Operand,
        to: TypeId,
        writes: bool,
    ) {
        if self.memo.is_none() || !x.is_value() || self.is_untyped(x.ty) {
            return;
        }
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
        }
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
