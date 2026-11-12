//! Method declarations: what a method may be declared for, and the names
//! of the methods and the fields of a type. The checks follow `go/types`
//! (signature.go, decl.go), and the messages are Go's.

use super::*;
use crate::types::NamedId;

/// The receiver `(r *T[P])` of a method, taken apart.
struct Receiver {
    /// Written with a star.
    pointer: bool,
    /// `T[P]`: the receiver without its star.
    named: ExprId,
    /// `T`, or what is written in its place.
    base: ExprId,
    /// What the brackets name.
    indices: Vec<ExprId>,
}

impl<'a> Checker<'a> {
    /// Declares the methods of the file for their types. Every method
    /// gets its name first; `with_signatures` gives each its signature and
    /// reports what is wrong with its receiver.
    pub(super) fn methods(&mut self, file: usize, unit: Unit<'a>, with_signatures: bool) {
        let tree = unit.file;
        for (index, &decl) in tree.decls.iter().enumerate() {
            let node = &tree.decl_nodes[decl];
            let DeclKind::Func {
                receiver: Some(receiver),
                name,
                type_params,
                ty,
                ..
            } = &node.kind
            else {
                continue;
            };
            let body = &self.package.files[file].bodies[index];
            // What was incomplete in the method before is not in this one.
            self.incomplete = false;
            let decl = Decl {
                unit,
                body,
                form: Form::Func {
                    type_params: &[],
                    ty: *ty,
                },
            };
            let mut cx = self.cx(&decl, None);
            let parts = Self::receiver_parts(tree, receiver.ty);
            let receiver_params = self.named_by_receiver(&mut cx, &parts, with_signatures);
            if with_signatures {
                self.check_receiver(&cx, receiver.ty, &parts, receiver_params.len());
            }
            let Some((named, pointer_receiver)) = self.receiver_base(&cx, &parts) else {
                continue;
            };
            if type_params.is_empty() {
                self.mono_canon(&receiver_params, named);
            }
            // A method that is named `_` is no method of its type.
            if name.name == self.blank {
                continue;
            }
            let package = self.package_of(name.name);
            let first = *self
                .method_names
                .entry((named, name.name, package))
                .or_insert(name.span);
            if first != name.span {
                if !with_signatures {
                    self.method_again(named, *name, first);
                }
                continue;
            }
            if !with_signatures {
                self.method_nodes.insert(name.span, self.needs.len());
                self.needs.push(Vec::new());
                self.methods_declared.push(*name);
            }
            // A method may have type parameters of its own.
            let sig = match with_signatures {
                true => {
                    let own = self.declare_params(&mut cx, type_params);
                    if !own.is_empty() {
                        self.mono_method(name.span, &receiver_params, &own);
                    }
                    self.in_method_sig = true;
                    let sig = self.typ(&cx, *ty);
                    self.in_method_sig = false;
                    match own.is_empty() {
                        true => sig,
                        false => self.types.generic_func_type(sig, &own),
                    }
                }
                false => Types::INVALID,
            };
            // The method that ends a value: `func (t *T) drop()` (OWN-R7).
            if self.own_rules(&cx) && self.names.resolve(&name.name) == "drop" {
                self.types.set_drops(named);
                let plain = matches!(&*self.types.get(sig), Type::Func { type_params, params, results, .. }
                    if type_params.is_empty() && params.is_empty() && results.is_empty());
                if with_signatures && !(plain && pointer_receiver) {
                    let message = "invalid drop method: it has a pointer receiver, no parameters \
                                   and no results";
                    self.error(Code::OwnDropSignature, name.span, message.to_owned());
                }
            }
            let method = Method {
                name: name.name,
                package,
                sig,
            };
            self.types.add_method(
                named,
                DeclaredMethod {
                    method,
                    marks: self.marks(&cx, &node.attrs),
                    pointer_receiver,
                    receiver_params,
                },
            );
        }
        // A method names no object, so nothing waits for what was
        // incomplete in it.
        self.incomplete = false;
    }

    fn method_again(&mut self, named: NamedId, name: Ident, first: Span) {
        if !self.reported.insert((name.span, Code::DuplicateMethod)) {
            return;
        }
        let method = self.names.resolve(&name.name);
        let message = format!(
            "method {}.{method} already declared",
            self.names.resolve(&self.types.name_of(named))
        );
        let note = format!("other declaration of method {method}");
        let diagnostic = Diagnostic::new(Code::DuplicateMethod, name.span, message);
        self.sink
            .push(diagnostic.with_note(Code::NoteOtherDeclaration, Some(first), note));
    }

