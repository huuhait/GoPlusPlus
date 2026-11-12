//! Conversions `T(x)`, after `go/types` (conversions.go).

use super::*;

impl<'a> Checker<'a> {
    /// `T(x)` for the type `target`: a constant where `T` is a type of
    /// constants and holds the constant `x`.
    pub(super) fn conversion(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        target: TypeId,
        args: &[ExprId],
    ) -> Operand {
        let shown = self.type_string(target);
        let arg = match args {
            [arg] => *arg,
            [] => {
                // At the closing parenthesis.
                let span = cx.tree.exprs[call].span;
                let at = Span {
                    lo: span.hi.saturating_sub(1),
                    ..span
                };
                let message = format!("missing argument in conversion to {shown}");
                self.error(Code::NotEnoughArguments, at, message);
                return Operand::UNKNOWN;
            }
            [.., last] => {
                for &arg in args {
                    self.operand(cx, arg);
                }
                let message = format!("too many arguments in conversion to {shown}");
                self.error(Code::TooManyArguments, cx.tree.exprs[*last].span, message);
                return Operand::UNKNOWN;
            }
        };
        let x = self.value(cx, arg);
        // Nothing more is said about a type that is in error, nor about a
        // generic type without its arguments.
        let generic = matches!(&*self.types.get(target), Type::Named { id, args }
            if args.is_empty() && !self.types.type_params(*id).is_empty());
        if x.mode == Mode::Unknown || generic || self.types.underlying(target) == Types::INVALID {
            return Operand::UNKNOWN;
        }
        if self.open(target) || self.open(x.ty) {
            return Operand::value(target);
        }
        use crate::types::Constraint;
        if let Some(Constraint::Comparable | Constraint::Terms) = self.types.constraint_kind(target)
        {
            let message = format!(
                "cannot use interface {shown} in conversion (contains specific type constraints \
                 or is comparable)"
            );
            let span = cx.tree.exprs[call].span;
            self.error(Code::CannotUseConstraintOutside, span, message);
            return Operand::UNKNOWN;
        }
        let span = cx.tree.exprs[arg].span;
        let basic = self.basic_of(target);
        let mut cause = None;
        let converted = match (&x.mode, basic) {
            // A constant converts to a type parameter if it converts to
            // every type that the parameter may be; the result is no
            // constant.
            (Mode::Const(value), _) if self.is_param(target) => {
                cause = self.constant_to_param(cx, arg, &x, value, target);
                cause.is_none().then(|| Operand::value(target))
            }
            (Mode::Const(value), Some(basic)) => {
                let integer = self.is_integer(x.ty);
                match representable(value, basic) {
                    Some(Value::Unknown) => return Operand::UNKNOWN,
                    Some(value) => Some(Operand::constant(target, value)),
                    // An integer converts to the string of that character.
                    None if integer && basic == Basic::String => {
                        let code = value.as_i64().and_then(|code| u32::try_from(code).ok());
                        let c = code
                            .and_then(char::from_u32)
                            .unwrap_or(char::REPLACEMENT_CHARACTER);
                        let value = Value::string(c.encode_utf8(&mut [0; 4]).as_bytes());
                        Some(Operand::constant(target, value))
                    }
                    // Between integers, only the range can be wrong.
                    None if integer && self.is_integer(target) => {
                        let message = format!("constant {value} overflows {shown}");
                        self.error(Code::CannotConvert, span, message);
                        return Operand::UNKNOWN;
                    }
                    None => None,
                }
            }
            _ => self
                .convertible(&x, target, &mut cause)
                .then(|| Operand::value(target)),
        };
        let Some(converted) = converted else {
            // What is converted to a type parameter is no constant, as Go
            // says it.
            let described = match self.is_param(target) && matches!(x.mode, Mode::Const(_)) {
                true => Operand::value(x.ty),
                false => x.clone(),
            };
            let message = format!(
                "cannot convert {} to type {shown}",
                self.describe(cx, arg, &described)
            );
            let cause = match cause {
                Some(text) => Some(super::implements::Cause {
                    text,
                    have_want: None,
                }),
                None => self.unfit_cause(&x, target),
            };
            self.error_with_cause(Code::CannotConvert, span, message, cause);
            return Operand::UNKNOWN;
        };
        // The argument has its type for good: that of the conversion,
        // but its default type in an interface and where the result is
        // no constant, and its own kind for an integer made a string.
        if self.is_untyped(x.ty) {
            let constant = matches!(x.mode, Mode::Const(_));
            let last = if self.types.is_interface(target) || constant && basic.is_none() {
                self.types.default_type(x.ty)
            } else if constant && self.is_integer(x.ty) && basic == Some(Basic::String) {
                x.ty
            } else {
                target
            };
            if last != self.types.basic(Basic::UntypedNil) {
                self.update_expr_type(cx, arg, last, true);
            }
        }
        // A conversion that keeps how the value is held copies it as an
        // assignment does (EXP-2); one that computes a new value only
        // reads.
        let kept = self.types.is_interface(target)
            || self
                .types
                .identical(self.types.underlying(x.ty), self.types.underlying(target));
        if kept {
            self.given(cx, arg, &x, x.ty, place::Given::Stored);
        }
        converted
    }

