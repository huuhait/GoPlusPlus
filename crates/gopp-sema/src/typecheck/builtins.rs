//! Calls of the predeclared functions, after `go/types` (builtins.go).

use super::*;

/// How many arguments a built-in function takes at the least, and whether
/// it takes more.
fn arity(builtin: Predeclared) -> Option<(usize, bool)> {
    use Predeclared::*;
    Some(match builtin {
        Append | Make | Max | Min => (1, true),
        Cap | Clear | Close | Imag | Len | New | Panic | Real => (1, false),
        Move | NewUnique | NewShared | NewWeak => (1, false),
        ComplexFunc | CopyFunc | Delete => (2, false),
        Print | Println => (0, true),
        Recover => (0, false),
        _ => return None,
    })
}

impl<'a> Checker<'a> {
    /// Where `token` is written after the arguments `args` of `call`.
    fn after_args(&self, cx: &Cx<'a>, call: ExprId, args: &[ExprId], token: &str) -> Span {
        let span = cx.tree.exprs[call].span;
        let from = args
            .last()
            .map_or(span.lo, |&last| cx.tree.exprs[last].span.hi);
        let rest = &cx.text[from as usize..span.hi as usize];
        let lo = from + rest.find(token).unwrap_or(0) as u32;
        Span {
            lo,
            hi: lo + token.len() as u32,
            ..span
        }
    }

    /// What every call of a built-in function is checked for: no `...`,
    /// arguments that are values, and their count. The arguments, or
    /// `None` if the call is in error.
    pub(super) fn builtin_arguments(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        name: &str,
        (least, more): (usize, bool),
        args: &[ExprId],
        (spread, types_first): (bool, bool),
    ) -> Option<Vec<(ExprId, Operand)>> {
        if spread && name != "append" {
            let message = format!("invalid operation: invalid use of ... with built-in {name}");
            let at = self.after_args(cx, call, args, "...");
            self.error(Code::InvalidUseOfDots, at, message);
            for &arg in args {
                self.operand(cx, arg);
            }
            return None;
        }
        // `f(g())` passes all results of `g`; `make` and `new` take a type.
        let mut given: Vec<(ExprId, Operand)> = Vec::new();
        let mut count = args.len();
        if !types_first {
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
            if given.iter().any(|(_, x)| x.mode == Mode::Unknown) {
                return None;
            }
            count = given.len();
        }
        let wrong = match (count < least, !more && count > least) {
            (true, _) => "not enough",
            (_, true) => "too many",
            _ => return Some(given),
        };
        let message = format!(
            "invalid operation: {wrong} arguments for {} (expected {least}, found {count})",
            self.expr_string(cx, call)
        );
        let code = match count < least {
            true => Code::BuiltinNotEnoughArguments,
            false => Code::BuiltinTooManyArguments,
        };
        let at = self.after_args(cx, call, args, ")");
        self.error(code, at, message);
        None
    }

    /// The element type of the slice type of `x`: of a type parameter,
    /// the one element type of the slice types that it may be.
    fn slice_elem(&self, cx: &Cx<'a>, expr: ExprId, x: &Operand) -> Result<TypeId, String> {
        let no_slice = || match x.ty == self.types.basic(Basic::UntypedNil) {
            true => "argument must be a slice; have untyped nil".to_owned(),
            false => format!(
                "argument must be a slice; have {}",
                self.describe(cx, expr, x)
            ),
        };
        let types = self.types.specific_types(x.ty).ok_or_else(no_slice)?;
        let mut shared = None;
        for (_, under) in types {
            let Type::Slice(elem) = &*self.types.get(under) else {
                return Err(no_slice());
            };
            match shared {
                None => shared = Some(*elem),
                Some(first) if self.types.identical(first, *elem) => {}
                Some(first) => {
                    return Err(format!(
                        "mismatched slice element types {} and {} in {}",
                        self.type_string(first),
                        self.type_string(*elem),
                        self.describe(cx, expr, x)
                    ));
                }
            }
        }
        shared.ok_or_else(no_slice)
    }