    /// `(*T[P])`: the base type name and what the brackets name. One star
    /// belongs to the receiver; more are part of what stands for the
    /// base.
    fn receiver_parts(tree: &File, receiver: ExprId) -> Receiver {
        let unparen = |mut expr: ExprId| {
            while let ExprKind::Paren(x) = &tree.exprs[expr].kind {
                expr = *x;
            }
            expr
        };
        let mut base = unparen(receiver);
        let mut pointer = false;
        if let ExprKind::Unary {
            op: UnaryOp::Star,
            x,
        } = &tree.exprs[base].kind
        {
            (base, pointer) = (unparen(*x), true);
        }
        let named = base;
        let mut indices = Vec::new();
        if let ExprKind::Index { x, indices: listed } = &tree.exprs[base].kind {
            (base, indices) = (*x, listed.clone());
        }
        Receiver {
            pointer,
            named,
            base,
            indices,
        }
    }

    /// The type parameters that the receiver names, `P` in
    /// `func (l *List[P]) Len() int`: the signature is written with them.
    fn named_by_receiver(
        &mut self,
        cx: &mut Cx<'a>,
        receiver: &Receiver,
        report: bool,
    ) -> Vec<TypeId> {
        let mut params = Vec::new();
        for &index in &receiver.indices {
            let span = cx.tree.exprs[index].span;
            let name = match cx.tree.exprs[index].kind {
                ExprKind::Name(name) => name,
                ExprKind::Bad => self.blank,
                _ => {
                    if report {
                        let message = format!(
                            "receiver type parameter {} must be an identifier",
                            self.expr_string(cx, index)
                        );
                        self.error(Code::InvalidReceiverTypeParams, span, message);
                    }
                    self.blank
                }
            };
            let param = self.types.param(name);
            self.param_spans.insert(param, span);
            params.push(param);
            if let Some(&local) = cx.body.defs.get(&span.lo) {
                cx.params.insert(local, param);
            }
        }
        params
    }

    /// The generic alias with type arguments that the type `ty` is
    /// written as, through other aliases and a pointer.
    fn alias_instance(&self, mut ty: TypeId) -> Option<TypeId> {
        let mut pointer = false;
        loop {
            match (self.types.alias_of(ty), self.types.alias_target(ty)) {
                (Some((_, args)), _) if !args.is_empty() => return Some(ty),
                (_, Some(target)) => ty = target,
                _ => match &*self.types.get(ty) {
                    Type::Pointer(elem) if !pointer => (ty, pointer) = (*elem, true),
                    _ => return None,
                },
            }
        }
    }

    /// The type of this package that the receiver names, through aliases
    /// and at most one pointer, and whether the receiver is a pointer. A
    /// method belongs to that type also where its receiver is in error.
    fn receiver_base(&mut self, cx: &Cx<'a>, receiver: &Receiver) -> Option<(NamedId, bool)> {
        let Some(&Ref::Object(object)) = cx.body.uses.get(&receiver.base) else {
            return None;
        };
        let mut base = self.object_type(object, true);
        let mut pointer = receiver.pointer;
        if let Type::Pointer(elem) = &*self.types.get(base) {
            if pointer {
                return None;
            }
            (base, pointer) = (*elem, true);
        }
        match &*self.types.get(base) {
            Type::Named { id, args } if args.is_empty() && self.declared.contains(id) => {
                Some((*id, pointer))
            }
            _ => None,
        }
    }

