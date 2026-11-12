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
        // What a type parameter takes is known when the function is used.
        if self.types.has_param(target) || self.types.has_param(x.ty) {
            return Operand::value(target);
        }
        let span = cx.tree.exprs[arg].span;
        let basic = self.basic_of(target);
        let converted = match (&x.mode, basic) {
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
            _ => self.convertible(&x, target).then(|| Operand::value(target)),
        };
        let Some(converted) = converted else {
            // What is in an interface is taken out of it by an assertion.
            let asserted = self.types.is_interface(x.ty)
                && !self.types.is_interface(target)
                && self.types.implements(target, x.ty, self.names);
            let message = format!(
                "cannot convert {} to type {shown}{}",
                self.describe(cx, arg, &x),
                if asserted {
                    ": need type assertion"
                } else {
                    ""
                }
            );
            self.error(Code::CannotConvert, span, message);
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
        converted
    }

    /// Whether the value `x`, which is no constant of a type of constants,
    /// converts to the type `to`. What an interface or a type parameter
    /// takes is not decided here yet.
    fn convertible(&self, x: &Operand, to: TypeId) -> bool {
        if !self.not_assignable(x, to) {
            return true;
        }
        let (from_under, to_under) = (self.types.underlying(x.ty), self.types.underlying(to));
        let (from, target) = (self.types.get(from_under), self.types.get(to_under));
        if self
            .types
            .identical_ignoring_tags(from_under, to_under, self.names)
        {
            return true;
        }
        // Pointers that are no declared types, to types with identical
        // underlying types.
        if let (Type::Pointer(a), Type::Pointer(b)) = (&*self.types.get(x.ty), &*self.types.get(to))
            && self.types.identical_ignoring_tags(
                self.types.underlying(*a),
                self.types.underlying(*b),
                self.names,
            )
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
            (Type::Slice(elem), Type::Array { elem: of, .. }) => {
                self.types.identical(*elem, *of, self.names)
            }
            (Type::Slice(elem), Type::Pointer(array)) => {
                match &*self.types.get(self.types.underlying(*array)) {
                    Type::Array { elem: of, .. } => self.types.identical(*elem, *of, self.names),
                    _ => false,
                }
            }
            _ => false,
        }
    }
}