    /// Whether what holds for the type is not known here.
    fn unsettled(&self, ty: TypeId) -> bool {
        self.open(ty)
    }

    /// Whether `x` is of a type parameter whose types all fit the built-in
    /// function `name`, by `fits`: the function takes no such argument,
    /// which is reported.
    fn unsupported_param(
        &mut self,
        cx: &Cx<'a>,
        at: ExprId,
        x: &Operand,
        name: &str,
        fits: fn(&Self, TypeId) -> bool,
    ) -> bool {
        let unsupported = self.is_param(x.ty) && self.all_types(x.ty, fits);
        if unsupported {
            let message = format!(
                "{} not supported as argument to built-in {name} for go1.18 (see \
                 go.dev/issue/50937)",
                self.describe(cx, at, x)
            );
            self.error(
                Code::BuiltinInvalidArgument,
                cx.tree.exprs[at].span,
                message,
            );
        }
        unsupported
    }

    /// Whether `holds` for the underlying type of the type; for a type
    /// parameter, of every type that it may be, of which its constraint
    /// names one at least.
    fn under_is(&self, ty: TypeId, holds: impl Fn(&Type) -> bool) -> bool {
        self.types.specific_types(ty).is_some_and(|types| {
            types
                .iter()
                .all(|&(_, under)| holds(&self.types.get(under)))
        })
    }

    fn invalid_argument(&mut self, cx: &Cx<'a>, expr: ExprId, message: String) -> Operand {
        self.error(
            Code::BuiltinInvalidArgument,
            cx.tree.exprs[expr].span,
            message,
        );
        Operand::UNKNOWN
    }