    /// Reports what is wrong with the receiver type `receiver`, which
    /// names `named` type parameters.
    fn check_receiver(&mut self, cx: &Cx<'a>, receiver: ExprId, parts: &Receiver, named: usize) {
        let base_span = cx.tree.exprs[parts.base].span;
        let base = match cx.body.uses.get(&parts.base) {
            Some(&Ref::Object(object)) => Some((object, self.object_type(object, true))),
            _ => None,
        };
        if let Some((_, ty)) = base {
            let message = match &*self.types.get(ty) {
                Type::Named { id, .. } if self.types.is_alias(*id) => Some(format!(
                    "cannot define new methods on generic alias type {}",
                    self.generic_string(ty)
                )),
                _ => self.alias_instance(ty).map(|instance| {
                    let instance = self.type_string(instance);
                    format!("cannot define new methods on instantiated type {instance}")
                }),
            };
            if let Some(message) = message {
                return self.error(Code::InvalidReceiver, base_span, message);
            }
        }
        // The receiver names as many type parameters as its base type
        // has.
        if named != 0
            && let Some((_, ty)) = base
            && let Type::Named { id, .. } = &*self.types.get(ty)
        {
            let declared = self.types.type_params(*id).len();
            if declared != 0 && declared != named {
                let message = format!(
                    "receiver declares {named} type parameter{}, but receiver base type declares \
                     {declared}",
                    if named == 1 { "" } else { "s" }
                );
                // The number of type arguments is wrong for this reason
                // alone.
                let whole = cx.tree.exprs[parts.named].span;
                self.reported.insert((whole, Code::GotNTypeArgs));
                return self.error(Code::InvalidReceiverTypeParams, base_span, message);
            }
        }
        // A message names the base type behind its aliases.
        let written = self.typ(cx, receiver);
        let whole = self.types.unalias(written);
        let base = match &*self.types.get(whole) {
            Type::Pointer(elem) => self.types.unalias(*elem),
            _ => whole,
        };
        if self.types.underlying(base) == Types::INVALID {
            return;
        }
        let shown = self.type_string(base);
        // A type of another package is named as the receiver names it.
        let as_named = match &*self.types.get(whole) {
            Type::Pointer(elem) => *elem,
            _ => written,
        };
        let as_named = self.type_string(as_named);
        let (code, message) = match &*self.types.get(base) {
            Type::Named { args, .. } if named == 0 && !args.is_empty() => {
                let message = format!("cannot define new methods on instantiated type {shown}");
                (Code::InvalidReceiver, message)
            }
            Type::Named { id, .. } if !self.declared.contains(id) => {
                let message = format!("cannot define new methods on non-local type {as_named}");
                (Code::ReceiverMethodPkg, message)
            }
            Type::Basic(_) => {
                let message = format!("cannot define new methods on non-local type {as_named}");
                (Code::ReceiverMethodPkg, message)
            }
            Type::Named { .. } => {
                let cause = match &*self.types.get(self.types.underlying(base)) {
                    _ if self.types.is_unsafe_pointer(base, self.names) => "unsafe.Pointer",
                    Type::Pointer(_) | Type::Interface { .. } => "pointer or interface type",
                    _ => return,
                };
                let message = format!("invalid receiver type {shown} ({cause})");
                (Code::InvalidReceiver, message)
            }
            _ => {
                let message = format!("invalid receiver type {}", self.type_string(written));
                (Code::InvalidReceiver, message)
            }
        };
        self.error(code, base_span, message);
    }

    /// What is being declared needs the method that a selector reaches,
    /// if it is a method of a type of this package.
    pub(super) fn need_method(&mut self, found: &crate::types::Found) {
        let (Member::Method { .. }, Type::Named { id, .. }) =
            (&found.member, &*self.types.get(found.holder))
        else {
            return;
        };
        let key = (*id, found.name, self.package_of(found.name));
        let node = self.method_names.get(&key);
        if let Some(&node) = node.and_then(|span| self.method_nodes.get(span)) {
            self.need(node);
        }
    }

    /// A field and a method of a struct type have different names.
    pub(super) fn fields_and_methods(&mut self) {
        for object in 0..self.decls.len() {
            let Some(Decl {
                unit,
                form: Form::Type {
                    alias: false, ty, ..
                },
                ..
            }) = self.decls[object]
            else {
                continue;
            };
            let Type::Named { id, .. } = &*self.types.get(self.ty[object]) else {
                continue;
            };
            let under = self.types.get(self.types.underlying(self.ty[object]));
            let Type::Struct(fields) = &*under else {
                continue;
            };
            for field in fields.iter() {
                let Some(&method) = self.method_names.get(&(*id, field.name, field.package)) else {
                    continue;
                };
                if !self.reported.insert((method, Code::FieldAndMethod)) {
                    continue;
                }
                let name = self.names.resolve(&field.name);
                let message = format!("field and method with the same name {name}");
                let mut diagnostic = Diagnostic::new(Code::FieldAndMethod, method, message);
                if let Some(span) = self.field_span(unit.file, ty, field.name) {
                    let note = format!("other declaration of {name}");
                    diagnostic = diagnostic.with_note(Code::NoteOtherDeclaration, Some(span), note);
                }
                self.sink.push(diagnostic);
            }
        }
    }

