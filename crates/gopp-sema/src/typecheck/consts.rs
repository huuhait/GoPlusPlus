//! Constants of Go++ (ADR-0033): a constant may be a struct, an array or
//! an option of constants; a `@Const` function is called in a constant
//! expression; and the body of a `@Const` function does nothing that
//! only a running program can do (EXP-31 to EXP-33).

use super::*;
use crate::constant::Composite;
use crate::consteval::{ConstCall, ConstFailure};
use crate::types::{Marks, NotConstant};

/// The largest array whose elements are kept one by one.
const KEPT_ELEMENTS: u64 = 1 << 16;

/// What a call calls.
enum Callee {
    /// A function of a package, with its attributes.
    Func(Marks, Spur),
    /// A method of a declared type, which is the second.
    Method(Marks, TypeId, Spur),
    /// A method of a type parameter: the instance says what it is.
    OfParam,
    /// `o.IsNil()`, `o.Get()` and `w.Lock()`, which the language has.
    OfHandle,
    /// What only a running program knows.
    Dynamic(&'static str),
}

impl<'a> Checker<'a> {
    /// Says why the type `ty`, which `expr` writes, is not the type of a
    /// constant. `false` if it is one.
    pub(super) fn not_constant_type(&mut self, cx: &Cx<'a>, expr: ExprId, ty: TypeId) -> bool {
        let Some((part, why)) = self.types.not_constant(ty, self.names) else {
            return false;
        };
        let shown = self.type_string(part);
        let cause = match why {
            NotConstant::Handle => format!("a {shown} names a value of the running program"),
            NotConstant::Buffer => {
                format!("the buffer of a {shown} is made by the running program")
            }
            NotConstant::Func => format!("{shown} is a func type"),
            NotConstant::Interface => format!("{shown} is an interface type"),
            NotConstant::Pinned => format!("{shown} is @Pinned"),
            NotConstant::Drop => format!("{shown} has a drop method"),
            NotConstant::Table => format!("{shown} has a type table"),
            NotConstant::Param => format!("{shown} is a type parameter"),
            NotConstant::Other => format!("a {shown} is no constant"),
        };
        let message = format!("invalid constant type {}: {cause}", self.type_string(ty));
        self.error(Code::InvalidConstantType, cx.tree.exprs[expr].span, message);
        true
    }

    /// The zero value of the type as a constant; unknown for a type that
    /// has none here.
    fn zero_constant(&self, ty: TypeId) -> Value {
        let under = self.types.underlying(ty);
        let composite = |parts: Composite| Value::Composite(std::sync::Arc::new(parts));
        match &*self.types.get(under) {
            Type::Basic(_) if self.is_boolean(under) => Value::Bool(false),
            Type::Basic(_) if self.is_string(under) => Value::string(b""),
            Type::Basic(_) => self
                .converted(&Value::Int64(0), under)
                .unwrap_or(Value::Unknown),
            Type::Option(_) => composite(Composite::Nil),
            Type::Array { len, elem } if *len <= KEPT_ELEMENTS => {
                let zero = gopp_syntax::with_stack(|| self.zero_constant(*elem));
                composite(Composite::Array(vec![zero; *len as usize]))
            }
            Type::Struct(fields) if self.types.parents(ty).is_empty() => {
                let zero = |field: &Field| gopp_syntax::with_stack(|| self.zero_constant(field.ty));
                composite(Composite::Struct(fields.iter().map(zero).collect()))
            }
            _ => Value::Unknown,
        }
    }

    /// The constant operand `x` as a value of the type `ty`.
    fn constant_as(&self, x: &Operand, ty: TypeId) -> Option<Value> {
        let Mode::Const(value) = &x.mode else {
            return None;
        };
        Some(match self.implicit(x, ty) {
            Ok((_, Some(converted))) => converted,
            _ => value.clone(),
        })
    }

