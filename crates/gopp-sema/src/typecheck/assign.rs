//! Assignments: a value given to a variable, a constant, a parameter, a
//! result, a channel. The functions follow `go/types` (assignments.go),
//! and the messages are Go's.

use super::ops::Unfit;
use super::*;

/// What gives variables their first values.
#[derive(Clone, Copy)]
pub(super) enum Init {
    /// `a, b := x, y` and `var a, b = x, y`.
    Variables,
    /// `return x, y`: the statement, for an error about all of it.
    Return(Span),
}

impl<'a> Checker<'a> {
    /// The operand `x` of the expression `expr` is assigned to a place of
    /// the type `to`, or to a place that takes its type from the value.
    /// `context` says where, for the messages.
    pub(super) fn assignment(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        mut x: Operand,
        to: Option<TypeId>,
        context: &str,
    ) -> Operand {
        let span = cx.tree.exprs[expr].span;
        let place = self.assigned_to.take();
        if x.mode == Mode::Unknown {
            return x;
        }
        // A generic function is the function that the place asks for.
        if self.types.generic_func(x.ty).is_some() {
            use super::infer::Place;
            let generic = self.generic_func_string(x.ty);
            let within = context
                .strip_prefix("argument to ")
                .map(|fun| format!("in call to {fun}, "));
            let place = match (&place, &within, context) {
                (_, Some(_), _) => Place::Parameter(&generic),
                (Some(place), ..) => Place::Named(place),
                (None, _, "return statement") => Place::Named("result variable"),
                (None, ..) => Place::Named("variable"),
            };
            let within = within.as_deref().unwrap_or("");
            x = self.generic_value(cx, expr, x, to, (place, within));
            if x.mode == Mode::Unknown {
                return x;
            }
        }
        // In a `.gopp` file a place takes more than its own type, and an
        // assignment may write through a pointer.
        let own = self.own_rules(cx);
        let writes = own && context == "assignment";
        let place = to;
        let to = match to {
            Some(to) if own && x.ty == self.types.basic(Basic::UntypedNil) => {
                if self.takes_nil(to, writes) {
                    return x;
                }
                let shown = self.type_string(to);
                let message = format!("cannot use nil as {shown} value in {context}");
                match self.never_nil(to) {
                    true => {
                        let fix = format!("a {shown} is never nil: use Option[{shown}]");
                        let fix = (Code::NoteOwnFixOption, fix);
                        self.error_with_fix(Code::NilNeverNil, span, message, fix);
                    }
                    false => self.error(Code::CannotAssign, span, message),
                }
                return Operand::UNKNOWN;
            }
            Some(to) if own && self.is_untyped(x.ty) => Some(self.own_target(to, writes)),
            to => to,
        };
        if x.mode == Mode::Value && matches!(&*self.types.get(x.ty), Type::Tuple(_)) {
            let message = format!(
                "multiple-value {} in single-value context",
                self.describe(cx, expr, &x)
            );
            self.error(Code::MultiValueInSingle, span, message);
            return Operand::UNKNOWN;
        }
        if !x.is_value() {
            return Operand::UNKNOWN;
        }
        if self.is_untyped(x.ty) {
            let nil = self.types.basic(Basic::UntypedNil);
            // In an interface, and where the place has no type of its
            // own, a value has the default type of its kind.
            let target = match to {
                Some(to) if !self.types.is_interface(to) => to,
                None if x.ty == nil => {
                    let message = format!("use of untyped nil in {context}");
                    self.error(Code::UseOfUntypedNil, span, message);
                    return Operand::UNKNOWN;
                }
                _ => self.types.default_type(x.ty),
            };
            match self.implicit(&x, target) {
                Err(unfit) => {
                    let why = match unfit {
                        Unfit::Truncated => " (truncated)",
                        Unfit::Overflows => " (overflows)",
                        Unfit::Other => "",
                    };
                    let message = format!(
                        "cannot use {} as {} value in {context}{why}",
                        self.describe(cx, expr, &x),
                        self.type_string(place.filter(|_| own).unwrap_or(target))
                    );
                    self.error(Code::CannotAssign, span, message);
                    return Operand::UNKNOWN;
                }
                Ok((ty, value)) => {
                    if let Some(value) = value {
                        self.update_expr_value(expr, &value);
                        x.mode = Mode::Const(value);
                    }
                    if ty != x.ty {
                        x.ty = ty;
                        self.update_expr_type(cx, expr, ty, false);
                    }
                }
            }
        }
        if let Some(to) = to
            && self.unfit(cx, &x, to, writes)
        {
            if own && self.hierarchy_unfit(cx, expr, &x, to) {
                return Operand::UNKNOWN;
            }
            let message = format!(
                "cannot use {} as {} value in {context}",
                self.describe(cx, expr, &x),
                self.type_string(to)
            );
            // A `@Convert` method is called where the conversion is
            // written (EXP-25).
            if self
                .convert_method(cx, self.read(cx, x.clone()).ty, to)
                .is_some()
            {
                let written = format!("{}({})", self.type_string(to), self.expr_string(cx, expr));
                let fix = (Code::NoteOpFix, format!("write {written}"));
                self.error_with_fix(Code::CannotAssign, span, message, fix);
                return Operand::UNKNOWN;
            }
            let cause = self
                .unfit_cause(&x, to)
                .or_else(|| self.untested_option(&x, to));
            self.error_with_cause(Code::CannotAssign, span, message, cause);
            return Operand::UNKNOWN;
        }
        // What an interface does not take is said once.
        let refused = to.is_some_and(|to| {
            let how = self.gives(context).filter(|_| own);
            self.note_interface(cx, expr, &x, to, writes, how)
        });
        if own
            && !refused
            && let Some(how) = self.gives(context)
        {
            match x.mode {
                Mode::Variable | Mode::MapIndex => {
                    // The value is of the type that the operand is read as.
                    let read = to.and_then(|to| self.fit_own(&x, to, writes));
                    let ty = read.map_or(x.ty, |(from, _)| from);
                    self.given(cx, expr, &x, ty, how);
                }
                Mode::Value => {
                    self.slice_given(cx, expr, &x, how);
                    self.lent_from_temporary(cx, expr, &x, to);
                }
                _ => {}
            }
        }
        x
    }

