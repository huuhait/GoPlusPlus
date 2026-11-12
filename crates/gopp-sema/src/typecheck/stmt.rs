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

    /// What `range` over a value of the type gives at each step: the
    /// types of the first and of the second variable, where there is
    /// one. `Ok(None)` if that is not decided here, `Err` with the cause,
    /// if there is a special one, when the type cannot be ranged over.
    #[allow(clippy::type_complexity)]
    fn range_key_value(
        &self,
        cx: &Cx<'a>,
        ty: TypeId,
    ) -> Result<Option<(Option<TypeId>, Option<TypeId>)>, String> {
        let int = Some(self.types.basic(Basic::Int));
        let mut under = self.types.underlying(ty);
        let is_param = matches!(&*self.types.get(under), Type::Param(_));
        if under == Types::INVALID || is_param || self.may_overload(cx, ty) {
            return Ok(None);
        }
        // A pointer to an array is ranged over as the array.
        if let Type::Pointer(elem) = &*self.types.get(under)
            && matches!(
                &*self.types.get(self.types.underlying(*elem)),
                Type::Array { .. }
            )
        {
            under = self.types.underlying(*elem);
        }
        let iterator = "func must be func(yield func(...) bool)";
        Ok(Some(match &*self.types.get(under) {
            Type::Basic(_) if self.is_string(ty) => (int, Some(self.types.basic(Basic::Int32))),
            Type::Basic(_) if self.is_integer(ty) => (Some(ty), None),
            Type::Array { elem, .. } | Type::Slice(elem) => (int, Some(*elem)),
            Type::Map { key, value } => (Some(*key), Some(*value)),
            Type::Chan {
                dir: ChanDir::Send, ..
            } => {
                return Err(format!(
                    "receive from send-only channel {}",
                    self.type_string(ty)
                ));
            }
            Type::Chan { elem, .. } => (Some(*elem), None),
            // An iterator calls `yield` with what each step gives.
            Type::Func {
                params, results, ..
            } => {
                let [yield_] = &params[..] else {
                    return Err(format!("{iterator}: wrong argument count"));
                };
                if !results.is_empty() {
                    return Err(format!("{iterator}: unexpected results"));
                }
                let yield_ = self.types.get(self.types.underlying(*yield_));
                let (params, results) = match &*yield_ {
                    Type::Func {
                        params, results, ..
                    } => (params, results),
                    // What a type parameter stands for is not followed.
                    Type::Param(_) => return Ok(None),
                    _ => return Err(format!("{iterator}: argument is not func")),
                };
                if params.len() > 2 {
                    return Err(format!("{iterator}: yield func has too many parameters"));
                }
                let bool = self.types.basic(Basic::Bool);
                match &results[..] {
                    [result] if *result == bool => {}
                    [result] if self.is_boolean(*result) => {
                        return Err(format!(
                            "{iterator}: yield func returns user-defined boolean, not bool"
                        ));
                    }
                    _ => return Err(format!("{iterator}: yield func does not return bool")),
                }
                (params.first().copied(), params.get(1).copied())
            }
            _ => return Err(String::new()),
        }))
    }

    /// `for lhs := range x` or `for lhs = range x`: what the variables
    /// are, and that there are no more of them than `x` gives.
    pub(super) fn range_clause(&mut self, cx: &Cx<'a>, lhs: &[ExprId], define: bool, of: ExprId) {
        let tree = cx.tree;
        let x = self.value(cx, of);
        let x = self.read(cx, x);
        let span = |expr: ExprId| tree.exprs[expr].span;
        let mut given = None;
        if x.mode != Mode::Unknown {
            let shown = self.describe(cx, of, &x);
            match self.range_key_value(cx, x.ty) {
                Err(cause) => {
                    let cause = match cause.is_empty() {
                        true => cause,
                        false => format!(": {cause}"),
                    };
                    let message = format!("cannot range over {shown}{cause}");
                    self.error(Code::RangeCannot, span(of), message);
                }
                Ok(None) => {}
                Ok(Some((key, value))) => {
                    // More than two are refused as Go's parser refuses
                    // them, whatever is ranged over.
                    let too_many = match (key, value, lhs) {
                        (_, _, [.., last]) if lhs.len() > 2 => {
                            let message = "expected at most 2 expressions".to_owned();
                            self.error(Code::RangeVariables, span(*last), message);
                            None
                        }
                        (None, _, [first, ..]) => Some((*first, "no iteration variables")),
                        (_, None, [_, second, ..]) => {
                            Some((*second, "only one iteration variable"))
                        }
                        _ => None,
                    };
                    if let Some((at, what)) = too_many {
                        let message = format!("range over {shown} permits {what}");
                        self.error(Code::RangeVariables, span(at), message);
                    }
                    given = Some([key, value]);
                }
            }
        }
        // A number is counted with the type that it has as a value.
        let counts = x.mode != Mode::Unknown && given.is_some() && self.is_integer(x.ty);
        for (position, &target) in lhs.iter().enumerate() {
            let ty = given.and_then(|given| given.get(position).copied().flatten());
            if !define {
                let Some(ty) = ty else {
                    self.operand(cx, target);
                    continue;
                };
                let Some(to) = self.lhs_var(cx, target).filter(|&to| to != Types::INVALID) else {
                    continue;
                };
                if !counts {
                    self.assignment(cx, target, Operand::value(ty), Some(to), "assignment");
                    continue;
                }
                let counted = self.assignment(cx, of, x.clone(), Some(to), "range clause");
                if counted.mode != Mode::Unknown && !self.is_integer(counted.ty) {
                    let message = format!(
                        "cannot use iteration variable of type {}",
                        self.type_string(counted.ty)
                    );
                    self.error(Code::RangeCannot, span(target), message);
                }
                continue;
            }
            // `&v` declares `v` as another name of the element.
            let (named, alias) = match tree.exprs[target].kind {
                ExprKind::Unary {
                    op: UnaryOp::Addr,
                    x,
                } => (x, true),
                _ => (target, false),
            };
            let local = self.declared_local(cx, named);
            let ty = match (ty, counts) {
                (Some(_), true) => {
                    let counted = self.assignment(cx, of, x.clone(), None, "range clause");
                    Some(counted.ty).filter(|_| counted.mode != Mode::Unknown)
                }
                (ty, _) => ty,
            };
            let ty = match (ty, alias) {
                (Some(ty), true) => self.aliased(cx, of, &x, target, ty),
                (ty, _) => ty,
            };
            self.set_local(local, ty);
            // What cannot be ranged over gives its variables no type.
            self.excused.extend(local.filter(|_| ty.is_none()));
        }
        if lhs.is_empty() && counts {
            self.assignment(cx, of, x, None, "range clause");
        }
    }

    /// The type of the variable `&v` of a range clause over `x`, which
    /// gives a value of the type `given` at each step: `v` is an element
    /// of a slice, an array or a map, or what a pointer that an iterator
    /// yields points to. What is counted, read or received is no place.
    fn aliased(
        &mut self,
        cx: &Cx<'a>,
        of: ExprId,
        x: &Operand,
        target: ExprId,
        given: TypeId,
    ) -> Option<TypeId> {
        let span = cx.tree.exprs[target].span;
        let shown = self.describe(cx, of, x);
        let mut under = self.types.underlying(x.ty);
        if let Type::Pointer(elem) = &*self.types.get(under) {
            under = self.types.underlying(*elem);
        }
        let (code, message) = match &*self.types.get(under) {
            Type::Array { .. } | Type::Slice(_) | Type::Map { .. } => return Some(given),
            Type::Func { .. } => match &*self.types.get(self.types.underlying(given)) {
                Type::Pointer(elem) => return Some(*elem),
                _ => (
                    Code::OwnRangeAliasFunc,
                    format!("invalid & in range clause: {shown} does not yield pointers"),
                ),
            },
            _ => (
                Code::OwnRangeAliasInvalid,
                format!("invalid & in range clause: {shown} has no elements to alias"),
            ),
        };
        self.error(code, span, message);
        None
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
