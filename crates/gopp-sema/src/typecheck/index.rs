//! Index and slice expressions, after `go/types` (index.go).

use super::*;

impl<'a> Checker<'a> {
    /// Whether the operand `x` of `expr` is an index: an integer, and as
    /// a constant not negative, unless `negative` allows it, and no larger
    /// than an `int`. `what` names it in the messages.
    pub(super) fn valid_index(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &mut Operand,
        what: &str,
        negative: bool,
    ) -> bool {
        let int = self.types.basic(Basic::Int);
        if x.mode == Mode::Unknown || !self.convert_untyped(cx, expr, x, int, true) {
            return false;
        }
        let span = cx.tree.exprs[expr].span;
        let wrong = if !self.all_integer(x.ty) {
            "must be integer"
        } else {
            match &x.mode {
                Mode::Const(value) if !negative && value.sign() < 0 => "must not be negative",
                Mode::Const(value) if representable(value, Basic::Int).is_none() => "overflows int",
                _ => return true,
            }
        };
        let message = format!(
            "invalid argument: {what} {} {wrong}",
            self.describe(cx, expr, x)
        );
        self.error(Code::InvalidIndex, span, message);
        false
    }

    /// The index `expr` into what has `length` elements, if that is
    /// known: its value if it is a constant.
    pub(super) fn index_value(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        length: Option<u64>,
    ) -> Option<u64> {
        let mut x = self.value(cx, expr);
        if !self.valid_index(cx, expr, &mut x, "index", false) {
            return None;
        }
        let Mode::Const(value) = &x.mode else {
            return None;
        };
        let value = u64::try_from(value.as_i64()?).ok()?;
        if let Some(length) = length.filter(|&length| value >= length) {
            let message = format!("invalid argument: index {value} out of bounds [0:{length}]");
            self.error(Code::IndexOutOfBounds, cx.tree.exprs[expr].span, message);
            return None;
        }
        Some(value)
    }

    /// The number of bytes of a constant string.
    fn constant_length(&self, x: &Operand) -> Option<u64> {
        match &x.mode {
            Mode::Const(Value::Str(text)) => Some(text.bytes().len() as u64),
            _ => None,
        }
    }

    pub(super) fn index(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        of: ExprId,
        indices: &[ExprId],
    ) -> Operand {
        if let Some(instance) = self.foreign_index(cx, expr, of, indices) {
            return instance;
        }
        let x = self.operand(cx, of);
        if x.mode == Mode::Type {
            // A generic type with its arguments.
            return Operand {
                mode: Mode::Type,
                ty: self.typ(cx, expr),
            };
        }
        if !x.is_value() {
            for &index in indices {
                self.operand(cx, index);
            }
            return Operand::UNKNOWN;
        }
        let x = self.read(cx, x);
        if let Some(element) = self.operator_index(cx, expr, of, &x, indices) {
            return element;
        }
        let under = self.types.get(self.types.underlying(x.ty));
        let (length, element) = match &*under {
            // A generic function with its type arguments.
            Type::Func { .. } => {
                let Some(params) = self.types.generic_func(x.ty) else {
                    return Operand::UNKNOWN;
                };
                let args: Vec<TypeId> = indices.iter().map(|&i| self.pointee_type(cx, i)).collect();
                if args.len() > params.len() {
                    let message = format!(
                        "got {} type arguments but want {}",
                        args.len(),
                        params.len()
                    );
                    let last = cx.tree.exprs[indices[args.len() - 1]].span;
                    self.error(Code::GotNTypeArgsFunc, last, message);
                    return Operand::UNKNOWN;
                }
                if args.contains(&Types::INVALID) {
                    return Operand::UNKNOWN;
                }
                // With the first type arguments only, the function is
                // still generic: a call finds the others.
                if args.len() < params.len() {
                    let spans = indices.iter().map(|&index| cx.tree.exprs[index].span);
                    self.partial
                        .insert(expr, args.into_iter().zip(spans).collect());
                    return x;
                }
                let instance = self.types.substitute(x.ty, &params, &args);
                let spans = indices.iter().map(|&index| cx.tree.exprs[index].span);
                self.note_instance(expr, &args, instance);
                self.instances.push(super::constraints::Instance {
                    params,
                    args,
                    spans: spans.collect(),
                    at: cx.tree.exprs[expr].span,
                    skip_cycle: false,
                });
                return Operand::value(instance);
            }
            Type::Basic(Basic::String | Basic::UntypedString) => {
                let byte = self.types.byte();
                (self.constant_length(&x), Operand::value(byte))
            }
            // An element of a slice is a variable, one of an array if the
            // array is, one of a map can only be assigned to.
            Type::Array { len, elem } => match x.mode {
                Mode::Variable => (Some(*len), Operand::variable(*elem)),
                _ => (Some(*len), Operand::value(*elem)),
            },
            Type::Pointer(array) => match &*self.types.get(self.types.underlying(*array)) {
                Type::Array { len, elem } => (Some(*len), Operand::variable(*elem)),
                _ => return self.cannot_index(cx, of, &x, indices),
            },
            Type::Slice(elem) => (None, Operand::variable(*elem)),
            Type::Map { key, value } => return self.map_index(cx, indices, *key, *value),
            Type::Param(_) if !self.open(x.ty) => match self.indexed_param(&x) {
                Some((Some(key), _, element)) => {
                    return self.map_index(cx, indices, key, element.ty);
                }
                Some((None, length, element)) => (length, element),
                None => return self.cannot_index(cx, of, &x, indices),
            },
            _ => return self.cannot_index(cx, of, &x, indices),
        };
        let Some(index) = self.single_index(cx, indices) else {
            return Operand::UNKNOWN;
        };
        let at = self.index_value(cx, index, length);
        // An element of a constant at a constant index is a constant
        // (EXP-31).
        match (&x.mode, at) {
            (Mode::Const(held), Some(at)) if self.own_rules(cx) => {
                Operand::constant(element.ty, self.constant_element(held, at))
            }
            _ => element,
        }
    }

