//! Composite literals, after `go/types` (literals.go).

use super::*;

impl<'a> Checker<'a> {
    /// The composite literal `expr`: of the type that `ty` writes, or of
    /// the type `hint` that the literal around it gives its elements.
    pub(super) fn composite(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        ty: Option<ExprId>,
        elems: &[ExprId],
        hint: Option<TypeId>,
    ) -> Operand {
        let span = cx.tree.exprs[expr].span;
        let (literal, base, is_element) = match (ty, hint) {
            (Some(ty), _) => {
                let literal = self.literal_type(cx, ty, elems);
                (literal, literal, false)
            }
            // `{…}` for a pointer type is `&T{…}`.
            (None, Some(hint)) => match &*self.types.get(self.types.underlying(hint)) {
                Type::Pointer(elem) => (hint, *elem, true),
                _ => (hint, hint, true),
            },
            (None, None) => {
                let message = "missing type in composite literal".to_owned();
                self.error(Code::LiteralMissingType, span, message);
                (Types::INVALID, Types::INVALID, false)
            }
        };
        // The elements are looked at when the body is walked.
        if self.memo.is_none() {
            return match self.types.underlying(literal) {
                Types::INVALID => Operand::UNKNOWN,
                _ => Operand::value(literal),
            };
        }
        // In a `.gopp` file a struct with parents has more than its own
        // fields.
        let strict = !self.types.extends(base);
        let under = self.types.get(self.types.underlying(base));
        let grows = ty.is_some_and(|ty| {
            matches!(
                cx.tree.exprs[ty].kind,
                ExprKind::ArrayType { len: None, .. }
            )
        });
        match &*under {
            Type::Struct(fields) => self.struct_elements(cx, expr, base, fields, elems, strict),
            Type::Array { len, elem } => {
                self.indexed_elements(cx, elems, *elem, Some(*len).filter(|_| !grows), strict);
            }
            Type::Slice(elem) => self.indexed_elements(cx, elems, *elem, None, strict),
            Type::Map { key, value } => self.map_elements(cx, elems, (*key, *value), strict),
            _ => {
                for &element in elems {
                    match cx.tree.exprs[element].kind {
                        ExprKind::KeyValue { value, .. } => self.element(cx, value, None),
                        _ => self.element(cx, element, None),
                    };
                }
                let unsettled =
                    self.types.underlying(base) == Types::INVALID || self.types.has_param(base);
                if !unsettled && strict {
                    let message = format!(
                        "invalid composite literal{} type {}",
                        if is_element { " element" } else { "" },
                        self.type_string(literal)
                    );
                    self.error(Code::LiteralInvalidType, span, message);
                }
                return Operand::UNKNOWN;
            }
        }
        // Nothing more is said about a type that is in error.
        match self.types.underlying(literal) {
            Types::INVALID => Operand::UNKNOWN,
            _ => Operand::value(literal),
        }
    }

    /// The element `expr` of a literal that gives its elements the type
    /// `hint`: written without a type, `{…}`, it has that type.
    fn element(&mut self, cx: &Cx<'a>, expr: ExprId, hint: Option<TypeId>) -> Operand {
        let ExprKind::CompositeLit { ty: None, elems } = &cx.tree.exprs[expr].kind else {
            return self.value(cx, expr);
        };
        let Some(hint) = hint else {
            return Operand::UNKNOWN;
        };
        let x = self.composite(cx, expr, None, elems, Some(hint));
        if let Some(memo) = &mut self.memo {
            memo.insert(expr, x.clone());
        }
        x
    }

    /// The element `expr`, assigned to a place of the type `to`.
    fn assign_element(&mut self, cx: &Cx<'a>, expr: ExprId, to: TypeId, context: &str) -> Operand {
        let x = self.element(cx, expr, Some(to));
        self.assignment(cx, expr, x, Some(to), context)
    }