    /// The composite literal with the elements `elems` of the type `ty`
    /// as a constant, in a constant expression: every element is one
    /// (EXP-31). `None` if it is no constant.
    pub(super) fn literal_constant(
        &mut self,
        cx: &Cx<'a>,
        ty: TypeId,
        elems: &[ExprId],
    ) -> Option<Value> {
        if !self.in_const || self.types.not_constant(ty, self.names).is_some() {
            return None;
        }
        let under = self.types.get(self.types.underlying(ty));
        let keyed = |element: ExprId| match cx.tree.exprs[element].kind {
            ExprKind::KeyValue { key, value } => (Some(key), value),
            _ => (None, element),
        };
        let composite = |parts: Composite| Value::Composite(std::sync::Arc::new(parts));
        match &*under {
            Type::Struct(fields) => {
                // The parts of a parent are not kept one by one.
                let kept = self.types.parents(ty).is_empty();
                // A field that is not written has its zero value.
                let mut values: Vec<Option<Value>> = vec![None; fields.len()];
                for (position, &element) in elems.iter().enumerate() {
                    let (key, value) = keyed(element);
                    let at = match key.map(|key| &cx.tree.exprs[key].kind) {
                        Some(ExprKind::Name(name)) => {
                            fields.iter().position(|field| field.name == *name)
                        }
                        Some(_) => None,
                        None => Some(position),
                    };
                    let field = at.and_then(|at| fields.get(at)).map(|field| field.ty);
                    let x = self.element(cx, value, field);
                    let held = self.constant_as(&x, field.unwrap_or(x.ty))?;
                    if let Some(place) = at.and_then(|at| values.get_mut(at)) {
                        *place = Some(held);
                    }
                }
                if !kept {
                    return Some(Value::Unknown);
                }
                let values = values
                    .into_iter()
                    .zip(fields.iter())
                    .map(|(value, field)| value.unwrap_or_else(|| self.zero_constant(field.ty)));
                Some(composite(Composite::Struct(values.collect())))
            }
            Type::Array { len, elem } => {
                let kept = *len <= KEPT_ELEMENTS;
                let mut values: Vec<Option<Value>> = match kept {
                    true => vec![None; *len as usize],
                    false => Vec::new(),
                };
                let mut next = 0;
                for &element in elems {
                    let (key, value) = keyed(element);
                    if let Some(key) = key {
                        let index = self.value(cx, key);
                        let Mode::Const(index) = &index.mode else {
                            return None;
                        };
                        next = index.as_i64().and_then(|at| usize::try_from(at).ok())?;
                    }
                    let x = self.element(cx, value, Some(*elem));
                    let held = self.constant_as(&x, *elem)?;
                    if let Some(place) = values.get_mut(next) {
                        *place = Some(held);
                    }
                    next += 1;
                }
                if !kept {
                    return Some(Value::Unknown);
                }
                // One zero value stands in every place that is not written.
                let mut zero = None;
                let values = values.into_iter().map(|value| {
                    let zero = || {
                        zero.get_or_insert_with(|| self.zero_constant(*elem))
                            .clone()
                    };
                    value.unwrap_or_else(zero)
                });
                Some(composite(Composite::Array(values.collect())))
            }
            _ => None,
        }
    }

    /// The field at the end of `path` of the constant struct `held`.
    pub(super) fn constant_field(&self, held: &Value, path: &[u32]) -> Value {
        let mut value = held.clone();
        for &step in path {
            value = match &value {
                Value::Composite(parts) => match &**parts {
                    Composite::Struct(fields) => {
                        fields.get(step as usize).cloned().unwrap_or(Value::Unknown)
                    }
                    _ => Value::Unknown,
                },
                _ => Value::Unknown,
            };
        }
        value
    }

    /// The element `at` of the constant array or string `held`.
    pub(super) fn constant_element(&self, held: &Value, at: u64) -> Value {
        match held {
            Value::Composite(parts) => match &**parts {
                Composite::Array(elements) => {
                    elements.get(at as usize).cloned().unwrap_or(Value::Unknown)
                }
                _ => Value::Unknown,
            },
            Value::Str(text) => {
                let byte = text.bytes().get(at as usize);
                byte.map_or(Value::Unknown, |&byte| Value::Int64(i64::from(byte)))
            }
            _ => Value::Unknown,
        }
    }