    pub(super) fn builtin(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        fun: ExprId,
        builtin: Predeclared,
        args: &[ExprId],
        spread: bool,
    ) -> Operand {
        use Predeclared::*;
        let Some(arity) = arity(builtin) else {
            for &arg in args {
                self.operand(cx, arg);
            }
            return Operand::UNKNOWN;
        };
        let name = builtin.name();
        let no_value = Operand {
            mode: Mode::NoValue,
            ty: Types::INVALID,
        };
        let types_first = matches!(builtin, Make | New);
        // The length of an array is a constant if nothing is called or
        // received to get the array.
        let outer = std::mem::replace(&mut self.called_or_received, false);
        let given = self.builtin_arguments(cx, call, name, arity, args, (spread, types_first));
        let is_static = !std::mem::replace(&mut self.called_or_received, outer);
        let Some(mut given) = given else {
            return Operand::UNKNOWN;
        };
        // What works on a slice, a map or a number works on what a
        // pointer to one points to.
        let read = match builtin {
            Append | Cap | Clear | Close | Delete | Len => 1,
            ComplexFunc | Imag | Max | Min | Real => given.len(),
            _ => 0,
        };
        for (_, x) in given.iter_mut().take(read) {
            *x = self.read(cx, x.clone());
        }
        let (int, any) = (self.types.basic(Basic::Int), self.types.any());
        let first = given.first().cloned();
        match (builtin, first) {
            (Append, Some((at, x))) => {
                if self.unsettled(x.ty) {
                    return Operand::value(x.ty);
                }
                let byte = self.types.byte();
                // `append(bytes, "text"...)`.
                let text = given.get(1).filter(|_| spread && given.len() == 2);
                // The text may be of a type parameter whose types are
                // strings and slices of bytes, with a string among them.
                let bytes = |this: &Self, ty: TypeId| {
                    matches!(&*this.types.get(this.types.underlying(ty)),
                        Type::Slice(elem) if this.basic_of(*elem) == this.basic_of(byte))
                };
                let appends_text = text.is_some_and(|(_, y)| {
                    let types = self.types.specific_types(y.ty).unwrap_or_default();
                    let text = |&(ty, _): &(TypeId, TypeId)| self.is_string(ty) || bytes(self, ty);
                    types.iter().all(text) && types.iter().any(|&(ty, _)| self.is_string(ty))
                }) && bytes(self, x.ty);
                if appends_text {
                    return Operand::value(x.ty);
                }
                let elem = match self.slice_elem(cx, at, &x) {
                    Ok(elem) => elem,
                    Err(why) => {
                        let message = format!("invalid append: {why}");
                        self.error(
                            Code::BuiltinInvalidArgument,
                            cx.tree.exprs[at].span,
                            message,
                        );
                        return Operand::UNKNOWN;
                    }
                };
                let params = [x.ty, self.types.slice(elem)];
                self.arguments(cx, call, fun, (&params, true), args, spread);
                Operand::value(x.ty)
            }
            (Cap | Len, Some((at, x))) => {
                let under = self.types.underlying(x.ty);
                let array = match &*self.types.get(under) {
                    Type::Pointer(elem) => match &*self.types.get(self.types.underlying(*elem)) {
                        Type::Array { .. } => self.types.underlying(*elem),
                        _ => under,
                    },
                    _ => under,
                };
                let is_len = builtin == Len;
                match (&*self.types.get(array), &x.mode) {
                    (Type::Basic(Basic::String | Basic::UntypedString), mode) if is_len => {
                        match mode {
                            // The length of a constant string is a constant.
                            Mode::Const(Value::Str(text)) => {
                                let len = Value::Int64(text.bytes().len() as i64);
                                Operand::constant(int, len)
                            }
                            _ => Operand::value(int),
                        }
                    }
                    (Type::Array { len, .. }, _) if is_static => {
                        Operand::constant(int, Value::Int64(*len as i64))
                    }
                    (Type::Array { .. } | Type::Slice(_) | Type::Chan { .. }, _) => {
                        Operand::value(int)
                    }
                    (Type::Map { .. }, _) if is_len => Operand::value(int),
                    _ if under == Types::INVALID => Operand::UNKNOWN,
                    _ if self.unsettled(x.ty) => Operand::value(int),
                    // Every type that a type parameter may be has a
                    // length.
                    (Type::Param(_), _)
                        if self.under_is(x.ty, |under| {
                            let sized = |ty: &Type| match ty {
                                Type::Basic(Basic::String) | Type::Map { .. } => is_len,
                                Type::Array { .. } | Type::Slice(_) | Type::Chan { .. } => true,
                                _ => false,
                            };
                            match under {
                                Type::Pointer(elem) => {
                                    let array = self.types.get(self.types.underlying(*elem));
                                    matches!(&*array, Type::Array { .. })
                                }
                                other => sized(other),
                            }
                        }) =>
                    {
                        Operand::value(int)
                    }
                    _ => {
                        let message = format!(
                            "invalid argument: {} for built-in {name}",
                            self.describe(cx, at, &x)
                        );
                        self.invalid_argument(cx, at, message)
                    }
                }
            }
            (Clear, Some((at, x))) => {
                let clears = |under: &Type| matches!(under, Type::Map { .. } | Type::Slice(_));
                if !self.unsettled(x.ty) && !self.under_is(x.ty, clears) {
                    let message = format!(
                        "invalid argument: cannot clear {}: argument must be (or constrained by) \
                         map or slice",
                        self.describe(cx, at, &x)
                    );
                    return self.invalid_argument(cx, at, message);
                }
                self.changed(cx, at, place::Change::Emptied("clear"));
                self.wrote(cx, at);
                self.shared_changed(cx, at, "clear");
                // A slice keeps its length, with zero values in it.
                if let Type::Slice(elem) = &*self.types.get(self.types.underlying(x.ty)) {
                    let text = self.expr_string(cx, at);
                    let doing = format!("cannot clear {text}");
                    let fix = format!("use {text} = {text}[:0], which drops the elements");
                    self.zero_needed(cx, cx.tree.exprs[at].span, *elem, &doing, &fix);
                }
                no_value
            }
            (Close, Some((at, x))) => {
                if self.unsettled(x.ty) {
                    return no_value;
                }
                // Of the types that a type parameter may be, the first
                // that is not closed says what is wrong.
                let types = self.types.specific_types(x.ty).unwrap_or_default();
                let wrong = types
                    .iter()
                    .find_map(|&(_, under)| match &*self.types.get(under) {
                        Type::Chan {
                            dir: ChanDir::Recv, ..
                        } => Some("receive-only channel"),
                        Type::Chan { .. } => None,
                        _ => Some("non-channel"),
                    });
                let wrong = match (wrong, types.is_empty()) {
                    (Some(wrong), _) => wrong,
                    (None, true) => "non-channel",
                    (None, false) => {
                        self.close_of_nil(cx, at);
                        return no_value;
                    }
                };
                let message = format!(
                    "invalid operation: cannot close {wrong} {}",
                    self.describe(cx, at, &x)
                );
                self.invalid_argument(cx, at, message)
            }
            (ComplexFunc, Some((at, x))) => self.complex(cx, call, (at, x), given[1].clone()),
            (CopyFunc, Some((at, x))) => {
                let (from, y) = given[1].clone();
                // In a `.gopp` file a slice is a value: what is written
                // is reached through a pointer.
                let by_value = self.own_rules(cx)
                    && matches!(
                        &*self.types.get(self.types.underlying(x.ty)),
                        Type::Slice(_)
                    );
                if by_value {
                    let message = format!(
                        "invalid copy: the destination {} is passed as a copy",
                        self.describe(cx, at, &x)
                    );
                    let span = cx.tree.exprs[at].span;
                    match x.mode {
                        Mode::Variable => {
                            let fix = format!("use &{}", self.expr_string(cx, at));
                            let fix = (Code::NoteOwnFixAddress, fix);
                            self.error_with_fix(Code::OwnCopyToCopy, span, message, fix);
                        }
                        _ => self.error(Code::OwnCopyToCopy, span, message),
                    }
                    return Operand::UNKNOWN;
                }
                let (x, y) = (self.read(cx, x), self.read(cx, y));
                if self.unsettled(x.ty) || self.unsettled(y.ty) {
                    return Operand::value(int);
                }
                let byte = self.types.byte();
                // Bytes are copied from a string too: into what a slice
                // of bytes takes, from a type whose types are strings and
                // slices of bytes.
                let text = |under: &Type| match under {
                    Type::Slice(elem) => self.types.identical(*elem, byte),
                    Type::Basic(basic) => matches!(basic, Basic::String | Basic::UntypedString),
                    _ => false,
                };
                if !self.not_assignable(&x, self.types.slice(byte)) && self.under_is(y.ty, text) {
                    return Operand::value(int);
                }
                let to_elem = match self.slice_elem(cx, at, &x) {
                    Ok(elem) => elem,
                    Err(why) => {
                        return self.invalid_argument(cx, at, format!("invalid copy: {why}"));
                    }
                };
                let from_elem = match self.slice_elem(cx, from, &y) {
                    Ok(elem) => elem,
                    Err(_) if self.all_types(y.ty, Self::is_string) => byte,
                    Err(why) => {
                        return self.invalid_argument(cx, from, format!("invalid copy: {why}"));
                    }
                };
                if !self.types.identical(to_elem, from_elem) {
                    let message = format!(
                        "invalid copy: arguments {} and {} have different element types {} and {}",
                        self.describe(cx, at, &x),
                        self.describe(cx, from, &y),
                        self.type_string(to_elem),
                        self.type_string(from_elem)
                    );
                    return self.invalid_argument(cx, at, message);
                }
                Operand::value(int)
            }
            (Delete, Some((at, x))) => {
                if self.unsettled(x.ty) {
                    return no_value;
                }
                // Every type that a type parameter may be is a map, all
                // with one key type.
                let types = self.types.specific_types(x.ty).unwrap_or_default();
                let keys: std::option::Option<Vec<TypeId>> = types
                    .iter()
                    .map(|&(_, under)| match &*self.types.get(under) {
                        Type::Map { key, .. } => Some(*key),
                        _ => None,
                    })
                    .collect();
                let shown = self.describe(cx, at, &x);
                let key = match keys.as_deref() {
                    Some([key, rest @ ..]) => {
                        if !rest.iter().all(|&other| self.types.identical(*key, other)) {
                            let message = format!(
                                "invalid argument: maps of {shown} must have identical key types"
                            );
                            return self.invalid_argument(cx, at, message);
                        }
                        *key
                    }
                    _ => {
                        let message = format!("invalid argument: {shown} is not a map");
                        return self.invalid_argument(cx, at, message);
                    }
                };
                self.changed(cx, at, place::Change::Emptied("delete"));
                self.wrote(cx, at);
                self.shared_changed(cx, at, "delete from");
                self.lost_entry(cx, at, at);
                let (key_at, key_value) = given[1].clone();
                let assigned =
                    self.assignment(cx, key_at, key_value, Some(key), "argument to delete");
                match assigned.mode {
                    Mode::Unknown => Operand::UNKNOWN,
                    _ => no_value,
                }
            }
            (Imag | Real, Some((at, x))) => self.real_or_imag(cx, builtin == Real, at, x),
            (Make, _) => self.make(cx, call, args),
            (Move, Some((at, x))) => self.moved(cx, at, x),
            (NewUnique, Some((at, x))) => self.on_heap(cx, name, Type::Unique, at, x),
            (NewShared, Some((at, x))) => self.on_heap(cx, name, Type::Shared, at, x),
            (NewWeak, Some((at, x))) => match &*self.types.get(self.types.underlying(x.ty)) {
                Type::Shared(elem) => {
                    let weak = self.types.handle(Type::Weak, *elem);
                    weak.map_or(Operand::UNKNOWN, Operand::value)
                }
                _ if self.unsettled(x.ty) => Operand::UNKNOWN,
                _ => {
                    let message = format!(
                        "invalid argument: {} is not a Shared[T]",
                        self.describe(cx, at, &x)
                    );
                    self.error(Code::OwnWeakArg, cx.tree.exprs[at].span, message);
                    Operand::UNKNOWN
                }
            },
            (Max | Min, Some(_)) => self.min_or_max(cx, builtin == Max, given),
            (New, _) => match self.var_type(cx, args[0]) {
                invalid if invalid == Types::INVALID => Operand::UNKNOWN,
                ty => Operand::value(self.types.pointer(ty)),
            },
            (Panic, Some((at, x))) => {
                match self
                    .assignment(cx, at, x, Some(any), "argument to panic")
                    .mode
                {
                    Mode::Unknown => Operand::UNKNOWN,
                    _ => no_value,
                }
            }
            (Print | Println, _) => {
                let context = format!("argument to built-in {name}");
                let mut known = true;
                for (at, x) in given {
                    known &= self.assignment(cx, at, x, None, &context).mode != Mode::Unknown;
                }
                match known {
                    true => no_value,
                    false => Operand::UNKNOWN,
                }
            }
            (Recover, _) => Operand::value(any),
            _ => Operand::UNKNOWN,
        }
    }