    /// `m[key]` for a map with the key type `key` and the value type
    /// `value`.
    fn map_index(
        &mut self,
        cx: &Cx<'a>,
        indices: &[ExprId],
        key: TypeId,
        value: TypeId,
    ) -> Operand {
        let Some(index) = self.single_index(cx, indices) else {
            return Operand::UNKNOWN;
        };
        self.assign(cx, index, Some(key), "map index");
        match value {
            invalid if invalid == Types::INVALID => Operand::UNKNOWN,
            ty => Operand {
                mode: Mode::MapIndex,
                ty,
            },
        }
    }

    /// What indexing a value of a type parameter gives, if every type
    /// that it may be is indexed and all give elements of one type: the
    /// key type if they are maps, the smallest length that is known, and
    /// the element.
    fn indexed_param(&self, x: &Operand) -> Option<(Option<TypeId>, Option<u64>, Operand)> {
        let types = self.types.specific_types(x.ty)?;
        let byte = self.types.byte();
        let mut variable = true;
        let mut shared: Option<(Option<TypeId>, Option<u64>, TypeId)> = None;
        for (_, under) in types {
            let (key, length, elem) = match &*self.types.get(under) {
                Type::Basic(Basic::String) => {
                    variable = false;
                    (None, None, byte)
                }
                Type::Array { len, elem } => {
                    variable &= x.mode == Mode::Variable;
                    (None, Some(*len), *elem)
                }
                Type::Pointer(array) => match &*self.types.get(self.types.underlying(*array)) {
                    Type::Array { len, elem } => (None, Some(*len), *elem),
                    _ => return None,
                },
                Type::Slice(elem) => (None, None, *elem),
                Type::Map { key, value } => (Some(*key), None, *value),
                _ => return None,
            };
            shared = match shared {
                None => Some((key, length, elem)),
                Some((first_key, first_length, first_elem)) => {
                    // Maps are not mixed with other types, and all keys
                    // and all elements are of one type.
                    let same_key = match (first_key, key) {
                        (Some(a), Some(b)) => self.types.identical(a, b),
                        (None, None) => true,
                        _ => false,
                    };
                    if !same_key || !self.types.identical(first_elem, elem) {
                        return None;
                    }
                    let length = match (first_length, length) {
                        (Some(a), Some(b)) => Some(a.min(b)),
                        (first, _) => first,
                    };
                    Some((first_key, length, first_elem))
                }
            };
        }
        let (key, length, elem) = shared?;
        let element = match variable {
            true => Operand::variable(elem),
            false => Operand::value(elem),
        };
        Some((key, length, element))
    }

    /// The type that slicing a value of a type parameter works on: what
    /// all the types that it may be share, a string counting as a slice
    /// of bytes. With a string among them, that is the string type.
    fn sliced_param(&mut self, cx: &Cx<'a>, of: ExprId, x: &Operand) -> Option<TypeId> {
        let (string, byte) = (self.types.basic(Basic::String), self.types.byte());
        let bytes = self.types.slice(byte);
        let cause = match self.types.specific_types(x.ty) {
            None => Some(format!("no specific type in {}", self.type_string(x.ty))),
            Some(types) => {
                let as_slice = |under: TypeId| if under == string { bytes } else { under };
                let (first, shared) = (types[0].0, as_slice(types[0].1));
                let other = types
                    .iter()
                    .find(|&&(_, under)| !self.types.identical(shared, as_slice(under)));
                match other {
                    Some(&(other, _)) => Some(format!(
                        "{} and {} have different underlying types",
                        self.type_string(first),
                        self.type_string(other)
                    )),
                    None if types.iter().any(|&(_, under)| under == string) => return Some(string),
                    None => return Some(shared),
                }
            }
        };
        let message = format!(
            "cannot slice {}: {}",
            self.describe(cx, of, x),
            cause.unwrap_or_default()
        );
        self.error(Code::CannotSlice, cx.tree.exprs[of].span, message);
        None
    }