    /// Whether the rules of a `.gopp` file hold: a pointer is used like
    /// what it points to, a value is its own option, a pointer is never
    /// nil, and an operator may be a method.
    pub(super) fn own_rules(&self, cx: &Cx<'a>) -> bool {
        cx.dialect == Dialect::Gopp
    }

    /// The one value of `expr`, assigned as `assignment` says.
    pub(super) fn assign(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        to: Option<TypeId>,
        context: &str,
    ) -> Operand {
        let x = self.value(cx, expr);
        self.assignment(cx, expr, x, to, context)
    }

    /// `(int, string)`: types as a message about a count lists them. A
    /// number of no type is a number, whatever its kind.
    pub(super) fn types_summary(&self, types: &[TypeId], variadic: bool, spread: bool) -> String {
        let mut shown = Vec::new();
        for (at, &ty) in types.iter().enumerate() {
            let last = at + 1 == types.len();
            let mut text = match self.basic_of(ty).filter(|basic| basic.is_untyped()) {
                _ if ty == Types::INVALID => "unknown type".to_owned(),
                Some(Basic::UntypedBool) => "bool".to_owned(),
                Some(Basic::UntypedString) => "string".to_owned(),
                Some(Basic::UntypedNil) => "nil".to_owned(),
                Some(_) => "number".to_owned(),
                None => self.type_string(ty),
            };
            if last && variadic {
                if let Type::Slice(elem) = &*self.types.get(ty) {
                    text = self.type_string(*elem);
                }
                text.insert_str(0, "...");
            } else if last && spread {
                text += "...";
            }
            shown.push(text);
        }
        format!("({})", shown.join(", "))
    }