    /// `complex(re, im)`.
    fn complex(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        (re_at, mut re): (ExprId, Operand),
        (im_at, mut im): (ExprId, Operand),
    ) -> Operand {
        if self.unsettled(re.ty) || self.unsettled(im.ty) {
            return Operand::UNKNOWN;
        }
        // An argument of no type takes the type of the other; two
        // constants of no type are floats if they have no imaginary part.
        match (self.is_untyped(re.ty), self.is_untyped(im.ty)) {
            (false, false) => {}
            (true, false) => {
                self.convert_untyped(cx, re_at, &mut re, im.ty, true);
            }
            (false, true) => {
                self.convert_untyped(cx, im_at, &mut im, re.ty, true);
            }
            (true, true) => {
                let float = self.types.basic(Basic::UntypedFloat);
                match (&re.mode, &im.mode) {
                    (Mode::Const(a), Mode::Const(b)) => {
                        let real = |value: &Value| value.to_complex().imag().sign() == 0;
                        if self.is_numeric(re.ty) && real(a) {
                            re.ty = float;
                        }
                        if self.is_numeric(im.ty) && real(b) {
                            im.ty = float;
                        }
                    }
                    _ => {
                        let float64 = self.types.basic(Basic::Float64);
                        self.convert_untyped(cx, re_at, &mut re, float64, true);
                        self.convert_untyped(cx, im_at, &mut im, float64, true);
                    }
                }
            }
        }
        if re.mode == Mode::Unknown || im.mode == Mode::Unknown {
            return Operand::UNKNOWN;
        }
        if !self.types.identical(re.ty, im.ty) {
            let message = format!(
                "invalid operation: {} (mismatched types {} and {})",
                self.expr_string(cx, call),
                self.type_string(re.ty),
                self.type_string(im.ty)
            );
            return self.invalid_argument(cx, re_at, message);
        }
        let float = |this: &Self, ty: TypeId| {
            matches!(
                this.basic_of(ty),
                Some(Basic::Float32 | Basic::Float64 | Basic::UntypedFloat)
            )
        };
        if self.unsupported_param(cx, re_at, &re, "complex", float) {
            return Operand::UNKNOWN;
        }
        let result = match self.basic_of(re.ty) {
            Some(Basic::Float32) => Basic::Complex64,
            Some(Basic::Float64) => Basic::Complex128,
            Some(Basic::UntypedFloat) => Basic::UntypedComplex,
            _ => {
                let message = format!(
                    "invalid argument: arguments have type {}, expected floating-point",
                    self.type_string(re.ty)
                );
                return self.invalid_argument(cx, re_at, message);
            }
        };
        let ty = self.types.basic(result);
        match (&re.mode, &im.mode) {
            (Mode::Const(a), Mode::Const(b)) => {
                let imaginary = b.to_float().make_imag();
                let value = a.to_float().binary(constant::BinaryOp::Add, &imaginary);
                self.fitted(ty, value)
            }
            _ => Operand::value(ty),
        }
    }