    fn struct_elements(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        base: TypeId,
        fields: &[Field],
        elems: &[ExprId],
        strict: bool,
    ) {
        let tree = cx.tree;
        let shown = self.type_string(base);
        let mixture = "mixture of field:value and value elements in struct literal";
        let keyed = elems
            .first()
            .is_some_and(|&first| matches!(tree.exprs[first].kind, ExprKind::KeyValue { .. }));
        if !keyed {
            for (position, &element) in elems.iter().enumerate() {
                let span = tree.exprs[element].span;
                if let ExprKind::KeyValue { value, .. } = tree.exprs[element].kind {
                    self.element(cx, value, None);
                    if strict {
                        self.error(Code::LiteralMixture, span, mixture.to_owned());
                    }
                    continue;
                }
                let Some(field) = fields.get(position) else {
                    let x = self.element(cx, element, None);
                    if x.mode != Mode::Unknown && strict {
                        let message = format!("too many values in struct literal of type {shown}");
                        self.error(Code::LiteralTooMany, span, message);
                    }
                    break;
                };
                // A field of another package that is not exported.
                if field.package.is_some_and(|package| package != self.path) && strict {
                    self.element(cx, element, Some(field.ty));
                    let message = format!(
                        "implicit assignment to unexported field {} in struct literal of type \
                         {shown}",
                        self.names.resolve(&field.name)
                    );
                    self.error(Code::LiteralUnexportedField, span, message);
                    continue;
                }
                self.assign_element(cx, element, field.ty, "struct literal");
            }
            if !elems.is_empty() && elems.len() < fields.len() && strict {
                let span = tree.exprs[expr].span;
                let brace = Span {
                    lo: span.hi.saturating_sub(1),
                    ..span
                };
                let message = format!("too few values in struct literal of type {shown}");
                self.error(Code::LiteralTooFew, brace, message);
            }
            return;
        }
        let mut seen = vec![false; fields.len()];
        for &element in elems {
            let span = tree.exprs[element].span;
            let ExprKind::KeyValue { key, value } = tree.exprs[element].kind else {
                self.element(cx, element, None);
                if strict {
                    self.error(Code::LiteralMixture, span, mixture.to_owned());
                }
                continue;
            };
            let ExprKind::Name(name) = tree.exprs[key].kind else {
                self.element(cx, value, None);
                if strict {
                    let message = format!(
                        "invalid field name {} in struct literal",
                        self.expr_string(cx, key)
                    );
                    self.error(Code::LiteralInvalidFieldName, span, message);
                }
                continue;
            };
            let text = self.names.resolve(&name);
            let own = self.path;
            let visible = |field: &Field| field.package.is_none_or(|package| package == own);
            let at = fields
                .iter()
                .position(|field| field.name == name && visible(field));
            let Some(at) = at else {
                self.element(cx, value, None);
                if !strict {
                    continue;
                }
                // A field whose name differs in its capitals only is the
                // likely one.
                let other = fields.iter().find(|field| {
                    self.names.resolve(&field.name).to_lowercase() == text.to_lowercase()
                });
                let message = match other {
                    None => format!("unknown field {text} in struct literal of type {shown}"),
                    Some(other) => {
                        let alt = self.names.resolve(&other.name);
                        let same_tail = text.chars().skip(1).eq(alt.chars().skip(1));
                        let exported = |name: &str| name.starts_with(char::is_uppercase);
                        if visible(other) || exported(text) && exported(alt) {
                            format!(
                                "unknown field {text} in struct literal of type {shown}, but \
                                 does have {alt}"
                            )
                        } else if exported(text) && same_tail {
                            format!(
                                "unknown field {text} in struct literal of type {shown}, but \
                                 does have unexported {alt}"
                            )
                        } else if text == alt {
                            format!(
                                "cannot refer to unexported field {alt} in struct literal of \
                                 type {shown}"
                            )
                        } else {
                            format!("unknown field {text} in struct literal of type {shown}")
                        }
                    }
                };
                self.error(Code::LiteralUnknownField, tree.exprs[key].span, message);
                continue;
            };
            self.assign_element(cx, value, fields[at].ty, "struct literal");
            if std::mem::replace(&mut seen[at], true) && strict {
                let message = format!("duplicate field name {text} in struct literal");
                self.error(Code::LiteralDuplicateField, span, message);
            }
        }
    }