    pub(super) fn unparen(&self, cx: &Cx<'a>, mut expr: ExprId) -> ExprId {
        while let ExprKind::Paren(inner) = cx.tree.exprs[expr].kind {
            expr = inner;
        }
        expr
    }

    /// `assignment mismatch: 2 variables but f() returns 1 value`.
    fn assign_error(&mut self, cx: &Cx<'a>, rhs: &[ExprId], variables: usize, values: usize) {
        let measure = |count: usize, unit: &str| match count {
            1 => format!("1 {unit}"),
            count => format!("{count} {unit}s"),
        };
        let (variables, values) = (measure(variables, "variable"), measure(values, "value"));
        let Some(&first) = rhs.first() else {
            return;
        };
        let message = match (rhs.len(), &cx.tree.exprs[self.unparen(cx, first)].kind) {
            (1, ExprKind::Call { fun, .. }) => format!(
                "assignment mismatch: {variables} but {} returns {values}",
                self.expr_string(cx, *fun)
            ),
            _ => format!("assignment mismatch: {variables} but {values}"),
        };
        self.error(Code::AssignMismatch, cx.tree.exprs[first].span, message);
    }

    /// `not enough return values`, with what there is and what is wanted.
    fn return_error(&mut self, at: Span, wanted: &[TypeId], given: &[(Span, TypeId)]) {
        let (code, qualifier, at) = match given.len() > wanted.len() {
            // At the first value that is too many, or at the last one.
            true => (Code::TooManyReturnValues, "too many", given[wanted.len()].0),
            false => (
                Code::NotEnoughReturnValues,
                "not enough",
                given.last().map_or(at, |last| last.0),
            ),
        };
        let have: Vec<TypeId> = given.iter().map(|&(_, ty)| ty).collect();
        let diagnostic = Diagnostic::new(code, at, format!("{qualifier} return values"))
            .with_note(
                Code::NoteHave,
                None,
                format!("have {}", self.types_summary(&have, false, false)),
            )
            .with_note(
                Code::NoteWant,
                None,
                format!("want {}", self.types_summary(wanted, false, false)),
            );
        if self.reported.insert((at, code)) {
            self.sink.push(diagnostic);
        }
    }

    /// The values of the one expression `expr` where `count` are wanted:
    /// the results of a call, or `v, ok` of a comma-ok expression.
    fn multi_value(&mut self, cx: &Cx<'a>, expr: ExprId, count: usize) -> Vec<Operand> {
        let x = self.operand(cx, expr);
        if let Type::Tuple(types) = &*self.types.get(x.ty)
            && x.mode == Mode::Value
        {
            return types.iter().map(|&ty| Operand::value(ty)).collect();
        }
        // `v, ok = m[k]`, `v, ok = x.(T)`, `v, ok = <-ch`.
        if count == 2 && matches!(x.mode, Mode::MapIndex | Mode::CommaOk) {
            // The entry of the map stays in it.
            self.given(cx, expr, &x, x.ty, place::Given::Stored);
            let first = Operand::value(x.ty);
            return vec![first, self.value_of(Basic::UntypedBool)];
        }
        vec![self.value(cx, expr)]
    }