    /// What the call of `fun` calls.
    fn callee(&mut self, cx: &Cx<'a>, fun: ExprId) -> Callee {
        let fun = self.unparen(cx, fun);
        let func = "a func value";
        let of_object = |this: &Self, package: &Package, object: ObjId, own: bool| {
            let declared = package.object(object);
            match declared.kind {
                ObjKind::Func if own => {
                    Callee::Func(this.func_marks[object.0 as usize], declared.name)
                }
                ObjKind::Func => Callee::Func(declared.marks, declared.name),
                _ => Callee::Dynamic(func),
            }
        };
        match cx.tree.exprs[fun].kind {
            ExprKind::Name(_) => match cx.body.uses.get(&fun) {
                Some(&Ref::Object(object)) => of_object(self, self.package, object, true),
                _ => Callee::Dynamic(func),
            },
            // A generic function with its type arguments.
            ExprKind::Index { x, .. } => self.callee(cx, x),
            ExprKind::Selector { x, sel } => {
                if let Some(&Ref::Member { import, member }) = cx.body.uses.get(&fun) {
                    return match &self.package.object(import).kind {
                        ObjKind::Import { package, .. } => of_object(self, package, member, false),
                        _ => Callee::Dynamic(func),
                    };
                }
                let base = self.value(cx, x);
                if base.is_value() && self.handle_method(&base, sel.name).is_some() {
                    return Callee::OfHandle;
                }
                let base = self.read(base);
                if !base.is_value() {
                    return Callee::Dynamic(func);
                }
                let found = self
                    .types
                    .lookup(base.ty, sel.name, self.path, self.names, false);
                match found {
                    Ok(found) if matches!(found.member, Member::Method { .. }) => {
                        let holder = found.holder;
                        if self.is_param(holder) {
                            Callee::OfParam
                        } else if self.types.is_interface(holder) {
                            Callee::Dynamic("a method of an interface")
                        } else {
                            let package = self.package_of(sel.name);
                            let marks = self.types.method_marks(holder, sel.name, package);
                            Callee::Method(marks, holder, sel.name)
                        }
                    }
                    _ => Callee::Dynamic(func),
                }
            }
            _ => Callee::Dynamic(func),
        }
    }

    /// The call `call` of `fun` in a constant expression gave `result`:
    /// a `@Const` function with constant arguments gives a constant, and
    /// no other function is called there (EXP-32).
    pub(super) fn constant_call(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        fun: ExprId,
        args: &[ExprId],
        result: Operand,
    ) -> Operand {
        let at = self.span(cx, call);
        let refused = |this: &mut Self| {
            let message = format!(
                "cannot call non-@Const function {} in constant expression",
                this.expr_string(cx, fun)
            );
            this.error(Code::ConstCall, at, message);
            Operand::UNKNOWN
        };
        let (receiver, name) = match self.callee(cx, fun) {
            Callee::Func(marks, name) if marks.has(Marks::CONST) => (None, name),
            Callee::Method(marks, holder, name) if marks.has(Marks::CONST) => (Some(holder), name),
            // What an option says of itself is asked of a value.
            Callee::OfHandle => return result,
            _ => return refused(self),
        };
        if !result.is_value() {
            return result;
        }
        let sig = self.value(cx, fun).ty;
        let (params, ..) = self.func_parts(sig);
        let mut values = Vec::new();
        // The receiver of a method is its first argument.
        if receiver.is_some()
            && let ExprKind::Selector { x, .. } = cx.tree.exprs[self.unparen(cx, fun)].kind
        {
            let base = self.value(cx, x);
            let Mode::Const(value) = self.read(base).mode else {
                return result;
            };
            values.push(value);
        }
        for (position, &arg) in args.iter().enumerate() {
            let x = self.value(cx, arg);
            let param = params.get(position).copied().unwrap_or(x.ty);
            let Some(value) = self.constant_as(&x, param) else {
                return result;
            };
            values.push(value);
        }
        self.constant_result(cx, (call, at), result.ty, (receiver, name, sig), &values)
    }

    /// The constant that the `@Const` function or method `name` gives
    /// for the constant arguments `args`: of the type `ty`, with the
    /// value that the evaluator finds, if there is one.
    pub(super) fn constant_result(
        &mut self,
        cx: &Cx<'a>,
        (expr, at): (ExprId, Span),
        ty: TypeId,
        (receiver, name, sig): (Option<TypeId>, Spur, TypeId),
        args: &[Value],
    ) -> Operand {
        // A value without a type is of its kind.
        let typed = self.types.default_type(ty);
        if matches!(&*self.types.get(typed), Type::Tuple(_))
            || self.not_constant_type(cx, expr, typed)
        {
            return Operand::UNKNOWN;
        }
        let known = args.iter().all(|arg| *arg != Value::Unknown);
        let Some(evaluator) = self.evaluator.filter(|_| known) else {
            return Operand::constant(ty, Value::Unknown);
        };
        let package = self.package.path.as_str();
        let call = ConstCall {
            package: receiver.is_none().then_some(package),
            receiver,
            name,
            sig,
            args,
        };
        match evaluator.call(&call) {
            Ok(value) => Operand::constant(ty, value),
            Err(ConstFailure { message, stack }) => {
                let message = format!("constant expression fails: {message}");
                let mut diagnostic = Diagnostic::new(Code::ConstEval, at, message);
                for function in stack {
                    let note = format!("in {function}");
                    diagnostic = diagnostic.with_note(Code::NoteRefersTo, None, note);
                }
                if self.report_once((at, Code::ConstEval)) {
                    self.emit(diagnostic);
                }
                Operand::UNKNOWN
            }
        }
    }