    /// `real(z)` and `imag(z)`.
    fn real_or_imag(&mut self, cx: &Cx<'a>, real: bool, at: ExprId, mut x: Operand) -> Operand {
        if self.unsettled(x.ty) {
            return Operand::UNKNOWN;
        }
        let name = if real { "real" } else { "imag" };
        if self.unsupported_param(cx, at, &x, name, Self::is_complex) {
            return Operand::UNKNOWN;
        }
        if self.is_untyped(x.ty) {
            match &x.mode {
                Mode::Const(_) if self.is_numeric(x.ty) => {
                    x.ty = self.types.basic(Basic::UntypedComplex);
                }
                Mode::Const(_) => {}
                _ => {
                    let complex = self.types.basic(Basic::Complex128);
                    if !self.convert_untyped(cx, at, &mut x, complex, true) {
                        return Operand::UNKNOWN;
                    }
                }
            }
        }
        let part = match self.basic_of(x.ty) {
            Some(Basic::Complex64) => Basic::Float32,
            Some(Basic::Complex128) => Basic::Float64,
            Some(Basic::UntypedComplex) => Basic::UntypedFloat,
            _ => {
                let message = format!(
                    "invalid argument: argument has type {}, expected complex type",
                    self.type_string(x.ty)
                );
                return self.invalid_argument(cx, at, message);
            }
        };
        let ty = self.types.basic(part);
        match &x.mode {
            Mode::Const(value) => {
                let complex = value.to_complex();
                let part = if real { complex.real() } else { complex.imag() };
                self.fitted(ty, part)
            }
            _ => Operand::value(ty),
        }
    }