    /// Gives variables or results of the types `lhs` their values `rhs`;
    /// a variable without a type takes the type of its value. The types
    /// that the variables have then, invalid where the value is in error.
    pub(super) fn init_vars(
        &mut self,
        cx: &Cx<'a>,
        lhs: &[Option<TypeId>],
        rhs: &[ExprId],
        init: Init,
    ) -> Vec<TypeId> {
        let context = match init {
            Init::Variables => "assignment",
            Init::Return(_) => "return statement",
        };
        let unknown = vec![Types::INVALID; lhs.len()];
        let is_call = match rhs {
            [one] => matches!(
                cx.tree.exprs[self.unparen(cx, *one)].kind,
                ExprKind::Call { .. }
            ),
            _ => false,
        };
        if lhs.len() == rhs.len() && !is_call {
            let mut types = Vec::new();
            for (&to, &value) in lhs.iter().zip(rhs) {
                types.push(self.init_var(cx, value, None, to, context).ty);
            }
            return types;
        }
        if rhs.len() != 1 {
            let values: Vec<Operand> = rhs.iter().map(|&value| self.operand(cx, value)).collect();
            if values.iter().all(|value| value.mode != Mode::Unknown) {
                match init {
                    Init::Return(at) => {
                        let wanted: Vec<TypeId> = lhs.iter().flatten().copied().collect();
                        let given: Vec<(Span, TypeId)> = rhs
                            .iter()
                            .zip(&values)
                            .map(|(&value, operand)| (cx.tree.exprs[value].span, operand.ty))
                            .collect();
                        self.return_error(at, &wanted, &given);
                    }
                    Init::Variables => self.assign_error(cx, rhs, lhs.len(), rhs.len()),
                }
            }
            return unknown;
        }
        // `return m[k]` returns one value: only an assignment asks whether
        // the key was there.
        let asked = match init {
            Init::Variables => lhs.len(),
            Init::Return(_) => 0,
        };
        let values = self.multi_value(cx, rhs[0], asked);
        if values.len() == lhs.len() {
            let mut types = Vec::new();
            for (&to, value) in lhs.iter().zip(values) {
                types.push(self.init_var(cx, rhs[0], Some(value), to, context).ty);
            }
            return types;
        }
        if values[0].mode != Mode::Unknown {
            match init {
                Init::Return(at) => {
                    let wanted: Vec<TypeId> = lhs.iter().flatten().copied().collect();
                    let span = cx.tree.exprs[rhs[0]].span;
                    let given: Vec<(Span, TypeId)> =
                        values.iter().map(|value| (span, value.ty)).collect();
                    self.return_error(at, &wanted, &given);
                }
                Init::Variables => self.assign_error(cx, rhs, lhs.len(), values.len()),
            }
        }
        unknown
    }

    /// One variable of the type `to`, or without a type yet, gets the
    /// value of `expr`: `x` if that is one of several results.
    pub(super) fn init_var(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: Option<Operand>,
        to: Option<TypeId>,
        context: &str,
    ) -> Operand {
        let one_of_several = x.is_some();
        let x = match x {
            Some(x) => x,
            None => self.value(cx, expr),
        };
        if x.mode == Mode::Unknown || to == Some(Types::INVALID) {
            return Operand::UNKNOWN;
        }
        // One of several results is assigned as it is: it has a type.
        if one_of_several {
            return match to {
                Some(to) if self.unfit(cx, &x, to, false) => {
                    let message = format!(
                        "cannot use {} as {} value in {context}",
                        self.describe(cx, expr, &x),
                        self.type_string(to)
                    );
                    self.error(Code::CannotAssign, cx.tree.exprs[expr].span, message);
                    Operand::UNKNOWN
                }
                Some(to) => Operand::variable(to),
                None => Operand::variable(self.types.default_type(x.ty)),
            };
        }
        let assigned = self.assignment(cx, expr, x, to, context);
        match (assigned.mode == Mode::Unknown, to) {
            (true, _) => Operand::UNKNOWN,
            (false, Some(to)) => Operand::variable(to),
            (false, None) => Operand::variable(assigned.ty),
        }
    }

