//! What fits a place in a `.gopp` file, where a pointer is used like the
//! variable it points to (OWN-P5), a `Unique` or `Shared` lends what it
//! owns (OWN-P6), a value is its own option (TYP-2), and a pointer, a
//! func and an interface with methods are never nil (OWN-P9).

use super::*;

impl<'a> Checker<'a> {
    /// The types that a value of the type `ty` is read as: its own, what
    /// a pointer points to, a pointer to what a `Unique` or `Shared` owns,
    /// and what an option holds. An option is taken as checked: whether it
    /// was is decided where the flow of the function is known.
    fn read_as(&self, ty: TypeId, out: &mut Vec<TypeId>) {
        if out.contains(&ty) {
            return;
        }
        out.push(ty);
        if self.types.is_unsafe_pointer(ty, self.names) {
            return;
        }
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Pointer(elem) | Type::Option(elem) => self.read_as(*elem, out),
            Type::Unique(elem) | Type::Shared(elem) => {
                out.push(self.types.pointer(*elem));
                self.read_as(*elem, out);
            }
            _ => {}
        }
    }

    /// The operand as it is read where a pointer does not fit: what a
    /// pointer, a `Unique` or a `Shared` points to, and what an option
    /// holds, which is taken as checked. What a `Shared` holds is only
    /// read.
    pub(super) fn read(&self, cx: &Cx<'a>, mut x: Operand) -> Operand {
        if !self.own_rules(cx) || !x.is_value() {
            return x;
        }
        while !self.types.is_unsafe_pointer(x.ty, self.names) {
            x = match &*self.types.get(self.types.underlying(x.ty)) {
                Type::Pointer(elem) | Type::Unique(elem) => Operand::variable(*elem),
                Type::Shared(elem) => Operand::value(*elem),
                Type::Option(elem) => Operand {
                    mode: x.mode,
                    ty: *elem,
                },
                _ => break,
            };
        }
        x
    }

    /// A method of an option or of a `Weak`, which are no values of what
    /// they hold: `o.IsNil()`, `o.Get()`, `w.Lock()`.
    pub(super) fn handle_method(&self, x: &Operand, name: Spur) -> Option<Operand> {
        let bool = self.types.basic(Basic::Bool);
        let results = match (
            &*self.types.get(self.types.underlying(x.ty)),
            self.names.resolve(&name),
        ) {
            (Type::Option(_), "IsNil") => vec![bool],
            (Type::Option(elem), "Get") => vec![*elem, bool],
            (Type::Weak(elem), "Lock") => {
                let shared = self.types.handle(Type::Shared, *elem).ok()?;
                vec![self.types.option(shared)]
            }
            _ => return None,
        };
        Some(Operand::value(self.types.func(&[], &results, false)))
    }

    /// Whether an operator on a value of the type may be a method of the
    /// type, which is decided with the methods.
    pub(super) fn may_overload(&self, cx: &Cx<'a>, ty: TypeId) -> bool {
        self.own_rules(cx) && self.types.declares_methods(ty)
    }

    /// The types whose values a place of the type `to` takes: its own,
    /// that of what an option holds, and, where an assignment `writes`,
    /// that of what a pointer points to.
    fn takes(&self, to: TypeId, writes: bool) -> Vec<TypeId> {
        let mut out = vec![to];
        let mut current = to;
        loop {
            current = match &*self.types.get(self.types.underlying(current)) {
                Type::Option(elem) => *elem,
                Type::Pointer(elem) if writes && out.len() == 1 => *elem,
                _ => return out,
            };
            out.push(current);
        }
    }

    /// Whether `nil` is a value of a place of the type `to`.
    pub(super) fn takes_nil(&self, to: TypeId, writes: bool) -> bool {
        self.takes(to, writes).into_iter().any(|to| {
            self.types.has_nil(to, Dialect::Gopp)
                || self.types.is_unsafe_pointer(to, self.names)
                || self.types.has_param(to)
        })
    }

    /// Whether the type has no zero value because its values are never
    /// nil; in a struct, also because a field must be set.
    pub(super) fn never_nil(&self, ty: TypeId) -> bool {
        if self.types.is_unsafe_pointer(ty, self.names) {
            return false;
        }
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Pointer(_)
            | Type::Unique(_)
            | Type::Shared(_)
            | Type::Weak(_)
            | Type::Func { .. } => true,
            Type::Interface { .. } => !self.types.has_nil(ty, Dialect::Gopp),
            Type::Struct(fields) => fields.iter().any(|field| field.required),
            _ => false,
        }
    }

    /// The type that a value of no type has in a place of the type `to`.
    pub(super) fn own_target(&self, to: TypeId, writes: bool) -> TypeId {
        self.takes(to, writes).last().copied().unwrap_or(to)
    }

    /// How the operand `x` fits a place of the type `to` in a `.gopp`
    /// file: the type it is read as, and the type that takes it. A
    /// pointer stays a pointer where that fits.
    pub(super) fn fit_own(
        &self,
        x: &Operand,
        to: TypeId,
        writes: bool,
    ) -> Option<(TypeId, TypeId)> {
        let mut read = Vec::new();
        self.read_as(x.ty, &mut read);
        let takes = self.takes(to, writes);
        read.iter().find_map(|&from| {
            let x = Operand {
                mode: x.mode.clone(),
                ty: from,
            };
            let taken = takes.iter().find(|&&to| !self.not_assignable(&x, to));
            taken
                .copied()
                .or(self.types.extends(from).then_some(to))
                .map(|to| (from, to))
        })
    }

    /// Whether the operand `x` surely fits no place of the type `to` in a
    /// `.gopp` file.
    pub(super) fn unfit_own(&self, x: &Operand, to: TypeId, writes: bool) -> bool {
        self.fit_own(x, to, writes).is_none()
    }

    /// Notes whether the assignment of `x` to the place `place` of the
    /// type `to` writes what a pointer points to: `x` does not fit the
    /// pointer itself. Without `x`, the place is counted or computed with,
    /// which is done to what it points to.
    pub(super) fn note_write(
        &mut self,
        cx: &Cx<'a>,
        place: ExprId,
        to: TypeId,
        x: Option<&Operand>,
    ) {
        let is_pointer = matches!(
            &*self.types.get(self.types.underlying(to)),
            Type::Pointer(_)
        ) && !self.types.is_unsafe_pointer(to, self.names);
        if !self.own_rules(cx) || !is_pointer || self.memo.is_none() {
            return;
        }
        let through = match x {
            None => true,
            Some(x) if x.mode == Mode::Unknown => false,
            Some(x) if x.ty == self.types.basic(Basic::UntypedNil) => !self.takes_nil(to, false),
            Some(x) => self.unfit_own(x, to, false),
        };
        if through {
            self.writes_through.insert(place);
        }
    }

    /// Whether the operand `x` surely fits no place of the type `to`;
    /// `writes` says that the place is on the left of `=`.
    pub(super) fn unfit(&self, cx: &Cx<'a>, x: &Operand, to: TypeId, writes: bool) -> bool {
        match self.own_rules(cx) {
            true => self.unfit_own(x, to, writes),
            false => self.not_assignable(x, to),
        }
    }
}
