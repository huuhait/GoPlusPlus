//! What fits a place in a `.gopp` file, where a pointer is used like the
//! variable it points to (OWN-P5), a `Unique` or `Shared` lends what it
//! owns (OWN-P6), a value is its own option (TYP-2), and a pointer, a
//! func and an interface with methods are never nil (OWN-P9).

use super::*;

/// How the reason starts that a type argument is refused for what
/// generic code keeps on the heap.
pub(super) const KEPT: &str = "a value of it is kept in ";

/// Why no type argument with a pointer is given to generic code that
/// keeps a value of its type parameter in `what`.
pub(super) fn kept_in(what: &str) -> String {
    format!("{KEPT}{what}, and a pointer in it may outlive what it points to")
}

/// What to do with a pointer that may not be kept where it is put.
pub(super) const HEAP_FIX: &str = "let the heap own the value with a Unique, a Shared or a Weak, \
                                   or keep the pointers in an array";

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
    pub(super) fn read(&self, mut x: Operand) -> Operand {
        if !x.is_value() {
            return x;
        }
        while !self.types.is_unsafe_pointer(x.ty, self.names) {
            x = match &*self.types.get(self.types.underlying(x.ty)) {
                Type::Pointer(elem) | Type::Unique(elem) => Operand::variable(*elem),
                Type::Shared(elem) => Operand::value(*elem),
                // What is tied to one goroutine is used like any other.
                Type::Local(elem) => Operand {
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
    pub(super) fn may_overload(&self, ty: TypeId) -> bool {
        self.types.declares_methods(ty)
    }

    /// The types whose values a place of the type `to` takes: its own,
    /// that of what an option holds, and, where an assignment `writes`,
    /// that of what a pointer points to.
    fn takes(&self, to: TypeId, writes: bool) -> Vec<TypeId> {
        let mut out = vec![to];
        let mut current = to;
        loop {
            current = match &*self.types.get(self.types.underlying(current)) {
                // A func or an interface value may be tied to one
                // goroutine, never the other way (CON-12).
                Type::Option(elem) | Type::Local(elem) => *elem,
                Type::Pointer(elem) if writes && out.len() == 1 => *elem,
                _ => return out,
            };
            out.push(current);
        }
    }

    /// Whether `nil` is a value of a place of the type `to`. It is one
    /// of a type parameter if it is one of every type that the parameter
    /// may be.
    pub(super) fn takes_nil(&self, to: TypeId, writes: bool) -> bool {
        self.takes(to, writes).into_iter().any(|to| {
            let has_nil = |this: &Self, ty: TypeId| {
                this.types.has_nil(ty) || this.types.is_unsafe_pointer(ty, this.names)
            };
            self.open(to) || self.all_types(to, has_nil)
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
            Type::Interface { .. } => !self.types.has_nil(ty),
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
    pub(super) fn zero_needed(&mut self, span: Span, ty: TypeId, doing: &str, fix: &str) {
        if self.memo.is_none() {
            return;
        }
        if let Some(lacking) = self.zero_lacking(ty) {
            let message = format!("{doing}: {lacking}");
            let fix = (Code::NoteOwnFixOption, fix.to_owned());
            self.error_with_fix(Code::NoZeroValue, span, message, fix);
        }
    }

    /// `expr` writes a predeclared generic type for the type `arg`, which
    /// it does not take.
    pub(super) fn bad_handle(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        arg: TypeId,
        bad: crate::types::BadArgument,
    ) {
        use crate::types::BadArgument;
        let written = self.expr_string(cx, expr);
        let shown = self.type_string(arg);
        let why = match bad {
            BadArgument::LocalOfValue => {
                format!("Local is written for a func or an interface type, and {shown} is neither")
            }
            BadArgument::PointerOnHeap => {
                format!("a {shown} borrows, and what borrows is not kept on the heap")
            }
        };
        let message = format!("invalid type {written}: {why}");
        self.error(Code::InvalidHandleType, cx.tree.exprs[expr].span, message);
    }

    /// Whether the name is a variable of a package.
    fn package_var(&self, cx: &Cx<'a>, name: ExprId) -> bool {
        match cx.body.uses.get(&name) {
            Some(Ref::Object(object)) => {
                matches!(self.package.objects[object.0 as usize].kind, ObjKind::Var)
            }
            Some(Ref::Member { import, member }) => {
                match &self.package.objects[import.0 as usize].kind {
                    ObjKind::Import { package, .. } => package
                        .objects
                        .get(member.0 as usize)
                        .is_some_and(|object| matches!(object.kind, ObjKind::Var)),
                    _ => false,
                }
            }
            _ => false,
        }
    }

    /// Whether the place is a variable of a package or a part of one.
    /// With `inline`, only a part that is where the variable is: a
    /// field, an element of an array, what a pointer in it points to;
    /// not an element of a slice or a map or what a handle holds, which
    /// may be replaced.
    fn in_package(&self, cx: &Cx<'a>, mut place: ExprId, inline: bool) -> bool {
        let typed = |this: &Self, expr: ExprId| {
            let known = this.memo.as_ref().and_then(|memo| memo.get(&expr));
            known.map(|operand| this.types.get(this.types.underlying(operand.ty)))
        };
        loop {
            place = self.unparen(cx, place);
            let of = match cx.tree.exprs[place].kind {
                ExprKind::Name(_) => return self.package_var(cx, place),
                ExprKind::Selector { x, .. } => {
                    if cx.body.uses.contains_key(&place) {
                        return self.package_var(cx, place);
                    }
                    x
                }
                ExprKind::Index { x, .. } | ExprKind::Slice { x, .. } => x,
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => x,
                _ => return false,
            };
            let moves = matches!(
                typed(self, self.unparen(cx, of)).as_deref(),
                Some(Type::Slice(_) | Type::Map { .. } | Type::Unique(_) | Type::Shared(_))
            );
            if inline && moves {
                return false;
            }
            place = of;
        }
    }

    /// Whether every pointer in the value of the expression points into
    /// a variable of a package, which lives as long as the program:
    /// the address of one or of a part of it, what such a variable
    /// holds, nil, and what holds no pointer.
    pub(super) fn points_to_packages(&mut self, cx: &Cx<'a>, expr: ExprId) -> bool {
        let mut rest = vec![expr];
        while let Some(next) = rest.pop() {
            self.steps += 1;
            let next = self.unparen(cx, next);
            let known = self.memo.as_ref().and_then(|memo| memo.get(&next));
            let ty = known.map(|operand| operand.ty);
            if ty.is_some_and(|ty| !self.types.holds_pointer(ty)) {
                continue;
            }
            match &cx.tree.exprs[next].kind {
                ExprKind::Unary {
                    op: UnaryOp::Addr,
                    x,
                } => {
                    if !self.in_package(cx, *x, true) {
                        return false;
                    }
                }
                ExprKind::CompositeLit { elems, .. } => rest.extend(elems.iter()),
                ExprKind::KeyValue { value, .. } => rest.push(*value),
                // A conversion gives what it is given.
                ExprKind::Call { fun, args, .. }
                    if args.len() == 1
                        && self.memo.as_ref().is_some_and(|memo| {
                            let callee = memo.get(&self.unparen(cx, *fun));
                            callee.is_some_and(|callee| callee.mode == Mode::Type)
                        }) =>
                {
                    rest.push(args[0]);
                }
                ExprKind::Name(_)
                    if cx.body.uses.get(&next) == Some(&Ref::Universe(Predeclared::Nil)) => {}
                _ => {
                    if !self.in_package(cx, next, false) {
                        return false;
                    }
                }
            }
        }
        true
    }

    /// The place gets the value: a variable of a package, or a part of
    /// one, holds no pointer that may end before the program does
    /// (OWN-B5). `value` is `None` for what a call gives.
    pub(super) fn kept_by_package(
        &mut self,
        cx: &Cx<'a>,
        place: ExprId,
        to: TypeId,
        value: std::option::Option<ExprId>,
    ) {
        if !self.in_package(cx, place, false) || !self.types.holds_pointer(to) {
            return;
        }
        if value.is_some_and(|value| self.points_to_packages(cx, value)) {
            return;
        }
        let at = value.unwrap_or(place);
        self.on_the_heap(to, cx.tree.exprs[at].span, "a variable of the package");
    }

    /// A value of the type is put on the heap, into `what`: refused if
    /// it holds a pointer, which the compiler follows only in the
    /// variables of functions (OWN-B5). Whether it was refused.
    pub(super) fn on_the_heap(&mut self, ty: TypeId, span: Span, what: &str) -> bool {
        match self.types.held(ty) {
            Held::Pointer => {}
            // What a type parameter stands for may hold one: the type
            // arguments are held to it.
            Held::Params(params) => {
                if !params.is_empty() {
                    self.kept_where(&params, span, &kept_in(what));
                }
                return false;
            }
        }
        let message = format!(
            "cannot keep a {} in {what}: a pointer in it may outlive what it points to",
            self.type_string(ty)
        );
        let fix = (Code::NoteOwnFixHeapPointer, HEAP_FIX.to_owned());
        self.error_with_fix(Code::OwnPointerOnHeap, span, message, fix);
        true
    }

    /// Whether the type has nil of its own, so that an option of it says
    /// nothing more (OWN-P9).
    pub(super) fn has_own_nil(&self, ty: TypeId) -> bool {
        self.types.has_own_nil(ty, self.names)
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
            // What the type parameter is, each type argument says: it
            // is held to this where the arguments are given.
            Type::Param(_) => self.types.set_required(ty),
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
        ) && self.types.has_nil(ty)
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
        if self.memo.is_none() || !self.kept_set(ty) || !x.is_value() {
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
        if made || !self.types.has_nil(x.ty) {
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
        if self.reaches_kept_set(cx, place) {
            self.stored_set(cx, text, ty, value);
        }
    }

    /// `&x.f` is written at `expr` for a field that is never nil: nothing
    /// may store nil through the pointer, so only `mem.Replace` takes it.
    pub(super) fn address_of_required(&mut self, cx: &Cx<'a>, expr: ExprId, of: ExprId) {
        if self.memo.is_some() && self.reaches_kept_set(cx, of) {
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
        if self.memo.is_none() {
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
    pub(super) fn declared_unset(&mut self, names: &[Ident], ty: TypeId) {
        let blank = self.blank;
        for name in names.iter().filter(|name| name.name != blank) {
            let text = self.names.resolve(&name.name);
            let doing = format!("cannot declare {text} without a value");
            let fix = format!(
                "give {text} a value, or declare it as Option[{}]",
                self.type_string(ty)
            );
            self.zero_needed(name.span, ty, &doing, &fix);
        }
    }

    /// The value `x` of `expr` is read alone. An entry of a map and what
    /// a channel gives may be missing, and a type without a zero value
    /// has nothing to stand for it: only the form with `ok` reads it
    /// (ADR-0016 decision 9).
    pub(super) fn read_alone(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand) {
        if self.memo.is_none() {
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
        let keeps_nil = is_option(to) || self.types.has_nil(to);
        if may_be_nil && !keeps_nil {
            read.truncate(1);
        }
        // A `Unique` and a `Shared` have the methods of what they hold
        // (OWN-P5): in an interface it is the handle that is held.
        let is_handle = matches!(
            &*self.types.get(self.types.underlying(x.ty)),
            Type::Unique(_) | Type::Shared(_)
        );
        let handle = x.ty;
        read.iter().find_map(|&from| {
            let x = Operand {
                mode: x.mode.clone(),
                ty: from,
            };
            let taken = takes
                .iter()
                .find(|&&to| !self.not_assignable(&x, to) || self.owned_by_interface(from, to));
            let upcast = || {
                takes
                    .iter()
                    .find(|&&to| self.upcast(from, to) == Some(true))
            };
            taken.or_else(upcast).copied().map(|to| {
                match is_handle && self.types.is_interface(to) {
                    true => (handle, to),
                    false => (from, to),
                }
            })
        })
    }

    /// Whether a value of the type `from` is held by the interface `to`
    /// through its methods with a pointer receiver: the interface owns
    /// the value, so it has an address to give them (TYP-5).
    pub(super) fn owned_by_interface(&self, from: TypeId, to: TypeId) -> bool {
        let plain = !matches!(
            &*self.types.get(self.types.underlying(from)),
            Type::Pointer(_) | Type::Interface { .. } | Type::Invalid
        ) && !self.is_untyped(from)
            && !self.is_param(from);
        plain
            && self.types.is_interface(to)
            && !self.not_assignable(&Operand::value(self.types.pointer(from)), to)
    }

    /// Whether the operand `x` surely fits no place of the type `to`;
    /// `writes` says that the place is on the left of `=`.
    pub(super) fn unfit(&self, x: &Operand, to: TypeId, writes: bool) -> bool {
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
        if !is_pointer || self.memo.is_none() {
            return;
        }
        let through = match x {
            None => true,
            Some(x) if x.mode == Mode::Unknown => false,
            Some(x) if x.ty == self.types.basic(Basic::UntypedNil) => !self.takes_nil(to, false),
            Some(x) => self.unfit(x, to, false),
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
}