    /// The type of the place `lhs` on the left of `=`: `None` for `_`,
    /// invalid for what cannot be assigned to, which is reported.
    pub(super) fn lhs_var(&mut self, cx: &Cx<'a>, lhs: ExprId) -> Option<TypeId> {
        let inner = self.unparen(cx, lhs);
        if matches!(cx.tree.exprs[inner].kind, ExprKind::Name(name) if name == self.blank) {
            return None;
        }
        let outer = self.lookup_target.replace(inner);
        let known = self.unnarrowed.replace(inner);
        let mut x = self.value(cx, lhs);
        // `a[i] op= v` read the element with `Index`, and stores it with
        // `SetIndex` (EXP-22).
        if let ExprKind::Index { x: of, indices } = &cx.tree.exprs[inner].kind
            && self.operators.contains_key(&inner)
        {
            let held = self.value(cx, *of);
            let held = self.read(cx, held);
            x = self
                .operator_index(cx, inner, *of, &held, indices)
                .unwrap_or(x);
        }
        self.unnarrowed = known;
        self.lookup_target = outer;
        if x.mode == Mode::Unknown || x.ty == Types::INVALID {
            return Some(Types::INVALID);
        }
        let span = cx.tree.exprs[lhs].span;
        let text = self.expr_string(cx, lhs);
        // In a `.gopp` file a pointer is written through without a star,
        // also one that is no variable itself.
        if self.own_rules(cx) {
            if let ExprKind::Unary {
                op: UnaryOp::Star,
                x: pointer,
            } = cx.tree.exprs[inner].kind
            {
                let message = format!("cannot assign to {text}: a pointer is written without *");
                let fix = format!("use {}", self.expr_string(cx, pointer));
                let fix = (Code::NoteOwnFixNoStar, fix);
                self.error_with_fix(Code::OwnStarOnLeft, span, message, fix);
                return Some(Types::INVALID);
            }
            if self.shared_changed(cx, lhs, "assign to") {
                return Some(Types::INVALID);
            }
            if self.read(cx, x.clone()).ty != x.ty {
                self.wrote(cx, lhs);
                self.flow_written(cx, lhs);
                return Some(x.ty);
            }
        }
        if matches!(x.mode, Mode::Variable | Mode::MapIndex) {
            self.changed(cx, lhs, place::Change::Assigned);
            self.wrote(cx, lhs);
            self.flow_written(cx, lhs);
            if let ExprKind::Index { x: held, .. } = cx.tree.exprs[inner].kind {
                self.lost_entry(cx, lhs, held);
            }
            return Some(x.ty);
        }
        if let ExprKind::Selector { x: of, .. } = cx.tree.exprs[inner].kind
            && self.operand(cx, of).mode == Mode::MapIndex
        {
            let message = format!("cannot assign to struct field {text} in map");
            self.error(Code::CannotAssignTo, span, message);
            return Some(Types::INVALID);
        }
        // A part of what `@Operator Index` gives is a part of a copy.
        let mut base = inner;
        while let ExprKind::Selector { x, .. } | ExprKind::Paren(x) = cx.tree.exprs[base].kind {
            base = x;
        }
        if base != inner && self.operators.contains_key(&base) {
            let message =
                format!("cannot assign to {text}: what @Operator Index gives is no place");
            self.error(Code::OpIndexNotPlace, span, message);
            return Some(Types::INVALID);
        }
        let message =
            format!("cannot assign to {text} (neither addressable nor a map index expression)");
        self.error(Code::CannotAssignTo, span, message);
        Some(Types::INVALID)
    }

    /// `lhs = rhs`.
    pub(super) fn assign_vars(&mut self, cx: &Cx<'a>, lhs: &[ExprId], rhs: &[ExprId]) {
        // A use of the one place in its new value is assigned over.
        if let ([place], true) = (lhs, self.own_rules(cx))
            && self.is_plain_place(cx, *place)
        {
            self.target = Some(self.expr_string(cx, *place));
        }
        self.assign_places(cx, lhs, rhs);
        self.target = None;
    }

