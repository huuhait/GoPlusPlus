//! Operators: what Go's type checker asks of their operands, and how a
//! value of no type takes the type of what is around it. The functions
//! follow `go/types` (expr.go, const.go), and the messages are Go's.

use super::*;
use crate::exprstring::{Writer, binary_text, unary_text};

/// Why a value of no type cannot take a type.
#[derive(Clone, Copy, PartialEq)]
pub(super) enum Unfit {
    /// A number with a fraction, for an integer type.
    Truncated,
    /// A number outside the range of the type.
    Overflows,
    Other,
}

fn is_comparison(op: BinaryOp) -> bool {
    use BinaryOp::*;
    matches!(op, Eq | Ne | Lt | Le | Gt | Ge)
}

fn is_shift(op: BinaryOp) -> bool {
    matches!(op, BinaryOp::Shl | BinaryOp::Shr)
}

impl<'a> Checker<'a> {
    pub(super) fn expr_string(&self, cx: &Cx<'a>, expr: ExprId) -> String {
        let writer = Writer {
            tree: cx.tree,
            text: cx.text,
            names: self.names,
        };
        writer.expr_string(expr)
    }

    pub(super) fn type_string(&self, ty: TypeId) -> String {
        self.types.display_in(ty, self.names, self.name)
    }

    /// Reports an error once: an expression may be looked at twice.
    pub(super) fn error(&mut self, code: Code, span: Span, message: String) {
        // In a value that a constant repeats, an error is at the constant.
        let span = self.error_at.unwrap_or(span);
        if self.reported.insert((span, code)) {
            self.sink.push(Diagnostic::new(code, span, message));
        }
    }

    /// An error with what to write in its place.
    pub(super) fn error_with_fix(
        &mut self,
        code: Code,
        span: Span,
        message: String,
        (fix, instead): (Code, String),
    ) {
        if self.reported.insert((span, code)) {
            let diagnostic = Diagnostic::new(code, span, message);
            self.sink.push(diagnostic.with_note(fix, None, instead));
        }
    }

    fn span(&self, cx: &Cx<'a>, expr: ExprId) -> Span {
        cx.tree.exprs[expr].span
    }

    /// The operand as a message describes it: `x (variable of type int)`,
    /// `1 << 70 (untyped int constant 1180591620717411303424)`.
    pub(super) fn describe(&self, cx: &Cx<'a>, expr: ExprId, x: &Operand) -> String {
        let nil = self.types.basic(Basic::UntypedNil);
        if x.mode == Mode::Value && x.ty == nil {
            return "nil".to_owned();
        }
        let text = self.expr_string(cx, expr);
        let mut out = format!("{text} (");
        let has_value = !matches!(
            x.mode,
            Mode::Unknown | Mode::NoValue | Mode::Builtin(_) | Mode::Unsafe(_) | Mode::Type
        );
        let untyped = has_value && self.is_untyped(x.ty);
        if untyped {
            out += &self.type_string(x.ty);
            out.push(' ');
        }
        out += match &x.mode {
            Mode::Unknown => "invalid operand",
            Mode::NoValue => "no value",
            Mode::Builtin(_) | Mode::Unsafe(_) => "built-in",
            Mode::Type => "type",
            Mode::Const(_) => "constant",
            Mode::Variable => "variable",
            Mode::MapIndex => "map index expression",
            Mode::Value => "value",
            Mode::CommaOk => "comma, ok expression",
        };
        if let Mode::Const(value) = &x.mode {
            let value = value.to_string();
            if value != text {
                out.push(' ');
                out += &value;
            }
        }
        if has_value && !untyped {
            match x.ty {
                invalid if invalid == Types::INVALID => out += " with invalid type",
                ty => {
                    // A declared type and an alias are said with what
                    // they are.
                    let mut what = String::new();
                    let named = matches!(&*self.types.get(ty), Type::Named { .. });
                    if named || self.types.named_by_alias(ty) && !self.is_param(ty) {
                        what = match self.composite_kind(ty) {
                            "" => {
                                let under = self.types.underlying(ty);
                                self.type_string(self.types.unalias(under))
                            }
                            kind => kind.to_owned(),
                        };
                        what.push(' ');
                    }
                    out += &format!(" of {what}type {}", self.type_string(ty));
                    if let Some(constraint) = self.types.constraint(ty) {
                        out += &format!(" constrained by {}", self.type_string(constraint));
                        if self.types.param_set(ty).is_some_and(|set| set.is_empty()) {
                            out += " with empty type set";
                        }
                    }
                }
            }
        }
        out.push(')');
        out
    }

