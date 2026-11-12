//! The arguments of a call and an expression that stands as a statement,
//! after `go/types` (call.go, stmt.go).

use super::*;

/// What a call expression is.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Called {
    Conversion,
    Computation,
    Function,
}

impl<'a> Checker<'a> {
    /// The result of `f!(x)`: the results of `f(x)` without the error
    /// (ERR-5). The innermost function around the call decides what the
    /// call does with an error (ERR-6, ERR-11a).
    pub(super) fn unwrapped(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        fun: ExprId,
        called: Operand,
    ) -> Operand {
        if called.mode == Mode::Unknown {
            return called;
        }
        self.steps += 1;
        let span = cx.tree.exprs[call].span;
        if self.called(fun) == Called::Conversion {
            let shown = self.expr_string(cx, call);
            let message = format!("invalid operation: {shown} (a conversion returns no error)");
            self.error(Code::BangOnConversion, span, message);
            return Operand::UNKNOWN;
        }
        let error = self.types.error();
        let unwrapped = match &*self.types.get(called.ty) {
            Type::Tuple(results) if called.mode == Mode::Value => match &results[..] {
                [one, last] if *last == error => Some(Operand::value(*one)),
                [rest @ .., last] if *last == error => {
                    Some(Operand::value(self.types.intern(Type::Tuple(rest.into()))))
                }
                _ => None,
            },
            _ if called.mode == Mode::Value && called.ty == error => Some(Operand {
                mode: Mode::NoValue,
                ty: Types::INVALID,
            }),
            _ => None,
        };
        let Some(unwrapped) = unwrapped else {
            let shown = self.expr_string(cx, call);
            let message = format!("invalid operation: {shown} (the call returns no error)");
            self.error(Code::BangNoErrorResult, span, message);
            return Operand::UNKNOWN;
        };
        let function = self.results.last().and_then(Option::as_ref);
        let can_fail =
            function.is_some_and(|results| results.fails || results.types.last() == Some(&error));
        let unset = function.and_then(|results| results.unset);
        let bang = match self.suspended {
            Some((suspended, go)) if suspended == call => match can_fail && !go {
                true => Bang::SetError,
                false => Bang::Panic,
            },
            _ if can_fail => Bang::Return,
            _ => Bang::Panic,
        };
        if let (Bang::Return, Some(unset)) = (bang, unset) {
            let message = format!(
                "invalid operation: {} (on an error the function returns no {} value)",
                self.expr_string(cx, call),
                self.type_string(unset)
            );
            let fix = "declare the function with ! or handle the error and return a value";
            let fix = (Code::NoteFixBang, fix.to_owned());
            self.error_with_fix(Code::BangNoZeroResult, span, message, fix);
        }
        // Only where the call stands, not where an object that another
        // body names is worked out.
        if self.memo.is_some() {
            self.bangs.insert(call, bang);
        }
        unwrapped
    }

    /// The arguments `args` of the call `call` of `fun`, which has the
    /// parameters `params`: as many as the parameters, and each a value
    /// that its parameter takes.
    pub(super) fn arguments(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        fun: ExprId,
        (params, variadic): (&[TypeId], bool),
        args: &[ExprId],
        spread: bool,
    ) {
        self.given_arguments(cx, call, fun, (params, variadic), args, spread, None);
    }