    /// Why the constant `value` of `x` does not convert to the type
    /// parameter `target`: `None` if it converts to every type that the
    /// parameter may be.
    fn constant_to_param(
        &self,
        cx: &Cx<'a>,
        arg: ExprId,
        x: &Operand,
        value: &Value,
        target: TypeId,
    ) -> Option<String> {
        let shown = self.type_string(target);
        let Some(types) = self.types.specific_types(target) else {
            return Some(format!("{shown} does not contain specific types"));
        };
        let (integer, string) = (self.is_integer(x.ty), self.is_string(x.ty));
        types.iter().find_map(|&(_, under)| {
            let bytes_or_runes = matches!(&*self.types.get(under), Type::Slice(elem)
                if matches!(self.basic_of(*elem), Some(Basic::Uint8 | Basic::Int32)));
            let basic = self.basic_of(under);
            let converts = string && bytes_or_runes
                || basic.is_some_and(|basic| representable(value, basic).is_some())
                || integer && basic == Some(Basic::String);
            if converts {
                return None;
            }
            let of = self.type_string(under);
            Some(match integer && self.is_integer(under) {
                true => format!("constant {value} overflows {of} (in {shown})"),
                false => format!(
                    "cannot convert {} to type {of} (in {shown})",
                    self.describe(cx, arg, x)
                ),
            })
        })
    }

    /// Whether the value `x`, which is no constant of a type of constants,
    /// converts to the type `to`. `cause` gets what there is to say where
    /// a type parameter does not.
    fn convertible(&self, x: &Operand, to: TypeId, cause: &mut Option<String>) -> bool {
        if !self.not_assignable(x, to) {
            return true;
        }
        // A type parameter converts if every type that it may be does.
        let from_types = self
            .types
            .specific_types(x.ty)
            .filter(|_| self.is_param(x.ty));
        let to_types = self.types.specific_types(to).filter(|_| self.is_param(to));
        if self.is_param(x.ty) || self.is_param(to) {
            let (from, target) = (self.type_string(x.ty), self.type_string(to));
            let as_type = |ty: TypeId| Operand {
                mode: x.mode.clone(),
                ty,
            };
            let refuse = |text: String, cause: &mut Option<String>| {
                // What is said of a type inside comes after.
                *cause = Some(match cause.take() {
                    Some(inner) => format!("{text}\n\t{inner}"),
                    None => text,
                });
                false
            };
            return match (from_types, to_types) {
                (Some(from_types), Some(to_types)) => from_types.iter().all(|&(v, _)| {
                    to_types.iter().all(|&(t, _)| {
                        self.convertible(&as_type(v), t, cause) || {
                            let (v, t) = (self.type_string(v), self.type_string(t));
                            let text =
                                format!("cannot convert {v} (in {from}) to type {t} (in {target})");
                            refuse(text, cause)
                        }
                    })
                }),
                (Some(from_types), None) if !self.is_param(to) => {
                    from_types.iter().all(|&(v, _)| {
                        self.convertible(&as_type(v), to, cause) || {
                            let v = self.type_string(v);
                            refuse(
                                format!("cannot convert {v} (in {from}) to type {target}"),
                                cause,
                            )
                        }
                    })
                }
                (None, Some(to_types)) if !self.is_param(x.ty) => to_types.iter().all(|&(t, _)| {
                    self.convertible(x, t, cause) || {
                        let t = self.type_string(t);
                        refuse(
                            format!("cannot convert {from} to type {t} (in {target})"),
                            cause,
                        )
                    }
                }),
                _ => false,
            };
        }
        let (from_under, to_under) = (self.types.underlying(x.ty), self.types.underlying(to));
        let (from, target) = (self.types.get(from_under), self.types.get(to_under));
        if self.types.identical_ignoring_tags(from_under, to_under) {
            return true;
        }
        // Pointers that are no declared types, to types with identical
        // underlying types.
        if let (Type::Pointer(a), Type::Pointer(b)) = (&*self.types.get(x.ty), &*self.types.get(to))
            && self
                .types
                .identical_ignoring_tags(self.types.underlying(*a), self.types.underlying(*b))
        {
            return true;
        }
        let number = |basic: Basic| {
            use Basic::*;
            !matches!(
                basic,
                Bool | String
                    | Complex64
                    | Complex128
                    | UntypedBool
                    | UntypedString
                    | UntypedComplex
                    | UntypedNil
            )
        };
        let complex = |basic: Basic| {
            matches!(
                basic,
                Basic::Complex64 | Basic::Complex128 | Basic::UntypedComplex
            )
        };
        let string = |ty: &Type| matches!(ty, Type::Basic(Basic::String | Basic::UntypedString));
        let bytes_or_runes = |ty: &Type| match ty {
            Type::Slice(elem) => matches!(self.basic_of(*elem), Some(Basic::Uint8 | Basic::Int32)),
            _ => false,
        };
        let pointer = |ty: &Type| matches!(ty, Type::Pointer(_));
        let uintptr = |ty: &Type| matches!(ty, Type::Basic(Basic::Uintptr));
        let (unsafe_from, unsafe_to) = (
            self.types.is_unsafe_pointer(x.ty, self.names),
            self.types.is_unsafe_pointer(to, self.names),
        );
        match (&*from, &*target) {
            (Type::Basic(a), Type::Basic(b)) if number(*a) && number(*b) => true,
            (Type::Basic(a), Type::Basic(b)) if complex(*a) && complex(*b) => true,
            (from, to) if (self.is_integer(from_under) || bytes_or_runes(from)) && string(to) => {
                true
            }
            (from, to) if string(from) && bytes_or_runes(to) => true,
            (from, _) if (pointer(from) || uintptr(from)) && unsafe_to => true,
            (_, to) if unsafe_from && (pointer(to) || uintptr(to)) => true,
            // A slice to an array, or to a pointer to one, of its elements.
            (Type::Slice(elem), Type::Array { elem: of, .. }) => self.types.identical(*elem, *of),
            (Type::Slice(elem), Type::Pointer(array)) => {
                match &*self.types.get(self.types.underlying(*array)) {
                    Type::Array { elem: of, .. } => self.types.identical(*elem, *of),
                    _ => false,
                }
            }
            _ => false,
        }
    }
}