    /// `NewUnique(v)` and `NewShared(v)`: the value `v` on the heap.
    fn on_heap(
        &mut self,
        cx: &Cx<'a>,
        name: &str,
        make: fn(TypeId) -> Type,
        at: ExprId,
        x: Operand,
    ) -> Operand {
        let context = format!("argument to {name}");
        let x = self.assignment(cx, at, x, None, &context);
        if x.mode == Mode::Unknown {
            return Operand::UNKNOWN;
        }
        if x.ty == self.types.basic(Basic::UntypedNil) {
            let message = format!("use of untyped nil in {context}");
            self.error(Code::UseOfUntypedNil, cx.tree.exprs[at].span, message);
            return Operand::UNKNOWN;
        }
        // A pointer that borrows does not live on the heap, which is said
        // where borrows are checked.
        let handle = self.types.handle(make, x.ty);
        handle.map_or(Operand::UNKNOWN, Operand::value)
    }

    /// `move(x)`: the value of the local variable or parameter `x`.
    fn moved(&mut self, cx: &Cx<'a>, at: ExprId, x: Operand) -> Operand {
        let mut name = at;
        while let ExprKind::Paren(inner) = cx.tree.exprs[name].kind {
            name = inner;
        }
        let denotes = match cx.tree.exprs[name].kind {
            ExprKind::Name(_) => cx.body.uses.get(&name),
            _ => None,
        };
        let shown = self.describe(cx, at, &x);
        let span = cx.tree.exprs[at].span;
        let local = match (denotes, &x.mode) {
            (Some(&Ref::Local(local)), Mode::Variable) => Some(local),
            _ => None,
        };
        // A variable of a plain range clause that is not copied is the
        // element, which stays where it is.
        let binding = local.and_then(|local| self.bindings.get(&local).copied());
        let no_copy = self.types.category(x.ty) == crate::types::Category::NoCopy;
        if let (Some(place::Binding::Element | place::Binding::Key), true) = (binding, no_copy) {
            let text = self.expr_string(cx, name);
            let message = format!("cannot move {shown}: the loop only reads {text}");
            let fix = format!(
                "write &{text} in the range clause, and use mem.Take(&{text}) or \
                 mem.Replace(&{text}, v)"
            );
            match binding {
                Some(place::Binding::Element) => {
                    let fix = (Code::NoteOwnFixAlias, fix);
                    self.error_with_fix(Code::OwnMoveRangeBorrowed, span, message, fix);
                }
                _ => self.error(Code::OwnMoveRangeBorrowed, span, message),
            }
            return Operand::UNKNOWN;
        }
        let code = match (local, &x.mode) {
            (Some(_), _) if self.place_of(cx, name) != place::Place::Elsewhere => {
                use crate::types::NotCopied;
                if let Some((part, NotCopied::Pinned)) = self.types.not_copied(x.ty, true) {
                    let cause = self.not_copied_text(part, NotCopied::Pinned, true);
                    let message = format!("cannot move {shown}: {cause}, and stays in its place");
                    self.error(Code::OwnPinnedMove, span, message);
                    return Operand::UNKNOWN;
                }
                if self.memo.is_some() {
                    self.var_modes.insert(name, crate::object::Transfer::Move);
                }
                return Operand::value(x.ty);
            }
            (_, Mode::Variable) => Code::MoveNonlocal,
            _ => Code::OwnMoveNotPlace,
        };
        let message = format!("cannot move {shown}: move takes a local variable or a parameter");
        // The value of another place is taken out of it.
        match x.mode {
            Mode::Variable => {
                let fix = format!("use mem.Take(&{})", self.expr_string(cx, at));
                self.error_with_fix(code, span, message, (Code::NoteOwnFixTake, fix));
            }
            _ => self.error(code, span, message),
        }
        Operand::UNKNOWN
    }