    fn assign_places(&mut self, cx: &Cx<'a>, lhs: &[ExprId], rhs: &[ExprId]) {
        let is_call = match rhs {
            [one] => matches!(
                cx.tree.exprs[self.unparen(cx, *one)].kind,
                ExprKind::Call { .. }
            ),
            _ => false,
        };
        if lhs.len() == rhs.len() && !is_call {
            self.swapping = self.is_swap(cx, lhs, rhs);
            if self.swapping && self.memo.is_some() {
                let places: Vec<ExprId> = lhs.iter().map(|&x| self.unparen(cx, x)).collect();
                self.swaps.extend(places);
            }
            for (&place, &value) in lhs.iter().zip(rhs) {
                match self.lhs_var(cx, place) {
                    Some(invalid) if invalid == Types::INVALID => {
                        self.operand(cx, value);
                    }
                    None => {
                        self.assign(cx, value, None, "assignment to _ identifier");
                    }
                    Some(to) => {
                        let x = self.value(cx, value);
                        let text = self.expr_string(cx, place);
                        self.assigned_to = Some(text.clone());
                        let assigned = self.assignment(cx, value, x, Some(to), "assignment");
                        self.note_write(cx, place, to, Some(&assigned));
                        self.field_stored(cx, place, &text, to, (value, &assigned));
                        self.flow_assigned_nil(cx, place, value);
                    }
                }
            }
            self.swapping = false;
            return;
        }
        if rhs.len() != 1 {
            // Nothing is said of the places when the count is wrong.
            let places: Vec<Operand> = lhs.iter().map(|&place| self.operand(cx, place)).collect();
            let values: Vec<Operand> = rhs.iter().map(|&value| self.operand(cx, value)).collect();
            let known = |operands: &[Operand]| operands.iter().all(|x| x.mode != Mode::Unknown);
            if known(&places) && known(&values) {
                self.assign_error(cx, rhs, lhs.len(), rhs.len());
            }
            return;
        }
        let values = self.multi_value(cx, rhs[0], lhs.len());
        if values.len() != lhs.len() {
            if values[0].mode != Mode::Unknown {
                self.assign_error(cx, rhs, lhs.len(), values.len());
            }
            for &place in lhs {
                self.operand(cx, place);
            }
            return;
        }
        let places: Vec<std::option::Option<LocalId>> = lhs
            .iter()
            .map(|&place| self.flow_local(cx, place))
            .collect();
        let mut types = Vec::new();
        for (&place, value) in lhs.iter().zip(values) {
            types.push(value.ty);
            let to = self.lhs_var(cx, place);
            if let Some(to) = to {
                self.note_write(cx, place, to, Some(&value));
                let text = self.expr_string(cx, place);
                self.field_stored(cx, place, &text, to, (rhs[0], &value));
            }
            if let Some(to) = to
                && value.mode != Mode::Unknown
                && self.unfit(cx, &value, to, true)
            {
                let message = format!(
                    "cannot use {} as {} value in assignment",
                    self.describe(cx, rhs[0], &value),
                    self.type_string(to)
                );
                self.error(Code::CannotAssign, cx.tree.exprs[rhs[0]].span, message);
            }
        }
        self.flow_guards(cx, &places, &types);
    }

    /// A declaration of `names` with the values `values` has as many
    /// values as names; `repeated` is how many values a constant without
    /// values of its own repeats.
    pub(super) fn arity(
        &mut self,
        cx: &Cx<'a>,
        names: &[Ident],
        values: &[ExprId],
        repeated: Option<usize>,
    ) {
        let is_const = repeated.is_some();
        let count = match values.is_empty() {
            true => repeated.unwrap_or(0),
            false => values.len(),
        };
        if count == 0 && !is_const {
            return;
        }
        if let Some(&extra) = values.get(names.len()) {
            let message = format!("extra init expr {}", self.expr_string(cx, extra));
            self.error(Code::ExtraInitExpr, cx.tree.exprs[extra].span, message);
        } else if names.len() > count
            && (is_const || count != 1)
            && let Some(name) = names.get(count)
        {
            let message = format!("missing init expr for {}", self.names.resolve(&name.name));
            self.error(Code::MissingInitExpr, name.span, message);
        }
    }

