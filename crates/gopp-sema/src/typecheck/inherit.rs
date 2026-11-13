//! Types with parents (`@Extends`, TYP-16 to TYP-26): reading the
//! parents, what a hierarchy may be, and what a child may declare next
//! to what it inherits.

use super::*;
use crate::types::{Found, Marks, NamedId, PARENT};

impl<'a> Checker<'a> {
    fn marked(&self, cx: &Cx<'a>, attr: &Attribute, name: &str) -> bool {
        let span = attr.path.name.span;
        attr.path.package.is_none() && &cx.text[span.lo as usize..span.hi as usize] == name
    }

    /// Reads the `@Extends` attributes of the declaration of `named`: its
    /// parents, in their order, written with its type parameters.
    pub(super) fn inherit(&mut self, cx: &Cx<'a>, named: NamedId, attrs: &'a [Attribute]) {
        if cx.dialect != Dialect::Gopp {
            return;
        }
        let mut parents = Vec::new();
        for attr in attrs {
            if !self.marked(cx, attr, "Extends") {
                continue;
            }
            let Some([parent]) = attr.args.as_deref() else {
                continue;
            };
            let ty = self.typ(cx, *parent);
            if self.types.underlying(ty) == Types::INVALID {
                continue;
            }
            // A parent is a declared struct type.
            let is_struct = matches!(&*self.types.get(ty), Type::Named { .. })
                && matches!(&*self.types.get(self.types.underlying(ty)), Type::Struct(_));
            if !is_struct {
                let message = format!(
                    "invalid parent {}: a parent is a struct type",
                    self.type_string(ty)
                );
                self.error(
                    Code::ExtendsParentKind,
                    cx.tree.exprs[*parent].span,
                    message,
                );
                continue;
            }
            parents.push(ty);
        }
        if !parents.is_empty() {
            self.types.set_parents(named, parents);
        }
        if attrs.iter().any(|attr| self.marked(cx, attr, "Abstract")) {
            self.types.set_abstract(named);
        }
    }

