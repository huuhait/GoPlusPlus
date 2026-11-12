//! What fits a place in a `.gopp` file, where a pointer is used like the
//! variable it points to (OWN-P5), a `Unique` or `Shared` lends what it
//! owns (OWN-P6), a value is its own option (TYP-2), and a pointer, a
//! func and an interface with methods are never nil (OWN-P9).

use super::*;

impl<'a> Checker<'a> {
    /// The types that a value of the type `ty` is read as: its own, what
    /// a pointer points to, a pointer to what a `Unique` or `Shared` owns,
    /// and what an option holds. What an option holds is given only to a
    /// place that takes nil too, which `fit_own` sees to.
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
    /// pointer, a `Unique` or a `Shared` points to. What a `Shared` holds
    /// is only read. An option is read as what it holds only where it is
    /// known to be set, which its name says then (OWN-P10).
    pub(super) fn read(&self, cx: &Cx<'a>, mut x: Operand) -> Operand {
        if !self.own_rules(cx) || !x.is_value() {
            return x;
        }
        while !self.types.is_unsafe_pointer(x.ty, self.names) {
            x = match &*self.types.get(self.types.underlying(x.ty)) {
                Type::Pointer(elem) | Type::Unique(elem) => Operand::variable(*elem),
                Type::Shared(elem) => Operand::value(*elem),
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

    /// Why an option does not fit a place that would take what it holds:
    /// it may be nil.
    pub(super) fn untested_option(
        &self,
        x: &Operand,
        to: TypeId,
    ) -> std::option::Option<super::implements::Cause> {
        let Type::Option(elem) = &*self.types.get(self.types.underlying(x.ty)) else {
            return None;
        };
        let held = Operand {
            mode: x.mode.clone(),
            ty: *elem,
        };
        self.fit_own(&held, to, false)?;
        Some(super::implements::Cause {
            text: "the option may be nil: test it against nil first".to_owned(),
            have_want: None,
        })
    }

    /// The temporary `x` of `expr` is given to a place of the type `to`:
    /// a `Unique` or a `Shared` that ends with its statement lends no
    /// pointer (OWN-P6).
    pub(super) fn lent_from_temporary(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: &Operand,
        to: std::option::Option<TypeId>,
    ) {
        let is_handle = matches!(
            &*self.types.get(self.types.underlying(x.ty)),
            Type::Unique(_) | Type::Shared(_)
        );
        let Some((from, _)) = to
            .filter(|_| is_handle)
            .and_then(|to| self.fit_own(x, to, false))
        else {
            return;
        };
        let lends = matches!(
            &*self.types.get(self.types.underlying(from)),
            Type::Pointer(_)
        );
        if !lends || from == x.ty {
            return;
        }
        let message = format!(
            "cannot use {} as {} value: the {} is a temporary that ends with the statement",
            self.expr_string(cx, expr),
            self.type_string(from),
            self.type_string(x.ty)
        );
        let fix = (
            Code::NoteOwnFixAddress,
            "store it in a variable first".to_owned(),
        );
        self.error_with_fix(Code::OwnRefIntoTemp, cx.tree.exprs[expr].span, message, fix);
    }

    /// Whether a value of the type may be without a zero value: the type
    /// has none, holds a part that has none, or is a type parameter that
    /// may stand for such a type (OWN-R9).
    pub(super) fn may_lack_zero(&self, ty: TypeId) -> bool {
        self.types.no_zero(ty, self.names).is_some()
    }

    /// That the type has no zero value, and why, as a message says it;
    /// `None` if it has one, or if a constraint that would say is in
    /// error.
    pub(super) fn zero_lacking(&self, ty: TypeId) -> Option<String> {
        use crate::types::NoZero;
        let (part, why) = self.types.no_zero(ty, self.names)?;
        if self.open(part) {
            return None;
        }
        let (shown, held) = (self.type_string(ty), self.type_string(part));
        let cause = match why {
            NoZero::Param if part == ty => {
                return Some(format!("{shown} may be a type with no zero value"));
            }
            NoZero::NeverNil => format!("a {held} is never nil"),
            NoZero::Required(field) => format!(
                "the field {} of {held} is @Required",
                self.names.resolve(&field)
            ),
            NoZero::Param => format!("{held} may be a type without one"),
        };
        Some(format!("{shown} has no zero value ({cause})"))
    }

    /// What is done at `span` would make a zero value of the type, which
    /// must have one (ADR-0016). `doing` says what, `fix` what to write.
    pub(super) fn zero_needed(
        &mut self,
        cx: &Cx<'a>,
        span: Span,
        ty: TypeId,
        doing: &str,
        fix: &str,
    ) {
        if !self.own_rules(cx) || self.memo.is_none() {
            return;
        }
        if let Some(lacking) = self.zero_lacking(ty) {
            let message = format!("{doing}: {lacking}");
            let fix = (Code::NoteOwnFixOption, fix.to_owned());
            self.error_with_fix(Code::NoZeroValue, span, message, fix);
        }
    }

    /// Whether the type has nil of its own, so that an option of it says
    /// nothing more (OWN-P9).
    pub(super) fn has_own_nil(&self, ty: TypeId) -> bool {
        self.types.underlying(ty) != Types::INVALID
            && !self.is_param(ty)
            && (self.types.has_nil(ty, Dialect::Gopp)
                || self.types.is_unsafe_pointer(ty, self.names))
    }

    /// `Option[of]` is written at `span`. A declaration that writes it
    /// for its type parameter asks every type argument to have no nil,
    /// which is held where the arguments are given.
    pub(super) fn option_of(&mut self, of: TypeId, span: Span, in_body: bool) {
        if self.is_param(of) && !in_body {
            self.types.set_optional(of);
        }
        if self.has_own_nil(of) {
            let shown = self.type_string(of);
            let message = format!("invalid type Option[{shown}]: {shown} has nil already");
            let fix = (Code::NoteOwnFixOption, format!("use {shown}"));
            self.error_with_fix(Code::InstOption, span, message, fix);
        }
    }

    /// `@Required` at `span` is on a field of the type `ty`: not on an
    /// option, which may be missing, nor on a type that is never nil
    /// (ADR-0016 decision 8).
    pub(super) fn required_field(&mut self, ty: TypeId, span: Span) {
        let shown = self.type_string(ty);
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Option(elem) => {
                let message = format!(
                    "cannot use @Required on a field of type {shown}: an option may be nil"
                );
                let fix = format!("use {} @Required, or {shown}", self.type_string(*elem));
                let fix = (Code::NoteOwnFixOption, fix);
                self.error_with_fix(Code::OptionAndRequired, span, message, fix);
            }
            Type::Struct(_) => {}
            _ if self.never_nil(ty) && !self.types.is_unsafe_pointer(ty, self.names) => {
                let message = format!(
                    "cannot use @Required on a field of type {shown}: a {shown} is never nil, \
                     so the field is set in every literal already"
                );
                self.error(Code::RequiredPointer, span, message);
            }
            _ => {}
        }
    }

    /// Whether a `@Required` field of the type is never nil: the type is
    /// a map, a channel, `error` or `any` (ADR-0016 decision 8).
    fn kept_set(&self, ty: TypeId) -> bool {
        matches!(
            &*self.types.get(self.types.underlying(ty)),
            Type::Map { .. } | Type::Chan { .. } | Type::Interface { .. }
        ) && self.types.has_nil(ty, Dialect::Gopp)
    }

    /// Whether the selector `expr` reaches a field that is never nil
    /// because it is `@Required`.
    fn reaches_kept_set(&self, cx: &Cx<'a>, expr: ExprId) -> bool {
        let Some(selection) = self.selections.get(&self.unparen(cx, expr)) else {
            return false;
        };
        let fields = self.types.get(self.types.underlying(selection.holder));
        let (Type::Struct(fields), SelectionKind::Field, Some(&at)) =
            (&*fields, selection.kind, selection.path.last())
        else {
            return false;
        };
        let field = fields.get(at as usize);
        field.is_some_and(|field| field.required && self.kept_set(field.ty))
    }

    /// The value `x` of `value` is stored in a `@Required` field of the
    /// type `ty`, which `place` names: if the field is never nil, the
    /// value is known to be set. Whether a local variable is, the flow of
    /// the function says.
    pub(super) fn stored_set(
        &mut self,
        cx: &Cx<'a>,
        place: &str,
        ty: TypeId,
        (value, x): (ExprId, &Operand),
    ) {
        if !self.own_rules(cx) || self.memo.is_none() || !self.kept_set(ty) || !x.is_value() {
            return;
        }
        let value = self.unparen(cx, value);
        let span = cx.tree.exprs[value].span;
        if x.ty == self.types.basic(Basic::UntypedNil) {
            let message = format!("cannot store nil in {place}: the field is @Required");
            let fix = format!(
                "store a value, or declare the field as Option[{}]",
                self.type_string(ty)
            );
            return self.error_with_fix(
                Code::RequiredNilStore,
                span,
                message,
                (Code::NoteOwnFixOption, fix),
            );
        }
        let made = match cx.tree.exprs[value].kind {
            ExprKind::CompositeLit { .. } | ExprKind::FuncLit { .. } => true,
            ExprKind::Call { fun, .. } => {
                let callee = self.memo.as_ref().and_then(|memo| memo.get(&fun));
                matches!(
                    callee.map(|callee| &callee.mode),
                    Some(Mode::Builtin(Predeclared::Make) | Mode::Type)
                )
            }
            ExprKind::Selector { .. } => self.reaches_kept_set(cx, value),
            ExprKind::Name(_) => match cx.body.uses.get(&value) {
                Some(&Ref::Local(local)) => self.flow_holds(local, flow::Known::Set, span),
                _ => false,
            },
            _ => false,
        };
        if made || !self.types.has_nil(x.ty, Dialect::Gopp) {
            return;
        }
        let message = format!(
            "cannot store {} in {place}: the field is @Required, and the value may be nil",
            self.expr_string(cx, value)
        );
        let fix = "put it in a variable and store it where that is not nil".to_owned();
        self.error_with_fix(
            Code::RequiredMaybeNil,
            span,
            message,
            (Code::NoteOwnFixOption, fix),
        );
    }

    /// The value is assigned to the place `place`, which `text` names.
    pub(super) fn field_stored(
        &mut self,
        cx: &Cx<'a>,
        place: ExprId,
        text: &str,
        ty: TypeId,
        value: (ExprId, &Operand),
    ) {
        if self.own_rules(cx) && self.reaches_kept_set(cx, place) {
            self.stored_set(cx, text, ty, value);
        }
    }

    /// `&x.f` is written at `expr` for a field that is never nil: nothing
    /// may store nil through the pointer, so only `mem.Replace` takes it.
    pub(super) fn address_of_required(&mut self, cx: &Cx<'a>, expr: ExprId, of: ExprId) {
        if self.own_rules(cx) && self.memo.is_some() && self.reaches_kept_set(cx, of) {
            self.required_addresses.push(expr);
        }
    }

    /// The call `call` of `mem.Take` or `mem.Replace`, as `name` says,
    /// has the arguments `args`: the address of a field that is never nil
    /// is given to `mem.Replace` with a value that is set, and not to
    /// `mem.Take`.
    pub(super) fn required_taken(&mut self, cx: &Cx<'a>, name: &str, args: &[ExprId]) {
        let Some(&first) = args.first() else {
            return;
        };
        let first = self.unparen(cx, first);
        let Some(at) = self
            .required_addresses
            .iter()
            .position(|&known| known == first)
        else {
            return;
        };
        self.required_addresses.swap_remove(at);
        let ExprKind::Unary { x: of, .. } = cx.tree.exprs[first].kind else {
            return;
        };
        let place = self.expr_string(cx, of);
        if name == "Take" {
            let message =
                format!("cannot take {place}: the field is @Required, so it is never nil");
            let fix = (
                Code::NoteOwnFixTake,
                format!("use mem.Replace(&{place}, v)"),
            );
            return self.error_with_fix(
                Code::RequiredMove,
                cx.tree.exprs[first].span,
                message,
                fix,
            );
        }
        let stored = args.get(1).and_then(|&value| {
            let x = self.memo.as_ref()?.get(&self.unparen(cx, value))?.clone();
            Some((value, x))
        });
        let ty = self
            .memo
            .as_ref()
            .and_then(|memo| memo.get(&self.unparen(cx, of)))
            .map(|x| x.ty);
        if let (Some((value, x)), Some(ty)) = (stored, ty) {
            self.stored_set(cx, &place, ty, (value, &x));
        }
    }

    /// The addresses of fields that are never nil that nothing took by
    /// the end of the body.
    pub(super) fn addresses_of_required(&mut self, cx: &Cx<'a>) {
        for expr in std::mem::take(&mut self.required_addresses) {
            let message = format!(
                "cannot take the address {}: the field is @Required, so it is never nil",
                self.expr_string(cx, expr)
            );
            let fix = (
                Code::NoteOwnFixTake,
                "use mem.Replace to give it another value".to_owned(),
            );
            self.error_with_fix(Code::RequiredAddr, cx.tree.exprs[expr].span, message, fix);
        }
    }

    /// The fields of a struct literal of the type `base` that are not
    /// set and must be: those that are `@Required` and those of a type
    /// without a zero value (ADR-0016 decisions 2 and 4). `set` says
    /// which fields the literal sets.
    pub(super) fn fields_missing(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        base: TypeId,
        fields: &[Field],
        set: &[bool],
    ) {
        if !self.own_rules(cx) || self.memo.is_none() {
            return;
        }
        let mut missing = Vec::new();
        for (field, _) in fields.iter().zip(set).filter(|&(_, &set)| !set) {
            let name = self.names.resolve(&field.name);
            if field.required {
                missing.push(format!("{name} (@Required)"));
            } else if self.zero_lacking(field.ty).is_some() {
                let shown = self.type_string(field.ty);
                missing.push(format!("{name} ({shown} has no zero value)"));
            }
        }
        if missing.is_empty() {
            return;
        }
        let message = format!(
            "missing {} in struct literal of type {}",
            missing.join(", "),
            self.type_string(base)
        );
        self.error(Code::RequiredMissing, cx.tree.exprs[expr].span, message);
    }

    /// The variables `names` of the type `ty` are declared without
    /// values.
    pub(super) fn declared_unset(&mut self, cx: &Cx<'a>, names: &[Ident], ty: TypeId) {
        let blank = self.blank;
        for name in names.iter().filter(|name| name.name != blank) {
            let text = self.names.resolve(&name.name);
            let doing = format!("cannot declare {text} without a value");
            let fix = format!(
                "give {text} a value, or declare it as Option[{}]",
                self.type_string(ty)
            );
            self.zero_needed(cx, name.span, ty, &doing, &fix);
        }
    }

    /// The value `x` of `expr` is read alone. An entry of a map and what
    /// a channel gives may be missing, and a type without a zero value
    /// has nothing to stand for it: only the form with `ok` reads it
    /// (ADR-0016 decision 9).
    pub(super) fn read_alone(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand) {
        if !self.own_rules(cx) || self.memo.is_none() {
            return;
        }
        let expr = self.unparen(cx, expr);
        let (what, form) = match (&x.mode, &cx.tree.exprs[expr].kind) {
            (Mode::MapIndex, _) => ("the entry may be missing", "v, ok := "),
            (
                Mode::CommaOk,
                ExprKind::Unary {
                    op: UnaryOp::Recv, ..
                },
            ) => ("the channel may be closed", "v, ok := "),
            _ => return,
        };
        if self.lookup_target == Some(expr) {
            return;
        }
        let Some(lacking) = self.zero_lacking(x.ty) else {
            return;
        };
        let text = self.expr_string(cx, expr);
        let message = format!("cannot read {text} alone: {what}, and {lacking}");
        let fix = (Code::NoteOwnFixOption, format!("use {form}{text}"));
        let span = cx.tree.exprs[expr].span;
        self.error_with_fix(Code::NoZeroLookup, span, message, fix);
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
        // An option that may be nil is given as it is, to an option, or
        // to `error` or `any`, where nil stays nil.
        let is_option =
            |ty: TypeId| matches!(&*self.types.get(self.types.underlying(ty)), Type::Option(_));
        let may_be_nil = is_option(x.ty);
        let keeps_nil = is_option(to) || self.types.has_nil(to, Dialect::Gopp);
        if may_be_nil && !keeps_nil {
            read.truncate(1);
        }
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
        match through {
            true => {
                self.writes_through.insert(place);
                let pointer = self.unparen(cx, place);
                let local = match cx.body.uses.get(&pointer) {
                    Some(&Ref::Local(local)) => Some(local),
                    _ => None,
                };
                if local.is_some_and(|local| self.read_only.contains(&local)) {
                    let message = format!(
                        "cannot assign to what {} points to: what a Shared holds is only read",
                        self.expr_string(cx, pointer)
                    );
                    let span = cx.tree.exprs[pointer].span;
                    self.error(Code::OwnSharedWrite, span, message);
                }
            }
            // The pointer itself is assigned.
            false => self.repointed(cx, place),
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