    /// The elements of an array or slice literal: each at the index after
    /// the one before it, or at the index that its key says.
    fn indexed_elements(
        &mut self,
        cx: &Cx<'a>,
        elems: &[ExprId],
        elem: TypeId,
        length: Option<u64>,
        strict: bool,
    ) {
        let tree = cx.tree;
        let mut seen = FxHashSet::default();
        let mut index = 0u64;
        for &element in elems {
            let span = tree.exprs[element].span;
            let mut value = element;
            let mut valid = false;
            if let ExprKind::KeyValue { key, value: of } = tree.exprs[element].kind {
                value = of;
                let x = self.key_operand(cx, key);
                if x.mode != Mode::Unknown {
                    match self.index_value(cx, key, length) {
                        Some(at) => {
                            index = at;
                            valid = true;
                        }
                        None if matches!(x.mode, Mode::Const(_)) => {}
                        None => {
                            let integer = self
                                .memo
                                .as_ref()
                                .and_then(|memo| memo.get(&key))
                                .is_some_and(|key| self.all_integer(key.ty));
                            if integer && strict {
                                let message = format!(
                                    "index {} must be integer constant",
                                    self.expr_string(cx, key)
                                );
                                self.error(Code::LiteralIndexNotConstant, span, message);
                            }
                        }
                    }
                }
            } else if let Some(length) = length.filter(|&length| index >= length) {
                if strict {
                    let message = format!("index {index} is out of bounds (>= {length})");
                    self.error(Code::LiteralArrayTooLong, span, message);
                }
            } else {
                valid = true;
            }
            if valid && !seen.insert(index) && strict {
                let message = format!("duplicate index {index} in array or slice literal");
                self.error(Code::LiteralDuplicateIndex, span, message);
            }
            index += 1;
            self.assign_element(cx, value, elem, "array or slice literal");
        }
    }

    /// A key of a map, array or slice literal that is a name: what the
    /// name denotes was found apart, since it may be a field in a struct
    /// literal.
    fn key_operand(&mut self, cx: &Cx<'a>, key: ExprId) -> Operand {
        // A key `{…}` has the type that the literal gives its keys.
        if matches!(
            cx.tree.exprs[key].kind,
            ExprKind::CompositeLit { ty: None, .. }
        ) {
            return Operand::value(self.types.any());
        }
        if let ExprKind::Name(name) = cx.tree.exprs[key].kind
            && !cx.body.uses.contains_key(&key)
            && !cx.body.keys.contains_key(&key)
            && name != self.blank
        {
            let message = format!("undefined: {}", self.names.resolve(&name));
            self.error(Code::Undefined, cx.tree.exprs[key].span, message);
            return Operand::UNKNOWN;
        }
        self.operand(cx, key)
    }

    fn map_elements(
        &mut self,
        cx: &Cx<'a>,
        elems: &[ExprId],
        (key_type, value_type): (TypeId, TypeId),
        strict: bool,
    ) {
        let tree = cx.tree;
        let any_key = self.types.is_interface(key_type);
        // The constant keys so far: in an interface, with their types.
        let mut seen: FxHashSet<(String, Option<TypeId>)> = FxHashSet::default();
        for &element in elems {
            let ExprKind::KeyValue { key, value } = tree.exprs[element].kind else {
                self.element(cx, element, None);
                if strict {
                    let message = "missing key in map literal".to_owned();
                    self.error(Code::LiteralMissingKey, tree.exprs[element].span, message);
                }
                continue;
            };
            if self.key_operand(cx, key).mode == Mode::Unknown {
                self.element(cx, value, Some(value_type));
                continue;
            }
            let x = self.assign_element(cx, key, key_type, "map literal");
            if x.mode == Mode::Unknown {
                continue;
            }
            if let Mode::Const(constant) = &x.mode {
                let identity = (constant.exact_string(), any_key.then_some(x.ty));
                if !seen.insert(identity) {
                    if strict {
                        let message = format!("duplicate key {constant} in map literal");
                        self.error(Code::LiteralDuplicateKey, tree.exprs[key].span, message);
                    }
                    continue;
                }
            }
            self.assign_element(cx, value, value_type, "map literal");
        }
    }
}