    /// The first of the indices, which is the only one that counts.
    fn single_index(&mut self, cx: &Cx<'a>, indices: &[ExprId]) -> Option<ExprId> {
        if let Some(&second) = indices.get(1) {
            let message = "invalid operation: more than one index".to_owned();
            self.error(Code::InvalidIndex, cx.tree.exprs[second].span, message);
        }
        indices.first().copied()
    }

    fn cannot_index(
        &mut self,
        cx: &Cx<'a>,
        of: ExprId,
        x: &Operand,
        indices: &[ExprId],
    ) -> Operand {
        for &index in indices {
            self.operand(cx, index);
        }
        // What a type in error can do is not said.
        if !self.open(x.ty) {
            let message = format!("cannot index {}", self.describe(cx, of, x));
            self.error(Code::CannotIndex, cx.tree.exprs[of].span, message);
        }
        Operand::UNKNOWN
    }

    pub(super) fn slice(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        of: ExprId,
        bounds: [Option<ExprId>; 3],
        full: bool,
    ) -> Operand {
        let x = self.value(cx, of);
        let x = self.read(cx, x);
        self.sliced(cx, of);
        let span = cx.tree.exprs[of].span;
        let sliced = match x.mode == Mode::Unknown || self.open(x.ty) {
            true => None,
            false if self.is_param(x.ty) => self.sliced_param(cx, of, &x),
            false => Some(self.types.underlying(x.ty)),
        };
        let Some(sliced) = sliced else {
            for bound in bounds.into_iter().flatten() {
                self.operand(cx, bound);
            }
            return Operand::UNKNOWN;
        };
        let under = self.types.get(sliced);
        let array_in = |pointer: TypeId| match &*self.types.get(self.types.underlying(pointer)) {
            Type::Array { len, elem } => Some((*len, *elem)),
            _ => None,
        };
        let (length, sliced) = match &*under {
            Type::Basic(basic @ (Basic::String | Basic::UntypedString)) => {
                if full {
                    let at =
                        bounds[2].map_or(cx.tree.exprs[expr].span, |max| cx.tree.exprs[max].span);
                    let message = "invalid operation: 3-index slice of string".to_owned();
                    self.error(Code::SliceOfString, at, message);
                    return Operand::UNKNOWN;
                }
                let ty = match basic {
                    Basic::UntypedString => self.types.basic(Basic::String),
                    _ => x.ty,
                };
                (self.constant_length(&x), ty)
            }
            Type::Array { len, elem } => {
                if x.mode != Mode::Variable {
                    let message = format!(
                        "cannot slice unaddressable value {}",
                        self.describe(cx, of, &x)
                    );
                    self.error(Code::CannotSliceUnaddressable, span, message);
                    return Operand::UNKNOWN;
                }
                (Some(*len), self.types.slice(*elem))
            }
            Type::Slice(_) => (None, x.ty),
            other => match other {
                Type::Pointer(array) => array_in(*array),
                _ => None,
            }
            .map(|(len, elem)| (Some(len), self.types.slice(elem)))
            .unwrap_or_else(|| {
                let message = format!("cannot slice {}", self.describe(cx, of, &x));
                self.error(Code::CannotSlice, span, message);
                (None, Types::INVALID)
            }),
        };
        if sliced == Types::INVALID {
            return Operand::UNKNOWN;
        }
        // The values of the bounds that are known: what is left out is 0
        // at the start and the length at the end.
        let mut known: [Option<u64>; 3] = [Some(0), length, length];
        for (at, bound) in bounds.into_iter().enumerate() {
            if let Some(bound) = bound {
                known[at] = self.index_value(cx, bound, length.map(|length| length + 1));
            }
        }
        'order: for (at, low) in known.into_iter().enumerate() {
            let Some(low) = low.filter(|&low| low > 0) else {
                continue;
            };
            for later in at + 1..3 {
                if let (Some(high), Some(bound)) = (known[later], bounds[later])
                    && high < low
                {
                    let message = format!("invalid slice indices: {high} < {low}");
                    self.error(
                        Code::SliceIndicesInverted,
                        cx.tree.exprs[bound].span,
                        message,
                    );
                    break 'order;
                }
            }
        }
        Operand::value(sliced)
    }
}