    /// The methods of an interface have different names: those that it
    /// writes, and those of the interfaces that it embeds unless they are
    /// one method. `written` is what the interface type names, in its
    /// order.
    pub(super) fn interface_methods(&mut self, written: &[(Result<Method, TypeId>, Span)]) {
        use crate::types::MethodSet;
        let (types, names) = (self.types, self.names);
        let mut steps = 0;
        // The names that come again, each with the place of what brings
        // it again.
        let mut again: Vec<(Spur, Option<Spur>, Span)> = Vec::new();
        let mut all = MethodSet::default();
        for (elem, span) in written {
            if let Ok(method) = elem {
                let mut both = |m: &Method, _: &Method| again.push((m.name, m.package, *span));
                all = all.with(method.clone(), &mut both, &mut steps);
            }
        }
        for (elem, span) in written {
            if let Err(embedded) = elem
                && let Some(methods) = types.method_set(*embedded)
            {
                let mut both = |m: &Method, other: &Method| {
                    if !types.identical(m.sig, other.sig) {
                        again.push((m.name, m.package, *span));
                    }
                };
                all = all.union(&methods, &mut both, &mut steps);
            }
        }
        self.steps += steps;
        // In the order of the source, whatever the order inside the sets.
        again.sort_by_key(|&(name, _, span)| (span.lo, names.resolve(&name)));
        for (name, package, span) in again {
            if !self.reported.insert((span, Code::DuplicateMethodIface)) {
                continue;
            }
            // The first of the interface that has the name.
            let has = |(elem, _): &&(Result<Method, TypeId>, Span)| match elem {
                Ok(method) => (method.name, method.package) == (name, package),
                Err(embedded) => types
                    .method_set(*embedded)
                    .is_some_and(|methods| methods.get((name, package)).is_some()),
            };
            let explicit = written.iter().filter(|(elem, _)| elem.is_ok());
            let embedded = written.iter().filter(|(elem, _)| elem.is_err());
            let other = explicit.chain(embedded).find(has).map(|&(_, other)| other);
            let name = names.resolve(&name);
            let message = format!("duplicate method {name}");
            let diagnostic = Diagnostic::new(Code::DuplicateMethodIface, span, message);
            let note = format!("other declaration of method {name}");
            self.sink
                .push(diagnostic.with_note(Code::NoteOtherDeclaration, other, note));
        }
    }

    /// A field of a struct type has a name of its own; `declared` has the
    /// fields before it.
    pub(super) fn field_once(
        &mut self,
        declared: &mut FxHashMap<Spur, Span>,
        name: Spur,
        span: Span,
    ) {
        if name == self.blank {
            return;
        }
        let Some(&other) = declared.get(&name) else {
            declared.insert(name, span);
            return;
        };
        if self.reported.insert((span, Code::Redeclared)) {
            let name = self.names.resolve(&name);
            let diagnostic = Diagnostic::new(Code::Redeclared, span, format!("{name} redeclared"));
            let note = format!("other declaration of {name}");
            self.sink
                .push(diagnostic.with_note(Code::NoteOtherDeclaration, Some(other), note));
        }
    }

    /// An embedded field is of a declared type that is no pointer, or of
    /// a pointer to one that is no interface.
    pub(super) fn embedded_field(&mut self, ty: TypeId, span: Span) {
        let (base, pointer) = match &*self.types.get(ty) {
            Type::Pointer(elem) => (*elem, true),
            _ => (ty, false),
        };
        let is_param = matches!(&*self.types.get(base), Type::Param(_));
        let (code, what) = match &*self.types.get(self.types.underlying(base)) {
            _ if is_param => (
                Code::EmbeddedFieldTypeParam,
                "a (pointer to a) type parameter",
            ),
            _ if self.types.is_unsafe_pointer(base, self.names) => {
                (Code::EmbeddedFieldUnsafePointer, "unsafe.Pointer")
            }
            Type::Pointer(_) => (Code::EmbeddedFieldPointer, "a pointer"),
            Type::Interface { .. } if pointer => {
                (Code::EmbeddedFieldPointerIface, "a pointer to an interface")
            }
            _ => return,
        };
        self.error(code, span, format!("embedded field type cannot be {what}"));
    }

    /// Where the struct type that `ty` writes declares its field `name`.
    fn field_span(&self, tree: &File, ty: ExprId, name: Spur) -> Option<Span> {
        let ExprKind::StructType { fields } = &tree.exprs[ty].kind else {
            return None;
        };
        fields.iter().find_map(|field| {
            if let Some(named) = field.names.iter().find(|ident| ident.name == name) {
                return Some(named.span);
            }
            let embedded = field
                .names
                .is_empty()
                .then(|| self.embedded(tree, field.ty))?;
            embedded
                .filter(|&(found, _)| found == name)
                .map(|found| found.1)
        })
    }
}