    /// `arguments` for a call of the generic function `generic` of the
    /// type `sig`: the type that the function has in this call. `None` if
    /// its type arguments are not found.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn call_arguments(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        fun: ExprId,
        sig: TypeId,
        args: &[ExprId],
        spread: bool,
        generic: super::infer::Generic,
    ) -> Option<TypeId> {
        let Type::Func {
            params, variadic, ..
        } = &*self.types.get(sig)
        else {
            return None;
        };
        self.given_arguments(
            cx,
            call,
            fun,
            (params, *variadic),
            args,
            spread,
            Some(generic),
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn given_arguments(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        fun: ExprId,
        (params, variadic): (&[TypeId], bool),
        args: &[ExprId],
        spread: bool,
        generic: Option<super::infer::Generic>,
    ) -> Option<TypeId> {
        // `f(g())` passes all results of `g`.
        let mut given: Vec<(ExprId, Operand)> = Vec::new();
        match args {
            [one] => {
                let x = self.operand(cx, *one);
                match &*self.types.get(x.ty) {
                    Type::Tuple(results) if x.mode == Mode::Value => {
                        given.extend(results.iter().map(|&ty| (*one, Operand::value(ty))));
                    }
                    _ => given.push((*one, self.value(cx, *one))),
                }
            }
            many => {
                for &arg in many {
                    given.push((arg, self.value(cx, arg)));
                }
            }
        }
        let fun_expr = fun;
        let fun = self.expr_string(cx, fun);
        let span = cx.tree.exprs[call].span;
        let after_args = args
            .last()
            .map_or(span.lo, |&last| cx.tree.exprs[last].span.hi);
        // Where `...` or the closing parenthesis is written.
        let token = |token: &str| {
            let rest = &cx.text[after_args as usize..span.hi as usize];
            let lo = after_args + rest.find(token).unwrap_or(0) as u32;
            Span {
                lo,
                hi: lo + token.len() as u32,
                ..span
            }
        };
        let (count, mut wanted) = (given.len(), params.len());
        // The type that the argument at `at` is given to: the element
        // type for the arguments of `...T`.
        let mut rest = None;
        if variadic {
            if spread {
                if args.len() == 1 && count > 1 {
                    let message = format!(
                        "cannot use ... with {count}-valued {}",
                        self.expr_string(cx, args[0])
                    );
                    self.error(Code::CannotUseEllipsis, token("..."), message);
                    return None;
                }
            } else if count + 1 >= wanted {
                rest = match params.last().map(|&last| self.types.get(last)).as_deref() {
                    Some(Type::Slice(elem)) => Some(*elem),
                    _ => return None,
                };
                wanted = count;
            } else {
                wanted -= 1;
            }
        } else if spread {
            let message = format!("cannot use ... in call to non-variadic {fun}");
            self.error(Code::CannotUseEllipsis, token("..."), message);
            return None;
        }
        if count != wanted {
            // An argument that is in error may stand for any number.
            if args.len() == 1 && given.iter().any(|(_, x)| x.mode == Mode::Unknown) {
                return None;
            }
            let (code, qualifier, at) = match count > wanted {
                true => (
                    Code::TooManyArguments,
                    "too many",
                    cx.tree.exprs[given[wanted].0].span,
                ),
                false => (Code::NotEnoughArguments, "not enough", token(")")),
            };
            let have: Vec<TypeId> = given.iter().map(|(_, x)| x.ty).collect();
            let message = format!("{qualifier} arguments in call to {fun}");
            let diagnostic = Diagnostic::new(code, at, message)
                .with_note(
                    Code::NoteHave,
                    None,
                    format!("have {}", self.types_summary(&have, false, spread)),
                )
                .with_note(
                    Code::NoteWant,
                    None,
                    format!("want {}", self.types_summary(params, variadic, false)),
                );
            if self.report_once((at, code)) {
                self.emit(diagnostic);
            }
            return None;
        }
        // The type that each argument is given to.
        let mut to: Vec<TypeId> = (0..count)
            .map(|at| match (rest, at + 1 >= params.len()) {
                (Some(elem), true) => elem,
                _ => params[at],
            })
            .collect();
        // The type arguments of a generic function are found from the
        // arguments, and say what the parameters are in this call.
        let mut instance = None;
        if let Some(generic) = generic {
            let found = self.instantiated_for(cx, (span, fun_expr), generic, &to, &given)?;
            let Type::Func { params: now, .. } = &*self.types.get(found) else {
                return None;
            };
            for (at, to) in to.iter_mut().enumerate() {
                *to = match (rest, at + 1 >= now.len(), now.last()) {
                    (Some(_), true, Some(&last)) => match &*self.types.get(last) {
                        Type::Slice(elem) => *elem,
                        _ => return None,
                    },
                    _ => now[at],
                };
            }
            instance = Some(found);
        }
        let context = format!("argument to {fun}");
        let several = given.len() > args.len();
        for ((arg, x), to) in given.into_iter().zip(to) {
            match several {
                true => drop(self.init_var(cx, arg, Some(x), Some(to), &context)),
                false => drop(self.assignment(cx, arg, x, Some(to), &context)),
            }
        }
        instance
    }

    /// What the call of `fun` is, once it is typed: a conversion, a
    /// built-in function that only computes a value, or a call that does
    /// something.
    pub(super) fn called(&self, fun: ExprId) -> Called {
        let callee = self.memo.as_ref().and_then(|memo| memo.get(&fun));
        match callee.map(|callee| &callee.mode) {
            Some(Mode::Type) => Called::Conversion,
            Some(Mode::Unsafe(_)) => Called::Computation,
            Some(Mode::Builtin(
                Predeclared::Append
                | Predeclared::Cap
                | Predeclared::ComplexFunc
                | Predeclared::Imag
                | Predeclared::Len
                | Predeclared::Make
                | Predeclared::Max
                | Predeclared::Min
                | Predeclared::New
                | Predeclared::Real,
            )) => Called::Computation,
            _ => Called::Function,
        }
    }

    /// The expression `expr` stands as a statement: it is a call or a
    /// receive, or its value is not used.
    pub(super) fn expr_stmt(&mut self, cx: &Cx<'a>, expr: ExprId) {
        let x = self.operand(cx, expr);
        let mut inner = expr;
        while let ExprKind::Paren(x) = cx.tree.exprs[inner].kind {
            inner = x;
        }
        let is_statement = match &cx.tree.exprs[inner].kind {
            ExprKind::Call { fun, .. } => self.called(*fun) == Called::Function,
            ExprKind::Unary {
                op: UnaryOp::Recv, ..
            }
            | ExprKind::MacroCall { .. } => true,
            _ => false,
        };
        let (code, what) = match x.mode {
            Mode::Unknown => return,
            Mode::Builtin(_) | Mode::Unsafe(_) => (Code::BuiltinNotCalled, "must be called"),
            Mode::Type => (Code::NotAnExpression, "is not an expression"),
            _ if is_statement => return,
            _ => (Code::UnusedExpression, "is not used"),
        };
        let message = format!("{} {what}", self.describe(cx, expr, &x));
        self.error(code, cx.tree.exprs[expr].span, message);
    }
}