    /// Checks the hierarchies of the declared types of the package, once
    /// every type has its parents and its methods (TYP-16 to TYP-19).
    pub(super) fn hierarchies(&mut self, units: &[Unit<'a>]) {
        self.types.forget_hierarchy();
        let mut types: Vec<(TypeId, Span, &'a [Attribute])> = Vec::new();
        for object in 0..self.decls.len() {
            if let Some(Decl {
                form:
                    Form::Type {
                        alias: false,
                        attrs,
                        ..
                    },
                unit,
                ..
            }) = self.decls[object]
                && unit.source.dialect() == Dialect::Gopp
            {
                types.push((self.ty[object], self.package.objects[object].span, attrs));
            }
        }
        // A type that is its own ancestor has no parents for what
        // follows.
        let all: Vec<TypeId> = types.iter().map(|&(ty, ..)| ty).collect();
        let cyclic = self.on_parent_cycles(&all);
        for &(ty, span, _) in &types {
            let Type::Named { id, .. } = &*self.types.get(ty) else {
                continue;
            };
            if !cyclic.contains(id) {
                continue;
            }
            let message = format!(
                "invalid parents: {} is its own ancestor",
                self.type_string(ty)
            );
            self.error(Code::ExtendsCycle, span, message);
            self.types.set_parents(*id, Vec::new());
        }
        for &(ty, span, attrs) in &types {
            let on_cycle =
                matches!(&*self.types.get(ty), Type::Named { id, .. } if cyclic.contains(id));
            if !on_cycle {
                self.hierarchy(ty, span, attrs);
            }
        }
        for unit in units
            .iter()
            .filter(|unit| unit.source.dialect() == Dialect::Gopp)
        {
            self.overrides(*unit);
        }
    }

    /// The declared types among `types` that are their own ancestors:
    /// one walk over the parents of all of them.
    fn on_parent_cycles(&mut self, types: &[TypeId]) -> FxHashSet<NamedId> {
        let named = |this: &Self, ty: TypeId| match &*this.types.get(ty) {
            Type::Named { id, .. } => Some(*id),
            _ => None,
        };
        // A type is open while its parents are walked.
        let mut open: FxHashSet<NamedId> = FxHashSet::default();
        let mut done: FxHashSet<NamedId> = FxHashSet::default();
        let mut cyclic = FxHashSet::default();
        for &start in types {
            let mut stack: Vec<(TypeId, usize)> = vec![(start, 0)];
            while let Some(&(ty, at)) = stack.last() {
                self.steps += 1;
                let Some(id) = named(self, ty) else {
                    stack.pop();
                    continue;
                };
                if at == 0 {
                    if done.contains(&id) {
                        stack.pop();
                        continue;
                    }
                    open.insert(id);
                }
                let parents = self.types.parents(ty);
                let Some(&parent) = parents.get(at) else {
                    open.remove(&id);
                    done.insert(id);
                    stack.pop();
                    continue;
                };
                if let Some(last) = stack.last_mut() {
                    last.1 += 1;
                }
                match named(self, parent) {
                    // Every type from the parent to this one is on the
                    // cycle.
                    Some(parent_id) if open.contains(&parent_id) => {
                        let from = stack
                            .iter()
                            .rposition(|&(on, _)| named(self, on) == Some(parent_id));
                        for &(on, _) in &stack[from.unwrap_or(0)..] {
                            cyclic.extend(named(self, on));
                        }
                    }
                    Some(_) => stack.push((parent, 0)),
                    None => {}
                }
            }
        }
        cyclic
    }

    /// One type: no ancestor twice, a type defined from another has no
    /// hierarchy to copy, `@Abstract` goes with parents, no field has
    /// the name of an inherited member, two parents share no name, and
    /// no `@Abstract` method is left open by a type that is not marked.
    fn hierarchy(&mut self, ty: TypeId, span: Span, attrs: &[Attribute]) {
        let Type::Named { id, .. } = &*self.types.get(ty) else {
            return;
        };
        let shown = self.type_string(ty);
        let parents = self.types.parents(ty);
        // `type X Y`.
        if let Some(from) = self.types.declared_as(*id)
            && matches!(&*self.types.get(from), Type::Named { .. })
            && (self.types.extends(from)
                || self.types.has_table(from)
                || self.types.is_abstract(from, self.names))
        {
            let from = self.type_string(from);
            let message = format!(
                "cannot define {shown} from {from}: {from} has parents or methods that may be \
                 replaced"
            );
            let fix = format!("write @Extends({from}) before type {shown} struct{{}}");
            let fix = (Code::NoteOwnFixOption, fix);
            self.error_with_fix(Code::DefinedFromHierarchy, span, message, fix);
        }
        let abstract_marked = attrs.iter().any(|attr| {
            attr.path.package.is_none() && self.names.resolve(&attr.path.name.name) == "Abstract"
        });
        if parents.is_empty() {
            if abstract_marked && !self.types.is_interface(ty) {
                let message = format!(
                    "cannot use @Abstract on {shown}: it is written on a type with parents, or \
                     on a method"
                );
                self.error(Code::AbstractAttrPlacement, span, message);
            }
            return;
        }
        if self.types.is_interface(ty) {
            return;
        }
        // Every ancestor once: a declared type is in the hierarchy once,
        // whatever its type arguments. One parent whose own hierarchy is
        // right brings no ancestor twice.
        let mut seen: FxHashMap<NamedId, TypeId> = FxHashMap::default();
        let mut rest: Vec<TypeId> = match parents.len() {
            1 => Vec::new(),
            _ => parents.iter().rev().copied().collect(),
        };
        while let Some(next) = rest.pop() {
            self.steps += 1;
            let Type::Named { id: ancestor, .. } = &*self.types.get(next) else {
                continue;
            };
            if let Some(&first) = seen.get(ancestor) {
                let message = format!(
                    "invalid parents of {shown}: {} is an ancestor twice",
                    self.type_string(first)
                );
                self.error(Code::ExtendsDiamond, span, message);
                return;
            }
            seen.insert(*ancestor, next);
            rest.extend(self.types.parents(next).into_iter().rev());
        }
        // The fields of the type against what it inherits.
        if let Type::Struct(fields) = &*self.types.get(self.types.underlying(ty)) {
            for field in fields.iter() {
                let inherited = parents.iter().find_map(|&parent| {
                    let found = self
                        .types
                        .lookup(parent, field.name, self.path, self.names, false);
                    found.ok().map(|found| found.holder)
                });
                if let Some(holder) = inherited {
                    let name = self.names.resolve(&field.name);
                    let message = format!(
                        "field {name} of {shown} has the name of a member of its ancestor {}",
                        self.type_string(holder)
                    );
                    self.error(Code::ExtendsNameClash, span, message);
                }
            }
        }
        self.parents_clash(ty, span, &parents);
        // What is left open makes the type abstract, which it says.
        if !self.types.is_abstract_marked(ty) {
            let open = self.types.open_abstracts(ty);
            if let Some((holder, method)) = open.first() {
                let message = format!(
                    "{shown} does not override the @Abstract method {} of {}",
                    self.names.resolve(&method.name),
                    self.type_string(*holder)
                );
                let fix = "declare the method with @Override, or mark the type @Abstract";
                let fix = (Code::NoteOwnFixOption, fix.to_owned());
                self.error_with_fix(Code::AbstractUnimplemented, span, message, fix);
            }
        }
    }

    /// Two parents of `ty` have a member of one name: a field, or
    /// methods of which one is final or which differ; methods that may
    /// all be replaced and are alike are replaced by one `@Override`
    /// method of the type (TYP-17).
    fn parents_clash(&mut self, ty: TypeId, span: Span, parents: &[TypeId]) {
        if parents.len() < 2 {
            return;
        }
        let shown = self.type_string(ty);
        let mut first: FxHashMap<(Spur, std::option::Option<Spur>), usize> = FxHashMap::default();
        for (index, &parent) in parents.iter().enumerate() {
            let mut holders = vec![parent];
            holders.extend(self.types.ancestors(parent));
            for holder in holders {
                for key in self.types.member_names(holder) {
                    self.steps += 1;
                    let Some(&other) = first.get(&key).filter(|&&other| other != index) else {
                        first.insert(key, index);
                        continue;
                    };
                    if self.replaced_together(ty, key, parents[other], parent)
                        || self.operators_combine(key.0, parents[other], parent)
                    {
                        continue;
                    }
                    let message = format!(
                        "the parents {} and {} of {shown} both have a member {}",
                        self.type_string(parents[other]),
                        self.type_string(parent),
                        self.names.resolve(&key.0)
                    );
                    self.error(Code::ExtendsNameClash, span, message);
                }
            }
        }
    }

    /// Whether the `@Operator` method of `ty` that is declared at `name`
    /// replaces none of the methods of its name that the parents have:
    /// none of those takes its parameters.
    fn operator_added(&self, ty: TypeId, name: Ident, parents: &[TypeId]) -> bool {
        let Type::Named { id, .. } = &*self.types.get(ty) else {
            return false;
        };
        let own = self.types.operators(ty, name.name);
        let declared = own.iter().find(|operator| {
            operator.holder == ty
                && self.operator_spans.get(&(*id, operator.at)) == Some(&name.span)
        });
        // One that was refused replaces nothing.
        let Some(declared) = declared else {
            return true;
        };
        !parents.iter().any(|&parent| {
            let inherited = self.types.operators(parent, name.name);
            inherited
                .iter()
                .any(|other| self.types.same_params(other.sig, declared.sig))
        })
    }

    /// Whether the two parents both have `@Operator` methods of the
    /// name, no two for the same parameters: a child has them all.
    fn operators_combine(&self, name: Spur, a: TypeId, b: TypeId) -> bool {
        let (of_a, of_b) = (self.types.operators(a, name), self.types.operators(b, name));
        !of_a.is_empty()
            && !of_b.is_empty()
            && !of_a.iter().any(|one| {
                of_b.iter()
                    .any(|other| self.types.same_params(one.sig, other.sig))
            })
    }

    /// Whether the member `key` that the parents `a` and `b` of `ty` both
    /// have is a method that may be replaced in both, with one signature,
    /// and `ty` replaces it or leaves it abstract.
    fn replaced_together(
        &self,
        ty: TypeId,
        key: (Spur, std::option::Option<Spur>),
        a: TypeId,
        b: TypeId,
    ) -> bool {
        let version = |parent: TypeId| {
            let found = self
                .types
                .lookup(parent, key.0, self.path, self.names, false)
                .ok()?;
            let Member::Method { sig, .. } = found.member else {
                return None;
            };
            Some((sig, self.types.method_marks(found.holder, key.0, key.1)))
        };
        let (Some((sig_a, marks_a)), Some((sig_b, marks_b))) = (version(a), version(b)) else {
            return false;
        };
        let open = |marks: Marks| {
            marks.has(Marks::VIRTUAL) || marks.has(Marks::ABSTRACT) || marks.has(Marks::OVERRIDE)
        };
        if !open(marks_a) || !open(marks_b) || !self.types.identical(sig_a, sig_b) {
            return false;
        }
        let own = self.types.method_marks(ty, key.0, key.1);
        let both_abstract = marks_a.has(Marks::ABSTRACT) && marks_b.has(Marks::ABSTRACT);
        own.has(Marks::OVERRIDE) || both_abstract && self.types.is_abstract_marked(ty)
    }

    /// The methods of one file against the methods that their types
    /// inherit (TYP-18): one that has the name of an inherited method
    /// replaces it, which it says with `@Override`, and only a method
    /// that may be replaced, with the same signature.
    fn overrides(&mut self, unit: Unit<'a>) {
        let tree = unit.file;
        for &decl in &tree.decls {
            let node = &tree.decl_nodes[decl];
            let DeclKind::Func {
                receiver: Some(receiver),
                name,
                body,
                ..
            } = &node.kind
            else {
                continue;
            };
            let text = unit.source.text();
            let marks = node
                .attrs
                .iter()
                .filter(|attr| attr.path.package.is_none())
                .filter_map(|attr| {
                    let span = attr.path.name.span;
                    Marks::of(&text[span.lo as usize..span.hi as usize])
                })
                .fold(Marks::default(), Marks::with);
            if marks.has(Marks::ABSTRACT) && body.is_some() {
                let message = format!(
                    "the @Abstract method {} has a body: a child gives it one",
                    self.names.resolve(&name.name)
                );
                self.error(Code::AbstractBody, name.span, message);
            }
            let Some(ty) = self.receiver_named(unit, receiver.ty) else {
                continue;
            };
            // A generic type, as its declaration writes it.
            let ty = match &*self.types.get(ty) {
                Type::Named { id, args } if args.is_empty() => {
                    let params = self.types.type_params(*id);
                    match params.is_empty() {
                        true => ty,
                        false => self.types.named(*id, &params),
                    }
                }
                _ => ty,
            };
            let by_value = !matches!(
                tree.exprs[receiver.ty].kind,
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    ..
                }
            );
            // A mark on a value receiver is said where the mark is.
            let unmarked = !(marks.has(Marks::ABSTRACT)
                || marks.has(Marks::VIRTUAL)
                || marks.has(Marks::OVERRIDE));
            if by_value && unmarked && self.types.is_abstract(ty, self.names) {
                let message = format!(
                    "cannot declare {} on a value of {}: the type is abstract and has no values",
                    self.names.resolve(&name.name),
                    self.type_string(ty)
                );
                self.error(Code::AbstractInstance, name.span, message);
            }
            let parents = self.types.parents(ty);
            // An `@Operator` method that takes other parameters than the
            // inherited ones of its name stands next to them (EXP-30).
            if marks.has(Marks::OPERATOR) && self.operator_added(ty, *name, &parents) {
                continue;
            }
            let package = self.package_of(name.name);
            let method = self.names.resolve(&name.name).to_owned();
            let shown = self.type_string(ty);
            // The versions that the parents have.
            let mut versions = Vec::new();
            for &parent in &parents {
                self.steps += 1;
                let found = self
                    .types
                    .lookup(parent, name.name, self.path, self.names, false);
                if let Ok(found) = found {
                    versions.push(found);
                }
            }
            if versions.is_empty() {
                if marks.has(Marks::OVERRIDE) {
                    let message = format!(
                        "{shown}.{method} is marked @Override, and no ancestor of {shown} has a \
                         method {method}"
                    );
                    self.error(Code::OverrideNothing, name.span, message);
                }
                continue;
            }
            let own = self
                .types
                .lookup(ty, name.name, self.path, self.names, false)
                .ok();
            for version in versions {
                let holder = self.type_string(version.holder);
                let Member::Method { sig, .. } = version.member else {
                    let message = format!(
                        "method {method} of {shown} has the name of a field of its ancestor \
                         {holder}"
                    );
                    self.error(Code::ExtendsNameClash, name.span, message);
                    continue;
                };
                let theirs = self.types.method_marks(version.holder, name.name, package);
                let open = theirs.has(Marks::VIRTUAL)
                    || theirs.has(Marks::ABSTRACT)
                    || theirs.has(Marks::OVERRIDE);
                if !open {
                    let message = format!(
                        "cannot replace {holder}.{method}: it is final, because it is not marked \
                         @Virtual"
                    );
                    self.error(Code::OverrideFinal, name.span, message);
                } else if !marks.has(Marks::OVERRIDE) {
                    let message =
                        format!("{shown}.{method} replaces {holder}.{method}: mark it @Override");
                    self.error(Code::OverrideMissing, name.span, message);
                } else if let Some(Member::Method { sig: ours, .. }) =
                    own.as_ref().map(|own| own.member)
                    && !self.types.identical(ours, sig)
                {
                    let message = format!(
                        "{shown}.{method} does not have the signature of {holder}.{method}"
                    );
                    let mut diagnostic =
                        Diagnostic::new(Code::OverrideSignature, name.span, message);
                    diagnostic = diagnostic
                        .with_note(
                            Code::NoteHave,
                            None,
                            format!("have {}", self.type_string(ours)),
                        )
                        .with_note(
                            Code::NoteWant,
                            None,
                            format!("want {}", self.type_string(sig)),
                        );
                    if self.report_once((name.span, Code::OverrideSignature)) {
                        self.emit(diagnostic);
                    }
                }
            }
        }
    }

    /// The declared type that the receiver type expression `ty` names.
    pub(super) fn receiver_named(
        &mut self,
        unit: Unit<'a>,
        mut ty: ExprId,
    ) -> std::option::Option<TypeId> {
        let tree = unit.file;
        loop {
            ty = match &tree.exprs[ty].kind {
                ExprKind::Paren(x) => *x,
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => *x,
                ExprKind::Index { x, .. } => *x,
                ExprKind::Name(_) => break,
                _ => return None,
            };
        }
        let ExprKind::Name(name) = tree.exprs[ty].kind else {
            return None;
        };
        let objects = &self.package.objects;
        let object = objects
            .iter()
            .position(|object| object.name == name && matches!(object.kind, ObjKind::Type))?;
        Some(self.ty[object])
    }

    /// Whether a value of the type `from` is given where one of the type
    /// `to` is taken by an upcast: a pointer or a handle of a child where
    /// one of an ancestor is wanted. `Some(false)` if the ancestor has no
    /// type table, so that there is no such conversion (EXP-13).
    pub(super) fn upcast(&self, from: TypeId, to: TypeId) -> std::option::Option<bool> {
        let (child, parent) = match (
            &*self.types.get(self.types.underlying(from)),
            &*self.types.get(self.types.underlying(to)),
        ) {
            (Type::Pointer(child), Type::Pointer(parent))
            | (Type::Unique(child), Type::Unique(parent))
            | (Type::Shared(child), Type::Shared(parent))
            | (Type::Weak(child), Type::Weak(parent)) => (*child, *parent),
            _ => return None,
        };
        let path = self.types.ancestor_path(child, parent)?;
        (!path.is_empty()).then(|| self.types.has_table(parent))
    }

    /// Why the value `x` does not fit a place of the type `to`, if what
    /// is in the way is a rule of hierarchies: no upcast to a parent
    /// without a type table, and no value of a child as a value of its
    /// parent (EXP-13, EXP-15). Reported; whether it was.
    pub(super) fn hierarchy_unfit(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &Operand,
        to: TypeId,
    ) -> bool {
        let span = cx.tree.exprs[expr].span;
        let shown = self.describe(cx, expr, x);
        let target = self.type_string(to);
        let pointer = |ty: TypeId| match &*self.types.get(self.types.underlying(ty)) {
            Type::Pointer(elem) | Type::Unique(elem) | Type::Shared(elem) | Type::Weak(elem) => {
                *elem
            }
            _ => ty,
        };
        let (child, parent) = (pointer(x.ty), pointer(to));
        let is_ancestor = self
            .types
            .ancestor_path(child, parent)
            .is_some_and(|path| !path.is_empty());
        if !is_ancestor {
            return false;
        }
        if child == x.ty || parent == to {
            let message = format!(
                "cannot use {shown} as {target} value: a {} is more than its parent {}",
                self.type_string(child),
                self.type_string(parent)
            );
            let fix = (Code::NoteOwnFixAddress, "use a pointer".to_owned());
            self.error_with_fix(Code::PartialValue, span, message, fix);
            return true;
        }
        if self.upcast(x.ty, to) == Some(false) || !self.types.has_table(parent) {
            let parent = self.type_string(parent);
            let message = format!(
                "cannot use {shown} as {target} value: {parent} has no type table, so nothing \
                 says what a {target} points to"
            );
            let fix = format!("add a @Virtual method to {parent}, or use the pointer to the child");
            self.error_with_fix(
                Code::UpcastUntagged,
                span,
                message,
                (Code::NoteOwnFixOption, fix),
            );
            return true;
        }
        false
    }

    /// The struct literal `expr` of the type `base`, which has parents:
    /// its elements name fields, those of ancestors too, or a parent part
    /// by the name of the parent's type; no field is set twice, and every
    /// field that must be set is (TYP-17).
    pub(super) fn inherited_literal(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        base: TypeId,
        elems: &[ExprId],
    ) {
        let tree = cx.tree;
        let shown = self.type_string(base);
        let keyed =
            |element: &ExprId| matches!(tree.exprs[*element].kind, ExprKind::KeyValue { .. });
        if let Some(&first) = elems.iter().find(|element| !keyed(element)) {
            for &element in elems {
                self.operand(cx, element);
            }
            let message = format!("a literal of {shown} names its fields: the type has parents");
            return self.error(
                Code::ExtendsLiteralPositional,
                tree.exprs[first].span,
                message,
            );
        }
        let mut ancestors = vec![base];
        ancestors.extend(self.types.ancestors(base));
        // The fields that are set, each by the type that has it; and the
        // parent parts that are set as a whole.
        let mut set: FxHashSet<(TypeId, Spur)> = FxHashSet::default();
        let mut parts: Vec<TypeId> = Vec::new();
        for &element in elems {
            let ExprKind::KeyValue { key, value } = tree.exprs[element].kind else {
                continue;
            };
            let ExprKind::Name(name) = tree.exprs[key].kind else {
                self.operand(cx, value);
                let message = format!(
                    "invalid field name {} in struct literal",
                    self.expr_string(cx, key)
                );
                self.error(Code::LiteralInvalidFieldName, tree.exprs[key].span, message);
                continue;
            };
            let text = self.names.resolve(&name);
            let found = self
                .types
                .lookup(base, name, self.path, self.names, false)
                .ok();
            let twice = |this: &mut Self| {
                let message = format!("{text} is set twice in the literal of {shown}");
                this.error(Code::ExtendsLiteralTwice, tree.exprs[key].span, message);
            };
            if let Some(Found {
                member: Member::Field { ty, .. },
                holder,
                ..
            }) = found
            {
                self.assign_literal_field(cx, value, ty);
                let inside = parts
                    .iter()
                    .any(|&part| self.types.ancestor_path(part, holder).is_some());
                if !set.insert((holder, name)) || inside {
                    twice(self);
                }
                continue;
            }
            // A parent part, by the name of the type of an ancestor.
            let part =
                ancestors[1..]
                    .iter()
                    .copied()
                    .find(|&ancestor| match &*self.types.get(ancestor) {
                        Type::Named { id, .. } => self.types.name_of(*id) == name,
                        _ => false,
                    });
            let Some(part) = part else {
                self.operand(cx, value);
                let message = format!("unknown field {text} in struct literal of type {shown}");
                self.error(Code::LiteralUnknownField, tree.exprs[key].span, message);
                continue;
            };
            let outer = std::mem::replace(&mut self.parent_part, true);
            self.assign_literal_field(cx, value, part);
            self.parent_part = outer;
            let overlaps = parts.iter().any(|&other| {
                self.types.ancestor_path(other, part).is_some()
                    || self.types.ancestor_path(part, other).is_some()
            }) || set
                .iter()
                .any(|&(holder, _)| self.types.ancestor_path(part, holder).is_some());
            if overlaps {
                twice(self);
            }
            parts.push(part);
        }
        // What must be set and is not.
        let mut missing = Vec::new();
        for &holder in &ancestors {
            if parts
                .iter()
                .any(|&part| self.types.ancestor_path(part, holder).is_some())
            {
                continue;
            }
            let Type::Struct(fields) = &*self.types.get(self.types.underlying(holder)) else {
                continue;
            };
            for field in fields
                .iter()
                .filter(|field| !set.contains(&(holder, field.name)))
            {
                let name = self.names.resolve(&field.name);
                if field.required {
                    missing.push(format!("{name} (@Required)"));
                } else if self.zero_lacking(field.ty).is_some() {
                    missing.push(format!(
                        "{name} ({} has no zero value)",
                        self.type_string(field.ty)
                    ));
                }
            }
        }
        if !missing.is_empty() {
            let message = format!(
                "missing {} in struct literal of type {shown}",
                missing.join(", ")
            );
            self.error(Code::RequiredMissing, tree.exprs[expr].span, message);
        }
    }

    /// Whether the pointer or the handle `x` points to a declared type
    /// with a type table; `None` if it is no such pointer.
    pub(super) fn pointee_with_table(&self, cx: &Cx<'a>, x: &Operand) -> std::option::Option<bool> {
        if !self.own_rules(cx) {
            return None;
        }
        let pointee = match &*self.types.get(self.types.underlying(x.ty)) {
            Type::Pointer(elem) | Type::Unique(elem) | Type::Shared(elem) => *elem,
            _ => return None,
        };
        let declared = matches!(
            &*self.types.get(pointee),
            Type::Named { .. } | Type::Param(_)
        );
        declared.then(|| self.types.has_table(pointee))
    }

    /// The pointer `x` is asserted to be of the type `ty` at `at`: a
    /// pointer to a child of what it points to (EXP-15).
    pub(super) fn downcast(&mut self, cx: &Cx<'a>, at: ExprId, x: &Operand, ty: TypeId) {
        let pointee = |ty: TypeId| match &*self.types.get(self.types.underlying(ty)) {
            Type::Pointer(elem) | Type::Unique(elem) | Type::Shared(elem) => Some(*elem),
            _ => None,
        };
        let Some(parent) = pointee(x.ty) else {
            return;
        };
        let child = match &*self.types.get(self.types.underlying(ty)) {
            Type::Pointer(elem) => Some(*elem),
            _ => None,
        };
        let fits = child.is_some_and(|child| self.types.ancestor_path(child, parent).is_some());
        if !fits {
            let message = format!(
                "impossible type assertion: {} is no pointer to {} or to a child of it",
                self.type_string(ty),
                self.type_string(parent)
            );
            self.error(Code::DowncastImpossible, cx.tree.exprs[at].span, message);
        }
    }

    /// A literal of an abstract type is no value, unless it is the part
    /// of a child that a literal of the child sets (TYP-19).
    pub(super) fn literal_of_abstract(&mut self, cx: &Cx<'a>, expr: ExprId, base: TypeId) {
        if !self.own_rules(cx) || self.parent_part || !self.types.is_abstract(base, self.names) {
            return;
        }
        let message = format!(
            "cannot make a value of {}: the type is abstract",
            self.type_string(base)
        );
        self.error(Code::AbstractInstance, cx.tree.exprs[expr].span, message);
    }

    /// A type that is written where a value is held is not abstract: an
    /// abstract type has no values of its own (TYP-19). The element type
    /// of an array, a slice, a map, a channel or an option is written
    /// where a value is held too.
    pub(super) fn held_abstract(&mut self, ty: TypeId, span: Span, in_body: bool) {
        match &*self.types.get(ty) {
            // A declaration that holds its type parameter by value takes
            // no abstract type for it.
            Type::Param(_) if !in_body => self.types.set_held(ty),
            Type::Named { .. }
                if self.types.is_interface(ty) && !self.types.parents(ty).is_empty() =>
            {
                let message = format!(
                    "cannot use {} as the type of a value: an interface with @Extends is a \
                     constraint",
                    self.type_string(ty)
                );
                self.error(Code::ExtendsConstraint, span, message);
            }
            Type::Named { .. } if self.types.is_abstract(ty, self.names) => {
                let message = format!(
                    "cannot hold a value of {}: the type is abstract; hold a pointer to a child",
                    self.type_string(ty)
                );
                self.error(Code::AbstractInstance, span, message);
            }
            _ => {}
        }
    }

    /// The name `expr` denotes nothing: `base` in a method is no value,
    /// and names nothing in a type without parents (TYP-20).
    pub(super) fn base_alone(&mut self, cx: &Cx<'a>, expr: ExprId) {
        let is_base = matches!(cx.tree.exprs[expr].kind, ExprKind::Name(name) if self.names.resolve(&name) == "base");
        let Some(receiver) = self.receiver_of.filter(|_| is_base && self.own_rules(cx)) else {
            return;
        };
        let span = cx.tree.exprs[expr].span;
        match self.types.parents(receiver).is_empty() {
            true => {
                let message = format!(
                    "undefined: base ({} has no parents)",
                    self.type_string(receiver)
                );
                self.error(Code::Undefined, span, message);
            }
            false => {
                let message = "base is no value: it selects a member of a parent, as in base.M()";
                self.error(Code::BaseValue, span, message.to_owned());
            }
        }
    }

    /// In a method of a type with parents no local is named `base`
    /// (TYP-20).
    pub(super) fn base_declared(&mut self, cx: &Cx<'a>) {
        let Some(receiver) = self.receiver_of else {
            return;
        };
        if !self.own_rules(cx) || self.types.parents(receiver).is_empty() {
            return;
        }
        for local in &cx.body.locals {
            if self.names.resolve(&local.name) == "base" {
                let message = "base names the parents of the receiver here: use another name";
                self.error(Code::BaseReserved, local.span, message.to_owned());
            }
        }
    }

    /// `base.sel` or `base[Parent].sel` in a method of a type with
    /// parents: the member of a direct parent, as it is declared there
    /// (TYP-20). `None` if the selector is no such thing.
    pub(super) fn base_member(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: ExprId,
        sel: Ident,
    ) -> std::option::Option<Operand> {
        let tree = cx.tree;
        let mut of = self.unparen(cx, x);
        let mut wanted = None;
        if let ExprKind::Index { x: inner, indices } = &tree.exprs[of].kind
            && let [parent] = indices[..]
        {
            wanted = Some(parent);
            of = self.unparen(cx, *inner);
        }
        let is_base = matches!(tree.exprs[of].kind, ExprKind::Name(name) if self.names.resolve(&name) == "base")
            && !cx.body.uses.contains_key(&of);
        let receiver = self.receiver_of.filter(|_| is_base && self.own_rules(cx))?;
        let mut parents = self.types.parents(receiver);
        // In the body the type parameters of the type have other names.
        if let Type::Named { id, .. } = &*self.types.get(receiver) {
            let declared = self.types.type_params(*id);
            if let Some(named) = self.body_params.get(..declared.len()) {
                for parent in &mut parents {
                    *parent = self.types.substitute(*parent, &declared, named);
                }
            }
        }
        let span = tree.exprs[x].span;
        let name = self.names.resolve(&sel.name);
        if parents.is_empty() {
            let message = format!(
                "undefined: base ({} has no parents)",
                self.type_string(receiver)
            );
            self.error(Code::Undefined, span, message);
            return Some(Operand::UNKNOWN);
        }
        // The parents that are meant: the one that is named, or all.
        let chosen: Vec<(usize, TypeId)> = match wanted {
            Some(parent) => {
                let ty = self.typ(cx, parent);
                let at = parents
                    .iter()
                    .position(|&known| self.types.identical(known, ty));
                let Some(at) = at else {
                    if ty != Types::INVALID {
                        let message = format!(
                            "{} is no parent of {}",
                            self.type_string(ty),
                            self.type_string(receiver)
                        );
                        self.error(Code::BaseAmbiguous, tree.exprs[parent].span, message);
                    }
                    return Some(Operand::UNKNOWN);
                };
                vec![(at, parents[at])]
            }
            None => parents.iter().copied().enumerate().collect(),
        };
        let mut found: Vec<(usize, Found)> = Vec::new();
        for (at, parent) in chosen {
            if let Ok(member) = self
                .types
                .lookup(parent, sel.name, self.path, self.names, false)
            {
                found.push((at, member));
            }
        }
        let (at, member) = match found.len() {
            0 => {
                let message = format!(
                    "base.{name} undefined (no parent of {} has {name})",
                    self.type_string(receiver)
                );
                self.error(Code::UndefinedFieldOrMethod, sel.span, message);
                return Some(Operand::UNKNOWN);
            }
            1 => found.remove(0),
            _ => {
                let all: Vec<String> = found
                    .iter()
                    .map(|(at, _)| self.type_string(parents[*at]))
                    .collect();
                let message = format!(
                    "base.{name} is ambiguous: the parents {} have it",
                    all.join(" and ")
                );
                let fix = (
                    Code::NoteOwnFixOption,
                    format!("write base[{}].{name}", all[0]),
                );
                self.error_with_fix(Code::BaseAmbiguous, sel.span, message, fix);
                return Some(Operand::UNKNOWN);
            }
        };
        let mut path = vec![PARENT | at as u32];
        path.extend(&member.path);
        let (kind, operand) = match member.member {
            Member::Field { ty, .. } => (SelectionKind::BaseField, Operand::variable(ty)),
            Member::Method { sig, .. } => {
                let package = self.package_of(sel.name);
                let marks = self.types.method_marks(member.holder, sel.name, package);
                if marks.has(Marks::ABSTRACT) {
                    let message = format!(
                        "cannot call base.{name}: {}.{name} is @Abstract and has no body",
                        self.type_string(member.holder)
                    );
                    self.error(Code::BaseAbstract, sel.span, message);
                }
                (SelectionKind::BaseMethod, Operand::value(sig))
            }
        };
        if self.memo.is_some() {
            let selection = Selection {
                kind,
                recv: receiver,
                holder: member.holder,
                path,
                indirect: true,
            };
            self.selections.insert(expr, selection);
        }
        Some(operand)
    }

    fn assign_literal_field(&mut self, cx: &Cx<'a>, value: ExprId, to: TypeId) {
        self.assign_element(cx, value, to, "struct literal");
    }
}