    /// `make(T, sizes...)`.
    fn make(&mut self, cx: &Cx<'a>, call: ExprId, args: &[ExprId]) -> Operand {
        let ty = self.var_type(cx, args[0]);
        let sizes = &args[1..];
        if self.unsettled(ty) {
            for &size in sizes {
                self.operand(cx, size);
            }
            return Operand::value(ty);
        }
        // What is made is what all the types of a type parameter share.
        let made = self.common_under(ty, |this, of| match of {
            Some((_, under)) => match &*this.types.get(under) {
                Type::Slice(_) | Type::Map { .. } | Type::Chan { .. } => None,
                _ => Some("type must be slice, map, or channel".to_owned()),
            },
            None => Some("no specific type".to_owned()),
        });
        let least = match made.as_ref().map(|&made| self.types.get(made)).as_deref() {
            Ok(Type::Slice(_)) => 2,
            Ok(_) => 1,
            Err(_) => {
                for &size in sizes {
                    self.operand(cx, size);
                }
                let message = format!(
                    "invalid argument: cannot make {}: {}",
                    self.expr_string(cx, args[0]),
                    made.err().unwrap_or_default()
                );
                return self.invalid_argument(cx, args[0], message);
            }
        };
        if args.len() < least || least + 1 < args.len() {
            for &size in sizes {
                self.operand(cx, size);
            }
            let message = format!(
                "invalid operation: {} expects {least} or {} arguments; found {}",
                self.expr_string(cx, call),
                least + 1,
                args.len()
            );
            self.error(
                Code::BuiltinInvalidArgument,
                cx.tree.exprs[call].span,
                message,
            );
            return Operand::UNKNOWN;
        }
        let known: Vec<u64> = sizes
            .iter()
            .filter_map(|&size| self.index_value(cx, size, None))
            .collect();
        if let [length, capacity] = known[..]
            && length > capacity
        {
            let message = "invalid argument: length and capacity swapped".to_owned();
            self.error(
                Code::BuiltinInvalidArgument,
                cx.tree.exprs[args[1]].span,
                message,
            );
        }
        // A slice with a length holds zero values.
        if let (Type::Slice(elem), Some(&length)) =
            (&*self.types.get(self.types.underlying(ty)), sizes.first())
            && self.index_value(cx, length, None) != Some(0)
        {
            let shown = self.type_string(ty);
            let doing = format!("cannot make a {shown} with a length");
            let fix = format!("use make({shown}, 0, n) and append the elements");
            self.zero_needed(cx, cx.tree.exprs[call].span, *elem, &doing, &fix);
        }
        Operand::value(ty)
    }