    /// `array`, `slice`, ... for the type; nothing for a basic type.
    fn composite_kind(&self, ty: TypeId) -> &'static str {
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Array { .. } => "array",
            Type::Slice(_) => "slice",
            Type::Struct(_) => "struct",
            Type::Pointer(_) => "pointer",
            Type::Func { .. } => "func",
            Type::Interface { .. } => "interface",
            Type::Map { .. } => "map",
            Type::Chan { .. } => "chan",
            Type::Tuple(_) => "tuple",
            _ => "",
        }
    }

    // ---- what a type is

    pub(super) fn is_param(&self, ty: TypeId) -> bool {
        matches!(&*self.types.get(ty), Type::Param(_))
    }

    /// Whether what holds for the type is not known here: it is in
    /// error, or a type parameter whose constraint is.
    pub(super) fn open(&self, ty: TypeId) -> bool {
        self.types.underlying(ty) == Types::INVALID
            || self.is_param(ty) && self.types.param_set(ty).is_none()
    }

    /// Whether `holds` for the type; for a type parameter, whether it
    /// holds for every type that the parameter may be, of which its
    /// constraint names one at least.
    pub(super) fn all_types(&self, ty: TypeId, holds: impl Fn(&Self, TypeId) -> bool) -> bool {
        match self.is_param(ty) {
            true => self
                .types
                .specific_types(ty)
                .is_some_and(|types| types.iter().all(|&(ty, _)| holds(self, ty))),
            false => holds(self, ty),
        }
    }

    pub(super) fn is_numeric(&self, ty: TypeId) -> bool {
        self.basic_of(ty).is_some_and(|basic| {
            !matches!(
                basic,
                Basic::Bool
                    | Basic::String
                    | Basic::UntypedBool
                    | Basic::UntypedString
                    | Basic::UntypedNil
            )
        })
    }

    pub(super) fn is_boolean(&self, ty: TypeId) -> bool {
        matches!(self.basic_of(ty), Some(Basic::Bool | Basic::UntypedBool))
    }

    pub(super) fn is_string(&self, ty: TypeId) -> bool {
        matches!(
            self.basic_of(ty),
            Some(Basic::String | Basic::UntypedString)
        )
    }

    pub(super) fn is_complex(&self, ty: TypeId) -> bool {
        use Basic::*;
        matches!(
            self.basic_of(ty),
            Some(Complex64 | Complex128 | UntypedComplex)
        )
    }

    fn is_nil(&self, x: &Operand) -> bool {
        x.mode == Mode::Value && x.ty == self.types.basic(Basic::UntypedNil)
    }

    fn is_interface(&self, ty: TypeId) -> bool {
        self.types.is_interface(ty)
    }

    /// Whether `nil` is a value of the type by Go's rule. What a `.gopp`
    /// file asks of pointers, funcs and interfaces is checked on its own.
    fn has_nil(&self, ty: TypeId) -> bool {
        self.all_types(ty, |this, ty| this.types.has_nil(ty, Dialect::Go))
    }

    // ---- constants

    /// The constant `value` of the type `from` as the basic type `to`
    /// holds it.
    fn representation(&self, value: &Value, from: TypeId, to: Basic) -> Result<Value, Unfit> {
        if let Some(value) = representable(value, to) {
            return Ok(value);
        }
        let to = self.types.basic(to);
        Err(match self.is_numeric(from) && self.is_numeric(to) {
            true if !self.is_integer(from) && self.is_integer(to) => Unfit::Truncated,
            true => Unfit::Overflows,
            false => Unfit::Other,
        })
    }

    fn invalid_conversion(
        &mut self,
        cx: &Cx<'a>,
        unfit: Unfit,
        expr: ExprId,
        x: &Operand,
        target: TypeId,
    ) {
        let (x, target) = (self.describe(cx, expr, x), self.type_string(target));
        let (code, message) = match unfit {
            Unfit::Truncated => (Code::ValueTruncated, format!("{x} truncated to {target}")),
            Unfit::Overflows => (Code::ValueOverflows, format!("{x} overflows {target}")),
            Unfit::Other => (
                Code::CannotConvert,
                format!("cannot convert {x} to type {target}"),
            ),
        };
        self.error(code, self.span(cx, expr), message);
    }

    /// The result `x` of the constant expression `expr` fits its type, or
    /// is not too large to compute with if it has none.
    fn overflow(&mut self, cx: &Cx<'a>, expr: ExprId, x: &mut Operand) {
        let Mode::Const(value) = &x.mode else { return };
        if *value == Value::Unknown {
            return;
        }
        if !self.is_untyped(x.ty) {
            let Some(basic) = self.basic_of(x.ty) else {
                return;
            };
            match self.representation(value, x.ty, basic) {
                Ok(value) => x.mode = Mode::Const(value),
                Err(unfit) => {
                    let target = self.types.basic(basic);
                    self.invalid_conversion(cx, unfit, expr, x, target);
                    *x = Operand::UNKNOWN;
                }
            }
            return;
        }
        // The precision of constants.
        const BITS: u64 = 512;
        if value.kind() == Kind::Int && value.bit_len() > BITS {
            let (operation, at) = match cx.tree.exprs[expr].kind {
                ExprKind::Binary { op, x, y } => {
                    let name = match op {
                        BinaryOp::Add => "addition ",
                        BinaryOp::Sub => "subtraction ",
                        BinaryOp::Xor => "bitwise XOR ",
                        BinaryOp::Mul => "multiplication ",
                        BinaryOp::Shl => "shift ",
                        _ => "",
                    };
                    (name, self.operator_span(cx, x, y, op))
                }
                ExprKind::Unary {
                    op: UnaryOp::Xor, ..
                } => ("bitwise complement ", self.span(cx, expr)),
                _ => ("", self.span(cx, expr)),
            };
            let message = format!("constant {operation}overflow");
            self.error(Code::ConstantExpressionOverflow, at, message);
            *x = Operand::UNKNOWN;
        }
    }

    /// Where the operator of `x op y` is written.
    fn operator_span(&self, cx: &Cx<'a>, x: ExprId, y: ExprId, op: BinaryOp) -> Span {
        let (from, to) = (self.span(cx, x).hi as usize, self.span(cx, y).lo as usize);
        let between = &cx.text[from..to.max(from)];
        // Comments may stand between the operand and the operator.
        let mut at = 0;
        loop {
            let rest = &between[at..];
            let trimmed = rest.trim_start();
            at += rest.len() - trimmed.len();
            let comment = match trimmed {
                _ if trimmed.starts_with("/*") => trimmed.find("*/").map(|end| end + 2),
                _ if trimmed.starts_with("//") => trimmed.find('\n').map(|end| end + 1),
                _ => None,
            };
            match comment {
                Some(len) => at += len,
                None => break,
            }
        }
        let lo = (from + at) as u32;
        Span {
            lo,
            hi: lo + binary_text(op).len() as u32,
            ..self.span(cx, x)
        }
    }

    // ---- values of no type

    /// The type, and for a constant the value, that the untyped `x` has
    /// as a value of the type `target`.
    pub(super) fn implicit(
        &self,
        x: &Operand,
        target: TypeId,
    ) -> Result<(TypeId, Option<Value>), Unfit> {
        if !x.is_value() || !self.is_untyped(x.ty) || target == Types::INVALID {
            return Ok((x.ty, None));
        }
        let nil = self.types.basic(Basic::UntypedNil);
        if self.is_untyped(target) {
            // Both have no type: the later kind of number.
            return match self.larger_kind(x.ty, target) {
                Some(kind) => Ok((kind, None)),
                None => Err(Unfit::Other),
            };
        }
        let for_nil = |fits: bool| match fits {
            true => Ok((nil, None)),
            false => Err(Unfit::Other),
        };
        match &*self.types.get(self.types.underlying(target)) {
            Type::Basic(basic) => {
                if let Mode::Const(value) = &x.mode {
                    let value = self.representation(value, x.ty, *basic)?;
                    return Ok((target, Some(value)));
                }
                // A value that is no constant: only its kind is known.
                let fits = match self.basic_of(x.ty) {
                    Some(Basic::UntypedBool) => self.is_boolean(target),
                    Some(Basic::UntypedString) => self.is_string(target),
                    Some(Basic::UntypedNil) => return for_nil(false),
                    _ => self.is_numeric(target),
                };
                match fits {
                    true => Ok((target, None)),
                    false => Err(Unfit::Other),
                }
            }
            // An interface holds a value with a type of its own; only the
            // empty interface takes a value that has no methods.
            Type::Interface { methods, embedded } => {
                if x.ty == nil {
                    return for_nil(self.has_nil(target));
                }
                match methods.is_empty() && embedded.is_empty() {
                    true => Ok((self.types.default_type(x.ty), None)),
                    false => Err(Unfit::Other),
                }
            }
            // A type parameter takes what every type that it may be
            // takes; `nil` stays without a type.
            Type::Param(_) => {
                let fits =
                    |this: &Self, ty: TypeId| this.implicit(x, this.types.underlying(ty)).is_ok();
                match (
                    self.open(target) || self.all_types(target, fits),
                    x.ty == nil,
                ) {
                    (false, _) => Err(Unfit::Other),
                    (true, true) => Ok((nil, None)),
                    (true, false) => Ok((target, None)),
                }
            }
            // A value is its own option (TYP-2).
            Type::Option(elem) if x.ty != nil => self.implicit(x, *elem),
            Type::Pointer(_)
            | Type::Func { .. }
            | Type::Slice(_)
            | Type::Map { .. }
            | Type::Chan { .. }
            | Type::Option(_) => for_nil(x.ty == nil && self.has_nil(target)),
            _ => Err(Unfit::Other),
        }
    }

    /// Gives the untyped operand `x` of the expression `expr` the type
    /// `target`. With `report`, an error says why it cannot have it.
    pub(super) fn convert_untyped(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &mut Operand,
        target: TypeId,
        report: bool,
    ) -> bool {
        match self.implicit(x, target) {
            Err(unfit) => {
                if report {
                    let shown = match &*self.types.get(target) {
                        Type::Param(_) => target,
                        _ => self.types.underlying(target),
                    };
                    self.invalid_conversion(cx, unfit, expr, x, shown);
                }
                *x = Operand::UNKNOWN;
                false
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
                true
            }
        }
    }

    /// The constant `expr`, which has no type yet, has the value `value`.
    pub(super) fn update_expr_value(&mut self, expr: ExprId, value: &Value) {
        let untyped = |operand: &&mut Operand| self.types.is_untyped(operand.ty);
        let known = self.memo.as_mut().and_then(|memo| memo.get_mut(&expr));
        if let Some(operand) = known.filter(untyped) {
            operand.mode = Mode::Const(value.clone());
        }
    }

    /// The expression `expr`, which has no type yet, gets the type `ty`,
    /// and so do the operands that it got its kind from. With a type that
    /// is no kind of constant, or with `last`, that is its type for good:
    /// a constant must then fit it.
    pub(super) fn update_expr_type(&mut self, cx: &Cx<'a>, expr: ExprId, ty: TypeId, last: bool) {
        let Some(old) = self.memo.as_ref().and_then(|memo| memo.get(&expr)).cloned() else {
            return;
        };
        if !old.is_value() || !self.is_untyped(old.ty) {
            return;
        }
        let constant = matches!(old.mode, Mode::Const(_));
        match cx.tree.exprs[expr].kind {
            ExprKind::Paren(x) => self.update_expr_type(cx, x, ty, last),
            // The operands of a constant keep their kind: `uint8(-1 + 2)`
            // is right though `-1` is no `uint8`.
            ExprKind::Unary { x, .. } if !constant => self.update_expr_type(cx, x, ty, last),
            ExprKind::Binary { op, x, y } if !constant => {
                // The result of a comparison says nothing of its operands.
                if is_shift(op) {
                    self.update_expr_type(cx, x, ty, last);
                } else if !is_comparison(op) {
                    self.update_expr_type(cx, x, ty, last);
                    self.update_expr_type(cx, y, ty, last);
                }
            }
            _ => {}
        }
        if self.is_untyped(ty) {
            if !last && let Some(entry) = self.memo.as_mut().and_then(|memo| memo.get_mut(&expr)) {
                entry.ty = self.types.underlying(ty);
            }
            return;
        }
        // What is shifted by a count that is no constant is an integer.
        if self.shifted.contains(&expr) && !self.all_integer(ty) {
            let message = format!(
                "invalid operation: shifted operand {} (type {}) must be integer",
                self.expr_string(cx, expr),
                self.type_string(ty)
            );
            return self.error(Code::ShiftOperandInvalid, self.span(cx, expr), message);
        }
        if constant {
            let mut held = old;
            if let Err(unfit) = self.implicit(&held, ty) {
                let shown = self.types.underlying(ty);
                self.invalid_conversion(cx, unfit, expr, &held, shown);
                held = Operand::UNKNOWN;
            }
            if held.mode == Mode::Unknown {
                return;
            }
        }
        if let Some(entry) = self.memo.as_mut().and_then(|memo| memo.get_mut(&expr)) {
            entry.ty = ty;
        }
    }

    /// Gives the expression `expr` the type `to`, if it has no type of its
    /// own yet, as an assignment to a place of the type `to` does. Nothing
    /// is said if it cannot have that type.
    pub(super) fn convert(&mut self, cx: &Cx<'a>, expr: ExprId, to: TypeId) {
        let Some(mut x) = self.memo.as_ref().and_then(|memo| memo.get(&expr)).cloned() else {
            return;
        };
        // In an interface, a value has the default type of its kind.
        let target = match self.is_interface(to) && self.is_untyped(x.ty) {
            true => self.types.default_type(x.ty),
            false => to,
        };
        self.convert_untyped(cx, expr, &mut x, target, false);
    }

    /// Gives the expression `expr` the default type of its kind, if it has
    /// no type of its own yet.
    pub(super) fn convert_default(&mut self, cx: &Cx<'a>, expr: ExprId) {
        let known = self.memo.as_ref().and_then(|memo| memo.get(&expr));
        if let Some(ty) = known
            .map(|operand| operand.ty)
            .filter(|&ty| self.is_untyped(ty))
        {
            self.convert(cx, expr, self.types.default_type(ty));
        }
    }

    // ---- operators

    /// The expression `expr` where one value is needed: a call without a
    /// result, a built-in that is not called, a type, and a call with
    /// several results are errors there.
    pub(super) fn value(&mut self, cx: &Cx<'a>, expr: ExprId) -> Operand {
        let x = self.operand(cx, expr);
        let (code, what) = match &x.mode {
            Mode::NoValue => (Code::NoValue, "used as value"),
            Mode::Builtin(_) | Mode::Unsafe(_) => (Code::BuiltinNotCalled, "must be called"),
            Mode::Type => (Code::NotAnExpression, "is not an expression"),
            Mode::Value if matches!(&*self.types.get(x.ty), Type::Tuple(_)) => {
                let message = format!(
                    "multiple-value {} in single-value context",
                    self.describe(cx, expr, &x)
                );
                self.error(Code::MultiValueInSingle, self.span(cx, expr), message);
                return Operand::UNKNOWN;
            }
            _ => {
                self.read_alone(cx, expr, &x);
                return x;
            }
        };
        let message = format!("{} {what}", self.describe(cx, expr, &x));
        self.error(code, self.span(cx, expr), message);
        Operand::UNKNOWN
    }

    /// Whether all types that `ty` may be are booleans.
    pub(super) fn all_boolean(&self, ty: TypeId) -> bool {
        self.open(ty) || self.all_types(ty, Self::is_boolean)
    }

    /// Whether all types that `ty` may be are numbers.
    pub(super) fn all_numeric(&self, ty: TypeId) -> bool {
        self.open(ty) || self.all_types(ty, Self::is_numeric)
    }

    /// Whether all types that `ty` may be are integers.
    pub(super) fn all_integer(&self, ty: TypeId) -> bool {
        self.open(ty) || self.all_types(ty, Self::is_integer)
    }

    /// Whether the operator `op` is defined on the operand `x`.
    fn defined(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &Operand,
        op: &str,
        holds: fn(&Self, TypeId) -> bool,
    ) -> bool {
        if self.open(x.ty) || self.all_types(x.ty, holds) {
            return true;
        }
        if self.may_overload(cx, x.ty) {
            return false;
        }
        let message = format!(
            "invalid operation: operator {op} not defined on {}",
            self.describe(cx, expr, x)
        );
        self.error(Code::OperatorNotDefined, self.span(cx, expr), message);
        false
    }

    pub(super) fn unary(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        op: UnaryOp,
        inner: ExprId,
    ) -> Operand {
        // `*T` is a type, `*p` what the pointer points to.
        let x = match op {
            UnaryOp::Star => {
                let x = self.operand(cx, inner);
                self.instantiated(cx, inner, x)
            }
            // The address is that of the variable, whatever is known of
            // it, and what gets the address may assign it.
            UnaryOp::Addr => {
                let outer = self.unnarrowed.replace(self.unparen(cx, inner));
                let x = self.value(cx, inner);
                self.unnarrowed = outer;
                self.flow_written(cx, inner);
                x
            }
            _ => self.value(cx, inner),
        };
        if x.mode == Mode::Unknown {
            return Operand::UNKNOWN;
        }
        match op {
            UnaryOp::Star => {
                return match x.mode {
                    Mode::Type => {
                        self.var_types.push((x.ty, cx.tree.exprs[inner].span));
                        Operand {
                            mode: Mode::Type,
                            ty: self.types.pointer(x.ty),
                        }
                    }
                    _ if x.is_value() => match &*self.types.get(self.types.underlying(x.ty)) {
                        Type::Pointer(elem) => Operand::variable(*elem),
                        Type::Param(_) if !self.open(x.ty) => self.indirect_param(cx, inner, &x),
                        Type::Unique(_) | Type::Shared(_) | Type::Option(_)
                            if self.own_rules(cx) =>
                        {
                            self.read(cx, x)
                        }
                        _ => {
                            if !self.open(x.ty) {
                                let message = format!(
                                    "invalid operation: cannot indirect {}",
                                    self.describe(cx, inner, &x)
                                );
                                self.error(Code::DerefNonPointer, self.span(cx, inner), message);
                            }
                            Operand::UNKNOWN
                        }
                    },
                    _ => Operand::UNKNOWN,
                };
            }
            UnaryOp::Addr => {
                if !x.is_value() {
                    return Operand::UNKNOWN;
                }
                let mut literal = inner;
                while let ExprKind::Paren(x) = cx.tree.exprs[literal].kind {
                    literal = x;
                }
                let is_literal =
                    matches!(cx.tree.exprs[literal].kind, ExprKind::CompositeLit { .. });
                // A value on the heap has an owner: one, or several.
                if is_literal && self.own_rules(cx) {
                    let written = self.expr_string(cx, literal);
                    let message = format!("cannot take the address of {written}");
                    let fix = format!("use NewUnique({written}) or NewShared({written})");
                    self.error_with_fix(
                        Code::AddrOfCompositeLit,
                        self.span(cx, expr),
                        message,
                        (Code::NoteOwnFixHeapPointer, fix),
                    );
                }
                // `&s[i:j]` in a `.gopp` file is those elements of `s`.
                let is_view = self.own_rules(cx)
                    && matches!(cx.tree.exprs[literal].kind, ExprKind::Slice { .. });
                let held = self.own_rules(cx) && self.held_shared(cx, inner);
                if !is_literal && !is_view && !held && x.mode != Mode::Variable {
                    let message = format!(
                        "invalid operation: cannot take address of {}",
                        self.describe(cx, inner, &x)
                    );
                    self.error(Code::AddressOfUnaddressable, self.span(cx, inner), message);
                    return Operand::UNKNOWN;
                }
                self.changed(cx, inner, place::Change::Addressed);
                self.addressed(cx, inner);
                self.address_of_required(cx, expr, inner);
                return Operand::value(self.types.pointer(x.ty));
            }
            UnaryOp::Recv => {
                if !x.is_value() {
                    return Operand::UNKNOWN;
                }
                self.called_or_received = true;
                let at = self.span(cx, inner);
                return match self.chan_elem(cx, (inner, at), &x, true) {
                    None => Operand::UNKNOWN,
                    Some(elem) if elem == Types::INVALID => Operand::UNKNOWN,
                    Some(elem) => Operand {
                        mode: Mode::CommaOk,
                        ty: elem,
                    },
                };
            }
            UnaryOp::Tilde | UnaryOp::Plus | UnaryOp::Neg | UnaryOp::Not | UnaryOp::Xor => {}
        }
        if !x.is_value() {
            return Operand::UNKNOWN;
        }
        let x = self.read(cx, x);
        // `~` belongs to constraints; on an integer, `^` was meant.
        let op = match op {
            UnaryOp::Tilde => {
                let integer = self.all_integer(x.ty);
                let message = format!(
                    "cannot use ~ outside of interface or type constraint{}",
                    if integer {
                        " (use ^ for bitwise complement)"
                    } else {
                        ""
                    }
                );
                self.error(Code::TildeOutsideConstraint, self.span(cx, expr), message);
                if !integer {
                    return Operand::UNKNOWN;
                }
                UnaryOp::Xor
            }
            op => op,
        };
        let holds: fn(&Self, TypeId) -> bool = match op {
            UnaryOp::Xor => Self::is_integer,
            UnaryOp::Not => Self::is_boolean,
            _ => Self::is_numeric,
        };
        if !self.defined(cx, inner, &x, unary_text(op), holds) {
            return Operand::UNKNOWN;
        }
        let Mode::Const(value) = &x.mode else {
            return Operand::value(x.ty);
        };
        if *value == Value::Unknown {
            return Operand::UNKNOWN;
        }
        let (op, bits) = match op {
            UnaryOp::Plus => (constant::UnaryOp::Plus, 0),
            UnaryOp::Neg => (constant::UnaryOp::Neg, 0),
            UnaryOp::Not => (constant::UnaryOp::Not, 0),
            // The complement of an unsigned number stays in its size.
            _ => {
                let bits = match self.basic_of(x.ty) {
                    Some(Basic::Uint8) => 8,
                    Some(Basic::Uint16) => 16,
                    Some(Basic::Uint32) => 32,
                    Some(Basic::Uint | Basic::Uint64 | Basic::Uintptr) => 64,
                    _ => 0,
                };
                (constant::UnaryOp::Complement, bits)
            }
        };
        let mut result = Operand::constant(x.ty, value.unary(op, bits));
        self.overflow(cx, expr, &mut result);
        result
    }

    /// `*p` for a `p` whose type is a type parameter: every type that it
    /// may be is a pointer, all to one type.
    fn indirect_param(&mut self, cx: &Cx<'a>, inner: ExprId, x: &Operand) -> Operand {
        let types = self.types.specific_types(x.ty).unwrap_or_default();
        let bases: Option<Vec<TypeId>> = types
            .iter()
            .map(|&(_, under)| match &*self.types.get(under) {
                Type::Pointer(elem) => Some(*elem),
                _ => None,
            })
            .collect();
        let shown = self.describe(cx, inner, x);
        let message = match bases.as_deref() {
            Some([first, rest @ ..]) => {
                if rest.iter().all(|&base| self.types.identical(base, *first)) {
                    return Operand::variable(*first);
                }
                format!("invalid operation: pointers of {shown} must have identical base types")
            }
            _ => format!("invalid operation: cannot indirect {shown}"),
        };
        self.error(Code::DerefNonPointer, self.span(cx, inner), message);
        Operand::UNKNOWN
    }

    /// The element type of the channel `x`, which is received from or sent
    /// to: a channel that goes that way, or a type parameter whose types
    /// are all such channels with one element type.
    pub(super) fn chan_elem(
        &mut self,
        cx: &Cx<'a>,
        (expr, at): (ExprId, Span),
        x: &Operand,
        receive: bool,
    ) -> Option<TypeId> {
        if self.open(x.ty) || !x.is_value() {
            return None;
        }
        let shared = self.common_under(x.ty, |this, of| {
            let Some((ty, under)) = of else {
                return Some("no specific channel type".to_owned());
            };
            let wrong = match &*this.types.get(under) {
                Type::Chan {
                    dir: ChanDir::Send, ..
                } if receive => "send-only channel",
                Type::Chan {
                    dir: ChanDir::Recv, ..
                } if !receive => "receive-only channel",
                Type::Chan { .. } => return None,
                _ => "non-channel",
            };
            Some(format!("{wrong} {}", this.type_string(ty)))
        });
        let cause = match shared {
            Ok(shared) => match &*self.types.get(shared) {
                Type::Chan { elem, .. } => return Some(*elem),
                _ => return None,
            },
            Err(cause) => cause,
        };
        let what = if receive { "receive from" } else { "send to" };
        let shown = self.describe(cx, expr, x);
        let message = match self.is_param(x.ty) {
            true => format!("invalid operation: cannot {what} {shown}: {cause}"),
            false => format!("invalid operation: cannot {what} {cause} {shown}"),
        };
        self.error(Code::ChannelDirection, at, message);
        None
    }

    /// The underlying type that all the types that a value of the type
    /// `ty` may have share, or why they share none. `refused` says what is
    /// wrong with one of the types, which it is given with its underlying
    /// type, and with the case that there is no type.
    pub(super) fn common_under(
        &self,
        ty: TypeId,
        refused: impl Fn(&Self, Option<(TypeId, TypeId)>) -> Option<String>,
    ) -> Result<TypeId, String> {
        use crate::types::NoCommon;
        let Some(types) = self.types.specific_types(ty) else {
            return Err(refused(self, None).unwrap_or_else(|| "no specific type".to_owned()));
        };
        if let Some(cause) = types.iter().find_map(|&of| refused(self, Some(of))) {
            return Err(cause);
        }
        let shown = |ty: TypeId| self.type_string(ty);
        self.types.common_under(ty).map_err(|why| match why {
            NoCommon::NoSpecificType => "no specific type".to_owned(),
            NoCommon::DifferentUnderlying(a, b) => {
                format!(
                    "{} and {} have different underlying types",
                    shown(a),
                    shown(b)
                )
            }
            NoCommon::DifferentElements(a, b) => format!(
                "channels {} and {} have different element types",
                shown(a),
                shown(b)
            ),
            NoCommon::ConflictingDirections(a, b) => format!(
                "channels {} and {} have conflicting directions",
                shown(a),
                shown(b)
            ),
        })
    }

    /// An untyped operand takes the type of the other, where that can be.
    pub(super) fn match_types(
        &mut self,
        cx: &Cx<'a>,
        (left, x): (ExprId, &mut Operand),
        (right, y): (ExprId, &mut Operand),
    ) {
        let may_convert = {
            let (x_untyped, y_untyped) = (self.is_untyped(x.ty), self.is_untyped(y.ty));
            if !x_untyped && !y_untyped {
                false
            } else if self.is_interface(x.ty) || self.is_interface(y.ty) {
                true
            } else if self.all_types(x.ty, Self::is_boolean)
                != self.all_types(y.ty, Self::is_boolean)
                || self.all_types(x.ty, Self::is_string) != self.all_types(y.ty, Self::is_string)
            {
                false
            } else if self.is_nil(x) {
                self.has_nil(y.ty)
            } else if self.is_nil(y) {
                self.has_nil(x.ty)
            } else {
                let pointer = |ty| {
                    matches!(
                        &*self.types.get(self.types.underlying(ty)),
                        Type::Pointer(_)
                    )
                };
                !pointer(x.ty) && !pointer(y.ty)
            }
        };
        if may_convert
            && self.convert_untyped(cx, left, x, y.ty, true)
            && !self.convert_untyped(cx, right, y, x.ty, true)
        {
            *x = Operand::UNKNOWN;
        }
    }

    /// `left op right`, which is the expression `whole`, or without one
    /// the statement `left op= right`.
    pub(super) fn binary(
        &mut self,
        cx: &Cx<'a>,
        whole: Option<ExprId>,
        op: BinaryOp,
        left: ExprId,
        right: ExprId,
    ) -> Operand {
        let (mut x, mut y) = self.operands(cx, op, left, right);
        if !x.is_value() || !y.is_value() {
            return Operand::UNKNOWN;
        }
        // Two pointers are compared as pointers; anything else is done
        // to what a pointer points to.
        let refers = |this: &Self, x: &Operand| {
            // An option of a pointer is compared like the pointer.
            let held = match &*this.types.get(this.types.underlying(x.ty)) {
                Type::Option(elem) => Operand {
                    mode: x.mode.clone(),
                    ty: *elem,
                },
                _ => x.clone(),
            };
            this.is_nil(x) || this.read(cx, held.clone()).ty != held.ty
        };
        if !(is_comparison(op) && refers(self, &x) && refers(self, &y)) {
            (x, y) = (self.read(cx, x), self.read(cx, y));
        }
        if is_shift(op) {
            return self.shift(cx, whole, op, (left, x), (right, y));
        }
        self.match_types(cx, (left, &mut x), (right, &mut y));
        if x.mode == Mode::Unknown {
            return Operand::UNKNOWN;
        }
        if is_comparison(op) {
            return self.comparison(cx, op, (left, x), (right, y));
        }
        if !self.types.identical(x.ty, y.ty) {
            let decided = !self.may_overload(cx, x.ty) && !self.may_overload(cx, y.ty);
            if !self.open(x.ty) && !self.open(y.ty) && decided {
                let written = match whole {
                    Some(expr) => self.expr_string(cx, expr),
                    None => format!(
                        "{} {}= {}",
                        self.expr_string(cx, left),
                        binary_text(op),
                        self.expr_string(cx, right)
                    ),
                };
                let message = format!(
                    "invalid operation: {written} (mismatched types {} and {})",
                    self.type_string(x.ty),
                    self.type_string(y.ty)
                );
                self.error(
                    Code::MismatchedTypes,
                    self.span(cx, whole.unwrap_or(left)),
                    message,
                );
            }
            return Operand::UNKNOWN;
        }
        let holds: fn(&Self, TypeId) -> bool = match op {
            BinaryOp::Add => |this, ty| this.is_numeric(ty) || this.is_string(ty),
            BinaryOp::Sub | BinaryOp::Mul | BinaryOp::Div => Self::is_numeric,
            BinaryOp::OrOr | BinaryOp::AndAnd => Self::is_boolean,
            _ => Self::is_integer,
        };
        if !self.defined(cx, left, &x, binary_text(op), holds) {
            return Operand::UNKNOWN;
        }
        if matches!(op, BinaryOp::Div | BinaryOp::Rem)
            && let Mode::Const(divisor) = &y.mode
            && *divisor != Value::Unknown
        {
            let by_zero = match (&x.mode, self.is_complex(x.ty)) {
                (Mode::Const(_), true) => {
                    let complex = divisor.to_complex();
                    complex.real().sign() == 0 && complex.imag().sign() == 0
                }
                (Mode::Const(_), false) => divisor.sign() == 0,
                _ => self.is_integer(x.ty) && divisor.sign() == 0,
            };
            if by_zero {
                let message = "invalid operation: division by zero".to_owned();
                self.error(Code::DivisionByZero, self.span(cx, right), message);
                return Operand::UNKNOWN;
            }
        }
        let (Mode::Const(a), Mode::Const(b)) = (&x.mode, &y.mode) else {
            return Operand::value(x.ty);
        };
        if *a == Value::Unknown || *b == Value::Unknown {
            return Operand::UNKNOWN;
        }
        let op = match op {
            BinaryOp::OrOr => constant::BinaryOp::LogicalOr,
            BinaryOp::AndAnd => constant::BinaryOp::LogicalAnd,
            BinaryOp::Add => constant::BinaryOp::Add,
            BinaryOp::Sub => constant::BinaryOp::Sub,
            BinaryOp::Mul => constant::BinaryOp::Mul,
            // Two integers divide to an integer.
            BinaryOp::Div if self.is_integer(x.ty) => constant::BinaryOp::IntQuo,
            BinaryOp::Div => constant::BinaryOp::Quo,
            BinaryOp::Rem => constant::BinaryOp::Rem,
            BinaryOp::And => constant::BinaryOp::And,
            BinaryOp::Or => constant::BinaryOp::Or,
            BinaryOp::Xor => constant::BinaryOp::Xor,
            BinaryOp::AndNot => constant::BinaryOp::AndNot,
            _ => return Operand::UNKNOWN,
        };
        let value = a.binary(op, b);
        if value == Value::Unknown {
            return Operand::UNKNOWN;
        }
        let mut result = Operand::constant(x.ty, value);
        if let Some(expr) = whole {
            self.overflow(cx, expr, &mut result);
        }
        result
    }

    /// Whether the operand `x` is surely no value of the type `to`.
    pub(super) fn not_assignable(&self, x: &Operand, to: TypeId) -> bool {
        if self.open(x.ty) || self.open(to) || self.types.is_constraint(to) {
            return false;
        }
        if self.is_param(x.ty) || self.is_param(to) {
            return self.param_unfit(x, to).is_some();
        }
        if self.is_untyped(x.ty) {
            return self.implicit(x, to).is_err();
        }
        if self.is_interface(to) {
            return self.lacking(x.ty, to).is_some();
        }
        // What is in an interface is taken out of it by an assertion.
        if self.is_interface(x.ty) {
            return true;
        }
        let value = match &x.mode {
            Mode::Const(value) => Some(value),
            _ => None,
        };
        !self
            .types
            .assignable(x.ty, value, to, Dialect::Go, self.names)
    }

    /// Where the type of `x` or the type `to` is a type parameter: that
    /// `x` is no value of `to`, with the cause where there is more to say
    /// than the two types. `None` if it is one.
    pub(super) fn param_unfit(&self, x: &Operand, to: TypeId) -> Option<Option<String>> {
        if self.types.identical(x.ty, to) {
            return None;
        }
        let (from_param, to_param) = (self.is_param(x.ty), self.is_param(to));
        // A value of no type fits a type parameter if it fits every type
        // that the parameter may be.
        if self.is_untyped(x.ty) {
            let fits = |this: &Self, ty: TypeId| this.implicit(x, ty).is_ok();
            return (!self.all_types(to, fits)).then_some(None);
        }
        // A type parameter is a value of an interface that its constraint
        // implements.
        if !to_param && self.is_interface(to) && self.lacking(x.ty, to).is_none() {
            return None;
        }
        // A type without a name fits a type parameter if it fits every
        // type that the parameter may be, and a type parameter fits a type
        // without a name if every type that it may be does.
        let named = |ty: TypeId| self.types.is_named(ty);
        let (inner, outer) = if !named(x.ty) && to_param {
            (None, to)
        } else if from_param && !named(to) {
            (Some(x.ty), to)
        } else {
            return Some(None);
        };
        let param = inner.unwrap_or(outer);
        let Some(types) = self.types.specific_types(param) else {
            return Some(None);
        };
        for (ty, _) in types {
            let (value, place) = match inner {
                Some(_) => (
                    Operand {
                        mode: x.mode.clone(),
                        ty,
                    },
                    to,
                ),
                None => (x.clone(), ty),
            };
            if self.not_assignable(&value, place) {
                let cause = match inner {
                    Some(from) => format!(
                        "cannot assign {} (in {}) to {}",
                        self.type_string(ty),
                        self.type_string(from),
                        self.type_string(to)
                    ),
                    None => format!(
                        "cannot assign {} to {} (in {})",
                        self.type_string(x.ty),
                        self.type_string(ty),
                        self.type_string(to)
                    ),
                };
                return Some(Some(cause));
            }
        }
        None
    }

    /// What forbids to compare `x` with `y` by `op`: whether it is the
    /// second operand that the error is about, the cause, and the code.
    /// `Err` if an operator method may decide instead.
    pub(super) fn uncomparable(
        &self,
        cx: &Cx<'a>,
        op: BinaryOp,
        x: &Operand,
        y: &Operand,
    ) -> Result<Option<(bool, String, Code)>, ()> {
        let ordered = |this: &Self, ty: TypeId| {
            this.open(ty)
                || this.all_types(ty, |this, ty| this.is_numeric(ty) || this.is_string(ty))
        };
        let comparable = |this: &Self, ty: TypeId| this.open(ty) || this.types.comparable(ty);
        // The operand that the error is about, and what is wrong with it.
        let wrong: Option<(bool, String, Code)> = {
            let mismatched = self.unfit(cx, x, y.ty, false) && self.unfit(cx, y, x.ty, false);
            // In a `.gopp` file a pointer of any kind, a func and an
            // interface with methods are never nil (OWN-P9).
            let with_nil = matches!(op, BinaryOp::Eq | BinaryOp::Ne)
                && self.own_rules(cx)
                && (self.is_nil(x) != self.is_nil(y));
            let other = if self.is_nil(x) { y.ty } else { x.ty };
            let is_struct = matches!(
                &*self.types.get(self.types.underlying(other)),
                Type::Struct(_)
            );
            if with_nil && self.never_nil(other) && !is_struct {
                let shown = self.type_string(other);
                let cause = format!("a {shown} is never nil: use Option[{shown}]");
                Some((self.is_nil(x), cause, Code::NilNeverNil))
            } else if mismatched {
                let cause = format!(
                    "mismatched types {} and {}",
                    self.type_string(x.ty),
                    self.type_string(y.ty)
                );
                Some((true, cause, Code::MismatchedTypes))
            } else if matches!(op, BinaryOp::Eq | BinaryOp::Ne) {
                // In a `.gopp` file a func is never nil, so it is compared
                // to nothing.
                let func = |this: &Self, ty: TypeId| {
                    let under = this.types.get(this.types.underlying(ty));
                    this.own_rules(cx) && matches!(&*under, Type::Func { .. })
                };
                let cause = |this: &Self, ty: TypeId| match func(this, ty) {
                    true => "func values cannot be compared".to_owned(),
                    false => this.incomparable_cause(ty),
                };
                if self.is_nil(x) || self.is_nil(y) {
                    let second = self.is_nil(x);
                    let other = if second { y.ty } else { x.ty };
                    let has_nil = self.open(other) || self.has_nil(other);
                    if func(self, other) {
                        let shown = self.type_string(other);
                        let cause = format!("a {shown} is never nil: use Option[{shown}]");
                        Some((second, cause, Code::NilNeverNil))
                    } else {
                        (!has_nil).then(|| (true, String::new(), Code::CannotCompare))
                    }
                } else if !comparable(self, x.ty) {
                    Some((false, cause(self, x.ty), Code::CannotCompare))
                } else if !comparable(self, y.ty) {
                    Some((true, cause(self, y.ty), Code::CannotCompare))
                } else {
                    None
                }
            } else if !ordered(self, x.ty) {
                Some((false, String::new(), Code::OperatorNotDefined))
            } else if !ordered(self, y.ty) {
                Some((true, String::new(), Code::OperatorNotDefined))
            } else {
                None
            }
        };
        let Some((mut second, mut cause, code)) = wrong else {
            return Ok(None);
        };
        if self.may_overload(cx, x.ty) || self.may_overload(cx, y.ty) {
            return Err(());
        }
        let is_param = |ty: TypeId| matches!(&*self.types.get(ty), Type::Param(_));
        if cause.is_empty() && (is_param(x.ty) || is_param(y.ty)) {
            second = !is_param(x.ty);
            let operand = if second { y } else { x };
            cause = format!(
                "type parameter {} cannot use operator {}",
                self.type_string(operand.ty),
                binary_text(op)
            );
        }
        if cause.is_empty() {
            let operand = if second { y } else { x };
            let what = match self.composite_kind(operand.ty) {
                "" => self.type_string(operand.ty),
                kind => kind.to_owned(),
            };
            cause = format!("operator {} not defined on {what}", binary_text(op));
        }
        Ok(Some((second, cause, code)))
    }

    fn comparison(
        &mut self,
        cx: &Cx<'a>,
        op: BinaryOp,
        (left, x): (ExprId, Operand),
        (right, y): (ExprId, Operand),
    ) -> Operand {
        if x.ty == Types::INVALID || y.ty == Types::INVALID {
            return Operand::UNKNOWN;
        }
        match self.uncomparable(cx, op, &x, &y) {
            Ok(None) => {}
            Err(()) => return Operand::UNKNOWN,
            Ok(Some((second, cause, code))) => {
                let message = format!(
                    "invalid operation: {} {} {} ({cause})",
                    self.expr_string(cx, left),
                    binary_text(op),
                    self.expr_string(cx, right)
                );
                let at = if second { right } else { left };
                self.error(code, self.span(cx, at), message);
                return Operand::UNKNOWN;
            }
        }
        if let (Mode::Const(a), Mode::Const(b)) = (&x.mode, &y.mode) {
            if *a == Value::Unknown || *b == Value::Unknown {
                return Operand::UNKNOWN;
            }
            let compare = match op {
                BinaryOp::Eq => CompareOp::Eq,
                BinaryOp::Ne => CompareOp::Ne,
                BinaryOp::Lt => CompareOp::Lt,
                BinaryOp::Le => CompareOp::Le,
                BinaryOp::Gt => CompareOp::Gt,
                _ => CompareOp::Ge,
            };
            return self.untyped(Basic::UntypedBool, Value::Bool(a.compare(compare, b)));
        }
        // Compared as values, both have their types for good.
        self.update_expr_type(cx, left, self.types.default_type(x.ty), true);
        self.update_expr_type(cx, right, self.types.default_type(y.ty), true);
        self.value_of(Basic::UntypedBool)
    }

    fn incomparable_cause(&self, ty: TypeId) -> String {
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Param(_) => match self.types.param_set(ty).is_some_and(|set| set.is_empty()) {
                true => "empty type set".to_owned(),
                false => "incomparable types in type set".to_owned(),
            },
            Type::Slice(_) | Type::Func { .. } | Type::Map { .. } => {
                format!("{} can only be compared to nil", self.composite_kind(ty))
            }
            Type::Struct(fields) => {
                let field = fields.iter().find(|field| !self.types.comparable(field.ty));
                field.map_or(String::new(), |field| {
                    format!(
                        "struct containing {} cannot be compared",
                        self.type_string(field.ty)
                    )
                })
            }
            Type::Array { .. } => format!("{} cannot be compared", self.type_string(ty)),
            _ => String::new(),
        }
    }

    fn shift(
        &mut self,
        cx: &Cx<'a>,
        whole: Option<ExprId>,
        op: BinaryOp,
        (left, mut x): (ExprId, Operand),
        (right, mut y): (ExprId, Operand),
    ) -> Operand {
        // A constant that is an integer, whatever its kind, is shifted
        // as one: `1.0 << 3`.
        let shifted = match &x.mode {
            Mode::Const(value) => Some(value.to_int()),
            _ => None,
        };
        let integer_constant = self.is_untyped(x.ty)
            && shifted
                .as_ref()
                .is_some_and(|value| value.kind() == Kind::Int);
        let operand_must_be_integer = |this: &mut Self, x: &Operand| {
            let message = format!(
                "invalid operation: shifted operand {} must be integer",
                this.describe(cx, left, x)
            );
            this.error(Code::ShiftOperandInvalid, this.span(cx, left), message);
            Operand::UNKNOWN
        };
        if !self.all_integer(x.ty) && !integer_constant {
            return operand_must_be_integer(self, &x);
        }

        let count = match &y.mode {
            Mode::Const(value) => {
                // -1 and 1.0 are integers here, -1.1 is not.
                let count = value.to_int();
                if count.kind() == Kind::Int && count.sign() < 0 {
                    let message = format!(
                        "invalid operation: negative shift count {}",
                        self.describe(cx, right, &y)
                    );
                    self.error(Code::ShiftCountNegative, self.span(cx, right), message);
                    return Operand::UNKNOWN;
                }
                if self.is_untyped(y.ty)
                    && let Err(unfit) = self.representation(value, y.ty, Basic::Uint)
                {
                    let uint = self.types.basic(Basic::Uint);
                    self.invalid_conversion(cx, unfit, right, &y, uint);
                    return Operand::UNKNOWN;
                }
                Some(count)
            }
            _ if self.all_integer(y.ty) => None,
            _ if self.is_untyped(y.ty) => {
                let uint = self.types.basic(Basic::Uint);
                if !self.convert_untyped(cx, right, &mut y, uint, true) {
                    return Operand::UNKNOWN;
                }
                None
            }
            _ => {
                let message = format!(
                    "invalid operation: shift count {} must be integer",
                    self.describe(cx, right, &y)
                );
                self.error(Code::ShiftCountInvalid, self.span(cx, right), message);
                return Operand::UNKNOWN;
            }
        };

        if let Some(shifted) = shifted {
            if let Some(count) = count {
                if shifted == Value::Unknown || count == Value::Unknown {
                    return Operand::UNKNOWN;
                }
                // So that the smallest float64 can be written as a shift.
                const BOUND: u64 = 1023 - 1 + 52;
                let bits = count.as_i64().and_then(|count| u64::try_from(count).ok());
                let Some(bits) = bits.filter(|&bits| bits <= BOUND) else {
                    let message = format!(
                        "invalid operation: invalid shift count {}",
                        self.describe(cx, right, &y)
                    );
                    self.error(Code::ShiftCountInvalid, self.span(cx, right), message);
                    return Operand::UNKNOWN;
                };
                if !self.is_integer(x.ty) {
                    x.ty = self.types.basic(Basic::UntypedInt);
                }
                let value = shifted.shift(op == BinaryOp::Shl, bits as u32);
                let mut result = Operand::constant(x.ty, value);
                if let Some(expr) = whole {
                    self.overflow(cx, expr, &mut result);
                }
                return result;
            }
            // A constant of no type shifted by a count that is no
            // constant gets its type from around the shift: `1 << s` is
            // an `int64` in `var x int64 = 1 << s`.
            if self.is_untyped(x.ty) {
                if self.memo.is_some() {
                    self.shifted.insert(left);
                }
                return Operand {
                    mode: Mode::Value,
                    ty: x.ty,
                };
            }
        }
        if !self.all_integer(x.ty) {
            return operand_must_be_integer(self, &x);
        }
        Operand::value(x.ty)
    }
}
