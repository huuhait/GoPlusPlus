//! Conditions, `go` and `defer`, and the cases of a `switch`, after
//! `go/types` (stmt.go).

use super::call::Called;
use super::*;

/// The constant values of the cases so far, each with its types and
/// where it is written.
pub(super) type CaseValues = FxHashMap<String, Vec<(TypeId, Span)>>;

impl<'a> Checker<'a> {
    /// The condition of an `if` or a `for` statement is a boolean.
    pub(super) fn condition(&mut self, cx: &Cx<'a>, cond: ExprId, statement: &str) {
        let x = self.value(cx, cond);
        let x = self.read(cx, x);
        if x.mode != Mode::Unknown && !self.all_boolean(x.ty) {
            let message = format!("non-boolean condition in {statement} statement");
            self.error(Code::NonBooleanCondition, cx.tree.exprs[cond].span, message);
        }
    }

    /// `go f(x)` and `defer f(x)`: a call that does something.
    pub(super) fn suspended(&mut self, cx: &Cx<'a>, keyword: &str, call: ExprId) {
        let x = self.operand(cx, call);
        let span = cx.tree.exprs[call].span;
        let fun = match &cx.tree.exprs[call].kind {
            ExprKind::Call { fun, .. } => *fun,
            // Parentheses were refused where the statement was read.
            ExprKind::Paren(_) | ExprKind::MacroCall { .. } | ExprKind::Bad => return,
            // As Go's parser says it: after the expression.
            _ => {
                let after = Span {
                    lo: span.hi,
                    ..span
                };
                let message = format!("expression in {keyword} must be function call");
                return self.error(Code::GoDeferNeedsCall, after, message);
            }
        };
        if x.mode == Mode::Unknown {
            return;
        }
        let (code, what) = match self.called(fun) {
            Called::Conversion => (
                Code::GoDeferNeedsCall,
                "requires function call, not conversion",
            ),
            Called::Computation => (Code::UnusedExpression, "discards result of"),
            Called::Function => return,
        };
        let message = format!("{keyword} {what} {}", self.describe(cx, call, &x));
        self.error(code, span, message);
    }

    /// What an expression switch compares its cases with: the value of
    /// `tag`, or `true`.
    pub(super) fn switch_tag(&mut self, cx: &Cx<'a>, tag: Option<ExprId>) -> Operand {
        let Some(tag) = tag else {
            return Operand::constant(self.types.basic(Basic::Bool), Value::Bool(true));
        };
        let x = self.value(cx, tag);
        let x = self.assignment(cx, tag, x, None, "switch expression");
        let x = self.read(cx, x);
        let compared = x.mode == Mode::Unknown
            || self.types.comparable(x.ty)
            || self.types.has_nil(x.ty, Dialect::Go)
            || self.types.has_param(x.ty)
            || self.may_overload(cx, x.ty);
        if !compared {
            let message = format!(
                "cannot switch on {} ({} is not comparable)",
                self.describe(cx, tag, &x),
                self.type_string(x.ty)
            );
            self.error(
                Code::SwitchTagNotComparable,
                cx.tree.exprs[tag].span,
                message,
            );
            return Operand::UNKNOWN;
        }
        x
    }

    /// The value `case` of a clause of an expression switch on `x`: it
    /// can be compared with `x`, and as a constant it stands once.
    pub(super) fn case_value(
        &mut self,
        cx: &Cx<'a>,
        x: &Operand,
        tag: Option<ExprId>,
        case: ExprId,
        seen: &mut CaseValues,
    ) {
        let mut v = self.value(cx, case);
        if x.mode == Mode::Unknown || v.mode == Mode::Unknown {
            return;
        }
        if !self.convert_untyped(cx, case, &mut v, x.ty, true) {
            return;
        }
        let v = self.read(cx, v);
        let span = cx.tree.exprs[case].span;
        if v.ty == Types::INVALID || x.ty == Types::INVALID {
            return;
        }
        match self.uncomparable(cx, BinaryOp::Eq, &v, x) {
            Ok(None) => {}
            Err(()) => return,
            Ok(Some((_, cause, code))) => {
                let message = format!(
                    "invalid case {} in switch on {} ({cause})",
                    self.expr_string(cx, case),
                    tag.map_or("true".to_owned(), |tag| self.expr_string(cx, tag))
                );
                return self.error(code, span, message);
            }
        }
        // Like other compilers, only numbers and strings are looked at.
        let Mode::Const(value) = &v.mode else { return };
        if !matches!(value.kind(), Kind::Int | Kind::Float | Kind::String) {
            return;
        }
        let known = seen.entry(value.exact_string()).or_default();
        let same = |&&(ty, _): &&(TypeId, Span)| self.types.identical(v.ty, ty, self.names);
        let Some(&(_, previous)) = known.iter().find(same) else {
            return known.push((v.ty, span));
        };
        let message = format!(
            "duplicate case {} in expression switch",
            self.describe(cx, case, &v)
        );
        self.duplicate_case(Code::DuplicateCase, span, message, previous);
    }

    /// The type `case` of a clause of a type switch, or `nil`, stands
    /// once.
    pub(super) fn case_type(
        &mut self,
        cx: &Cx<'a>,
        case: ExprId,
        x: &Operand,
        seen: &mut FxHashMap<Option<TypeId>, Span>,
    ) {
        let ty = match x.mode {
            Mode::Type => Some(x.ty),
            Mode::Unknown => return,
            _ => None,
        };
        let span = cx.tree.exprs[case].span;
        let Some(&previous) = seen.get(&ty) else {
            seen.insert(ty, span);
            return;
        };
        let shown = ty.map_or("nil".to_owned(), |ty| self.type_string(ty));
        let message = format!("duplicate case {shown} in type switch");
        self.duplicate_case(Code::DuplicateTypeCase, span, message, previous);
    }

    fn duplicate_case(&mut self, code: Code, span: Span, message: String, previous: Span) {
        if self.reported.insert((span, code)) {
            let diagnostic = Diagnostic::new(code, span, message);
            let note = "previous case".to_owned();
            self.sink
                .push(diagnostic.with_note(Code::NotePreviousCase, Some(previous), note));
        }
    }

    /// A `switch` or a `select` has one `default` at the most. A clause
    /// is given as whether it is a `default`, and where it is.
    pub(super) fn defaults(&mut self, cx: &Cx<'a>, clauses: impl Iterator<Item = (bool, Span)>) {
        let mut first: Option<Span> = None;
        for (_, span) in clauses.filter(|&(is_default, _)| is_default) {
            let Some(first) = first else {
                first = Some(span);
                continue;
            };
            let place = cx.source.line_col(first.lo);
            let message = format!(
                "multiple defaults (first at {}:{}:{})",
                cx.source.path().display(),
                place.line,
                place.column
            );
            self.error(Code::MultipleDefaults, span, message);
        }
    }
}