    /// `min(x, y...)` and `max(x, y...)`.
    fn min_or_max(&mut self, cx: &Cx<'a>, max: bool, given: Vec<(ExprId, Operand)>) -> Operand {
        let name = if max { "max" } else { "min" };
        if given.iter().any(|(_, x)| self.unsettled(x.ty)) {
            return Operand::UNKNOWN;
        }
        let mut exprs = Vec::new();
        let mut best: Option<(ExprId, Operand)> = None;
        for (at, mut a) in given {
            exprs.push(at);
            let ordered = |this: &Self, ty: TypeId| {
                this.is_numeric(ty) && !this.is_complex(ty) || this.is_string(ty)
            };
            if !self.all_types(a.ty, ordered) {
                let message = format!(
                    "invalid argument: {} cannot be ordered",
                    self.describe(cx, at, &a)
                );
                return self.invalid_argument(cx, at, message);
            }
            let Some((best_at, mut x)) = best.take() else {
                best = Some((at, a));
                continue;
            };
            self.match_types(cx, (best_at, &mut x), (at, &mut a));
            if x.mode == Mode::Unknown {
                return Operand::UNKNOWN;
            }
            if !self.types.identical(x.ty, a.ty) {
                let message = format!(
                    "invalid argument: mismatched types {} (previous argument) and {} (type of {})",
                    self.type_string(x.ty),
                    self.type_string(a.ty),
                    self.expr_string(cx, at)
                );
                return self.invalid_argument(cx, at, message);
            }
            best = Some(match (&x.mode, &a.mode) {
                (Mode::Const(old), Mode::Const(new)) => {
                    let op = if max { CompareOp::Gt } else { CompareOp::Lt };
                    match new.compare(op, old) {
                        true => (at, a),
                        false => (best_at, x),
                    }
                }
                _ => (best_at, Operand::value(x.ty)),
            });
        }
        let Some((at, mut x)) = best else {
            return Operand::UNKNOWN;
        };
        // A result that is no constant has a type of its own.
        if !matches!(x.mode, Mode::Const(_)) {
            let context = format!("argument to built-in {name}");
            let any = self.types.any();
            x = self.assignment(cx, at, Operand::value(x.ty), Some(any), &context);
            if x.mode == Mode::Unknown {
                return Operand::UNKNOWN;
            }
            x = Operand::value(x.ty);
        }
        for expr in exprs {
            self.update_expr_type(cx, expr, x.ty, true);
        }
        x
    }
}