    /// `var a, b T = x, y`: the types of the variables, each that of its
    /// value where none is declared.
    pub(super) fn var_decl(
        &mut self,
        cx: &Cx<'a>,
        names: &[Ident],
        declared: Option<TypeId>,
        values: &[ExprId],
    ) -> Vec<TypeId> {
        let count = names.len();
        let or_declared = |ty: TypeId| declared.unwrap_or(ty);
        match values {
            [] => {
                if let Some(declared) = declared {
                    self.declared_unset(cx, names, declared);
                    self.flow_declared_nil(cx, names);
                }
                vec![or_declared(Types::INVALID); count]
            }
            // `var a, b = f()`.
            [_] if count > 1 => {
                let places = vec![declared; count];
                let types = self.init_vars(cx, &places, values, Init::Variables);
                types.into_iter().map(or_declared).collect()
            }
            _ => {
                let mut types = Vec::new();
                for (position, name) in names.iter().enumerate() {
                    let ty = match values.get(position) {
                        Some(&value) => {
                            let context = "variable declaration";
                            let name = self.names.resolve(&name.name);
                            self.assigned_to = Some(name.to_owned());
                            let ty = self.init_var(cx, value, None, declared, context).ty;
                            self.assigned_to = None;
                            if self.is_nil_name(cx, value) {
                                self.flow_declared_nil(cx, &names[position..=position]);
                            }
                            ty
                        }
                        None => Types::INVALID,
                    };
                    types.push(or_declared(ty));
                }
                for &extra in values.iter().skip(count) {
                    self.operand(cx, extra);
                }
                types
            }
        }
    }

    /// The value of a constant that is declared with the type `declared`,
    /// or with none, and the value `expr`.
    pub(super) fn const_value(
        &mut self,
        cx: &Cx<'a>,
        declared: Option<TypeId>,
        expr: ExprId,
    ) -> Operand {
        let own = self.own_rules(cx);
        let outer = std::mem::replace(&mut self.in_const, own);
        let x = self.value(cx, expr);
        self.in_const = outer;
        // A constant that is declared with a type has that type whatever
        // its value is; a value in error is not known.
        let unknown = match declared {
            Some(ty) if ty != Types::INVALID => Operand::constant(ty, Value::Unknown),
            _ => Operand::UNKNOWN,
        };
        if x.mode == Mode::Unknown || declared == Some(Types::INVALID) {
            return unknown;
        }
        // An option that is nil is a constant (EXP-31).
        if let Some(ty) = declared
            && self.own_rules(cx)
            && x.ty == self.types.basic(Basic::UntypedNil)
            && matches!(&*self.types.get(self.types.underlying(ty)), Type::Option(_))
        {
            let nil = crate::constant::Composite::Nil;
            return Operand::constant(ty, Value::Composite(std::sync::Arc::new(nil)));
        }
        if !matches!(x.mode, Mode::Const(_)) {
            let message = format!("{} is not constant", self.describe(cx, expr, &x));
            self.error(Code::NotConstant, cx.tree.exprs[expr].span, message);
            return unknown;
        }
        let Some(ty) = declared else { return x };
        // The constant has the declared type and holds the value as that
        // type holds it.
        let assigned = self.assignment(cx, expr, x, Some(ty), "constant declaration");
        let Mode::Const(value) = assigned.mode else {
            return unknown;
        };
        // A constant that is made of others is held as it is.
        if self.basic_of(ty).is_none() && self.own_rules(cx) {
            return Operand::constant(ty, value);
        }
        match self.converted(&value, ty) {
            Some(value) => Operand::constant(ty, value),
            None => unknown,
        }
    }

    /// The type that a constant is declared with: a type of constants.
    pub(super) fn const_type(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        let ty = self.typ(cx, expr);
        let under = self.types.underlying(ty);
        if ty == Types::INVALID || matches!(&*self.types.get(under), Type::Basic(_)) {
            return ty;
        }
        // In a `.gopp` file a constant may be made of others (EXP-31).
        if under != Types::INVALID && self.own_rules(cx) {
            return match self.not_constant_type(cx, expr, ty) {
                true => Types::INVALID,
                false => ty,
            };
        }
        if under != Types::INVALID && !self.types.has_param(ty) {
            let message = format!("invalid constant type {}", self.type_string(ty));
            self.error(Code::InvalidConstantType, cx.tree.exprs[expr].span, message);
        }
        Types::INVALID
    }
}