    /// The `@Operator` method `method` stands for an expression at `at`
    /// that gave `result`, with the operands `operands`: in a constant
    /// expression it is `@Const`, and gives a constant for constants.
    pub(super) fn constant_operator(
        &mut self,
        cx: &Cx<'a>,
        (expr, at): (ExprId, Span),
        method: &crate::types::Operator,
        operands: &[&Operand],
        result: Operand,
    ) -> Operand {
        if !self.in_const {
            return result;
        }
        if !method.marks.has(Marks::CONST) {
            let message = format!(
                "cannot call non-@Const method {}.{} in constant expression",
                self.type_string(method.holder),
                self.names.resolve(&method.name)
            );
            self.error(Code::ConstCall, at, message);
            return Operand::UNKNOWN;
        }
        let (params, ..) = self.func_parts(method.sig);
        // The receiver first, then the operand that the method takes.
        let mut values = Vec::new();
        for (position, operand) in operands.iter().enumerate() {
            let param = match position {
                0 => operand.ty,
                _ => params.get(position - 1).copied().unwrap_or(operand.ty),
            };
            let Some(value) = self.constant_as(operand, param) else {
                return result;
            };
            values.push(value);
        }
        let callee = (Some(method.holder), method.name, method.sig);
        self.constant_result(cx, (expr, at), result.ty, callee, &values)
    }

    /// The body of the `@Const` function that is being checked does at
    /// `at` what only a running program can do (EXP-33).
    pub(super) fn impure(&mut self, at: Span, what: &str) {
        let Some(name) = self.const_fn else {
            return;
        };
        let message = format!(
            "invalid @Const function {}: it {what}",
            self.names.resolve(&name)
        );
        self.error(Code::ConstImpure, at, message);
    }

    /// A `@Const` function calls `fun`: a `@Const` function, or a method
    /// of a type parameter.
    pub(super) fn pure_call(&mut self, cx: &Cx<'a>, fun: ExprId) {
        if self.const_fn.is_none() {
            return;
        }
        let at = self.span(cx, fun);
        let written = self.expr_string(cx, fun);
        match self.callee(cx, fun) {
            Callee::Func(marks, _) | Callee::Method(marks, ..) if marks.has(Marks::CONST) => {}
            Callee::OfParam | Callee::OfHandle => {}
            Callee::Func(..) | Callee::Method(..) => {
                self.impure(at, &format!("calls {written}, which is not @Const"));
            }
            Callee::Dynamic(what) => self.impure(at, &format!("calls {what}")),
        }
    }

    /// A `@Const` function calls the predeclared function `builtin`.
    pub(super) fn pure_builtin(&mut self, cx: &Cx<'a>, fun: ExprId, builtin: Predeclared) {
        use Predeclared::*;
        let pure = matches!(
            builtin,
            Len | Cap
                | Min
                | Max
                | Make
                | Append
                | CopyFunc
                | Clear
                | ComplexFunc
                | Real
                | Imag
                | Panic
                | Move
        );
        if !pure {
            let written = self.expr_string(cx, fun);
            self.impure(self.span(cx, fun), &format!("calls {written}"));
        }
    }

    /// A `@Const` function writes the type `ty` at `expr`: no map, no
    /// channel, and no type with a `drop` method.
    pub(super) fn pure_type(&mut self, cx: &Cx<'a>, expr: ExprId, ty: TypeId) {
        if self.const_fn.is_none() {
            return;
        }
        let what = match &*self.types.get(ty) {
            Type::Map { .. } => "uses a map".to_owned(),
            Type::Chan { .. } => "uses a channel".to_owned(),
            Type::Named { .. } => match self.types.not_constant(ty, self.names) {
                Some((part, NotConstant::Drop)) => {
                    format!("uses {}, which has a drop method", self.type_string(part))
                }
                _ => return,
            },
            _ => return,
        };
        self.impure(self.span(cx, expr), &what);
    }
}
