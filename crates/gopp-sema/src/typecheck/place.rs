//! Where the value that an expression names lives, how a use gives it to
//! a new owner (OWN-R3, OWN-R3a), and which variables only name a value
//! that is read (OWN-R3b).

use super::*;
use crate::object::{Capture, CaptureUse, MethodValue, Transfer};
use crate::types::{Category, Marks, NotCopied};

/// Where the value of an expression lives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Place {
    /// In a variable of the function that names it.
    Local(LocalId),
    /// In no variable: the result of a call, a literal, what `move`
    /// gives, and a part of one of these.
    Temporary,
    /// Anywhere else: a field, an element, an entry of a map, a variable
    /// of the package or of a function around, what a pointer points to.
    Elsewhere,
}

/// What a variable of a range clause is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Binding {
    /// The plain second variable over a slice, an array or a map: a copy
    /// of an element, which is not made while the loop only reads it.
    Element,
    /// The first variable over one of these: a copy of an index or of a
    /// key, likewise.
    Key,
    /// `&v`: the element itself.
    Alias,
}

/// Where a value is given.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Given {
    /// To a variable, a field, an element, a channel, a built-in function
    /// that keeps it.
    Stored,
    /// To a parameter of a function, which may only read it.
    Passed,
    /// To the caller, which ends the variables of the function.
    Returned,
}

/// What the function that is being called does with its arguments.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Operands {
    /// A built-in function keeps them.
    Kept,
    /// A function gets them in its parameters.
    Passed,
    /// An operator method or a conversion method reads them where they
    /// are (PAR-5).
    Read,
}

/// What is done to a place that a variable of a range clause may not be
/// done to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Change {
    Assigned,
    Addressed,
    /// `delete` and `clear`, by the name of the function.
    Emptied(&'static str),
}

impl<'a> Checker<'a> {
    /// Whether what a value of the type reaches with a selector or an
    /// index is in the storage of another owner.
    fn reaches_out(&self, mut ty: TypeId) -> bool {
        loop {
            ty = match &*self.types.get(self.types.underlying(ty)) {
                Type::Pointer(_) | Type::Shared(_) | Type::Weak(_) => return true,
                Type::Option(elem) => *elem,
                _ => return false,
            };
        }
    }

    /// The type that the expression was given when it was walked.
    fn walked_type(&self, expr: ExprId) -> Option<TypeId> {
        let memo = self.memo.as_ref()?;
        memo.get(&expr).map(|x| x.ty)
    }

    /// Whether the local is a variable of a function around the function
    /// literal that is being walked.
    fn captured(&self, cx: &Cx<'a>, local: LocalId) -> bool {
        let declared = cx.body.locals[local.0 as usize].span;
        self.literals
            .last()
            .is_some_and(|(_, literal)| declared.lo < literal.lo || literal.hi < declared.hi)
    }

    /// The local variable that the name `expr` denotes.
    fn variable(&self, cx: &Cx<'a>, expr: ExprId) -> Option<LocalId> {
        use LocalKind::*;
        match cx.body.uses.get(&expr) {
            Some(&Ref::Local(local)) => {
                let kind = cx.body.locals[local.0 as usize].kind;
                matches!(kind, Var | Receiver | Param | Result).then_some(local)
            }
            _ => None,
        }
    }

    /// Where the value of the expression lives.
    pub(super) fn place_of(&mut self, cx: &Cx<'a>, mut expr: ExprId) -> Place {
        let mut part = false;
        loop {
            self.steps += 1;
            let of = match cx.tree.exprs[expr].kind {
                ExprKind::Paren(x) => {
                    expr = x;
                    continue;
                }
                ExprKind::Name(_) => {
                    return match (self.variable(cx, expr), cx.body.uses.get(&expr)) {
                        (Some(local), _) => {
                            let alias = self.bindings.get(&local) == Some(&Binding::Alias);
                            match part || alias || self.captured(cx, local) {
                                true => Place::Elsewhere,
                                false => Place::Local(local),
                            }
                        }
                        (None, Some(Ref::Object(_) | Ref::Member { .. })) => Place::Elsewhere,
                        _ => Place::Temporary,
                    };
                }
                // `pkg.Name` is a variable of another package.
                ExprKind::Selector { .. } if cx.body.uses.contains_key(&expr) => {
                    return Place::Elsewhere;
                }
                ExprKind::Selector { x, .. } | ExprKind::Index { x, .. } => x,
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => x,
                _ => return Place::Temporary,
            };
            if self.walked_type(of).is_none_or(|ty| self.reaches_out(ty)) {
                return Place::Elsewhere;
            }
            // A part of a temporary is one; a part of a variable is not
            // the variable.
            part = true;
            expr = of;
        }
    }

    /// The variable that a change of the place `expr` changes: the place
    /// is the variable or a part of it, which is said too. A part that a
    /// pointer points to is none.
    fn root(&mut self, cx: &Cx<'a>, mut expr: ExprId) -> Option<(LocalId, bool)> {
        let mut part = false;
        loop {
            self.steps += 1;
            let of = match cx.tree.exprs[expr].kind {
                ExprKind::Paren(x) => {
                    expr = x;
                    continue;
                }
                ExprKind::Name(_) => return Some((self.variable(cx, expr)?, part)),
                ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Slice { x, .. } => x,
                _ => return None,
            };
            if self.reaches_out(self.walked_type(of)?) {
                return None;
            }
            part = true;
            expr = of;
        }
    }

    /// Whether the place `expr` is in what a `Shared` holds, which is only
    /// read through the handle and through a pointer taken into it
    /// (OWN-P8).
    pub(super) fn held_shared(&mut self, cx: &Cx<'a>, mut expr: ExprId) -> bool {
        loop {
            self.steps += 1;
            let of = match cx.tree.exprs[expr].kind {
                ExprKind::Paren(x) => {
                    expr = x;
                    continue;
                }
                ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Slice { x, .. } => x,
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => x,
                _ => return false,
            };
            let Some(mut ty) = self.walked_type(of) else {
                return false;
            };
            // What an option holds is reached like the option.
            while let Type::Option(elem) = &*self.types.get(self.types.underlying(ty)) {
                ty = *elem;
            }
            match &*self.types.get(self.types.underlying(ty)) {
                Type::Shared(_) => return true,
                Type::Pointer(_) => {
                    let pointer = self.variable(cx, self.unparen(cx, of));
                    return pointer.is_some_and(|local| self.read_only.contains(&local));
                }
                _ => expr = of,
            }
        }
    }

    /// Whether the value of `expr` is a pointer into what a `Shared`
    /// holds: the address of such a place, the `Shared` itself where a
    /// pointer is wanted, or a variable that is such a pointer.
    fn points_into_shared(&mut self, cx: &Cx<'a>, expr: ExprId) -> bool {
        let expr = self.unparen(cx, expr);
        match cx.tree.exprs[expr].kind {
            ExprKind::Unary {
                op: UnaryOp::Addr,
                x,
            } => self.held_shared(cx, x),
            ExprKind::Name(_) => {
                let local = self.variable(cx, expr);
                let is_shared = self.walked_type(expr).is_some_and(|ty| {
                    matches!(&*self.types.get(self.types.underlying(ty)), Type::Shared(_))
                });
                is_shared || local.is_some_and(|local| self.read_only.contains(&local))
            }
            _ => self.walked_type(expr).is_some_and(|ty| {
                matches!(&*self.types.get(self.types.underlying(ty)), Type::Shared(_))
            }),
        }
    }

    /// The variables `locals` are declared with the values `values`: a
    /// pointer that gets a pointer into what a `Shared` holds only reads.
    pub(super) fn only_read(
        &mut self,
        cx: &Cx<'a>,
        locals: &[std::option::Option<LocalId>],
        values: &[ExprId],
    ) {
        if !self.own_rules(cx) || locals.len() != values.len() {
            return;
        }
        for (local, &value) in locals.iter().zip(values) {
            if let Some(local) = *local
                && self.points_into_shared(cx, value)
            {
                self.read_only.insert(local);
            }
        }
    }

    /// The place `expr` is changed, as `doing` says: not what a `Shared`
    /// holds. Whether something was said.
    pub(super) fn shared_changed(&mut self, cx: &Cx<'a>, expr: ExprId, doing: &str) -> bool {
        if !self.own_rules(cx) || self.memo.is_none() || !self.held_shared(cx, expr) {
            return false;
        }
        let message = format!(
            "cannot {doing} {}: what a Shared holds is only read through it",
            self.expr_string(cx, expr)
        );
        let fix = "put what changes in a Mutex or an Atomic, or own the value with a Unique";
        let span = cx.tree.exprs[expr].span;
        self.error_with_fix(
            Code::OwnSharedWrite,
            span,
            message,
            (Code::NoteOwnFixOption, fix.to_owned()),
        );
        true
    }

    /// The function literals that are being walked and do not declare the
    /// variable do `how` to it.
    pub(super) fn capture(&mut self, cx: &Cx<'a>, local: LocalId, how: CaptureUse) {
        if self.literals.is_empty() || self.memo.is_none() {
            return;
        }
        let declared = cx.body.locals[local.0 as usize].span;
        for at in (0..self.literals.len()).rev() {
            let (literal, span) = self.literals[at];
            if span.lo <= declared.lo && declared.hi <= span.hi {
                return;
            }
            self.steps += 1;
            // If the literal does as much already, so do those around it.
            match self.captures.insert((literal, local), how) {
                Some(known) if known >= how => {
                    self.captures.insert((literal, local), known);
                    return;
                }
                _ => {}
            }
        }
    }

    /// The name `expr` is used: a variable that it denotes is read.
    pub(super) fn read_variable(&mut self, cx: &Cx<'a>, expr: ExprId) {
        if let (false, Some(local)) = (self.literals.is_empty(), self.variable(cx, expr)) {
            self.capture(cx, local, CaptureUse::Read);
        }
    }

    /// The captures of the body that was walked, by their literals.
    pub(super) fn captured_by_literal(&mut self) -> FxHashMap<ExprId, Vec<Capture>> {
        let mut all: FxHashMap<ExprId, Vec<Capture>> = FxHashMap::default();
        for ((literal, local), how) in self.captures.drain() {
            all.entry(literal).or_default().push(Capture { local, how });
        }
        for captures in all.values_mut() {
            captures.sort_by_key(|capture| capture.local);
        }
        all
    }

    /// The place `expr` is assigned to: its variable is written, unless
    /// the place is a pointer, which may be written through.
    pub(super) fn wrote(&mut self, cx: &Cx<'a>, expr: ExprId) {
        if self.literals.is_empty() {
            return;
        }
        let Some((local, part)) = self.root(cx, expr) else {
            return;
        };
        let ty = self.locals.get(&local).map_or(Types::INVALID, |x| x.ty);
        let is_pointer = matches!(
            &*self.types.get(self.types.underlying(ty)),
            Type::Pointer(_)
        );
        if part || !is_pointer || !self.own_rules(cx) {
            self.capture(cx, local, CaptureUse::Write);
        }
    }

    /// The pointer variable `expr` is pointed at something else.
    pub(super) fn repointed(&mut self, cx: &Cx<'a>, expr: ExprId) {
        if let (false, Some((local, false))) = (self.literals.is_empty(), self.root(cx, expr)) {
            self.capture(cx, local, CaptureUse::Write);
        }
    }

    /// The address of the place `expr` is taken.
    pub(super) fn addressed(&mut self, cx: &Cx<'a>, expr: ExprId) {
        if self.literals.is_empty() {
            return;
        }
        if let Some((local, _)) = self.root(cx, expr) {
            self.capture(cx, local, CaptureUse::Address);
        }
    }

    /// The plain range variable that a change of the place `expr`
    /// changes, and whether the place is a part of it.
    fn bound(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<(LocalId, bool)> {
        let (local, part) = self.root(cx, expr)?;
        let copied = matches!(
            self.bindings.get(&local),
            Some(Binding::Element | Binding::Key)
        );
        copied.then_some((local, part))
    }

    /// The place `expr` is changed: a variable of a plain range clause is
    /// a copy that the loop only reads, so a change of a part of it would
    /// be lost, and one that is not copied is the element, which is read
    /// only (OWN-R3b, RNG-1).
    pub(super) fn changed(&mut self, cx: &Cx<'a>, expr: ExprId, change: Change) {
        if !self.own_rules(cx) || self.memo.is_none() {
            return;
        }
        let Some((local, part)) = self.bound(cx, expr) else {
            return;
        };
        let ty = self.locals.get(&local).map_or(Types::INVALID, |x| x.ty);
        let whole = !part && change == Change::Assigned;
        if whole && self.types.category(ty) != Category::NoCopy {
            return;
        }
        let name = self.names.resolve(&cx.body.locals[local.0 as usize].name);
        let text = self.expr_string(cx, expr);
        let is_key = self.bindings.get(&local) == Some(&Binding::Key);
        let of = match is_key {
            true => "a key",
            false => "an element",
        };
        let what = match self.types.category(ty) {
            Category::NoCopy => format!("{name} is {of} that the loop only reads"),
            _ => format!("{name} is a copy of {of}, and the change would be lost"),
        };
        let (code, message) = match change {
            Change::Assigned => (
                Code::OwnRangeVarWrite,
                format!("cannot assign to {text}: {what}"),
            ),
            Change::Addressed => (
                Code::OwnBorrowedAddress,
                format!("cannot take the address of {text}: {what}"),
            ),
            Change::Emptied(function) => (
                Code::OwnRangeVarWrite,
                format!("cannot {function} {text}: {what}"),
            ),
        };
        let span = cx.tree.exprs[expr].span;
        // Only an element has another name.
        if is_key {
            return self.error(code, span, message);
        }
        let fix = format!("write &{name} in the range clause to name the element itself");
        self.error_with_fix(code, span, message, (Code::NoteOwnFixAlias, fix));
    }

    /// Whether a value is given to a new owner where a message says
    /// `context`, and how.
    pub(super) fn gives(&self, context: &str) -> Option<Given> {
        match context {
            "return statement" => Some(Given::Returned),
            "assignment"
            | "variable declaration"
            | "send"
            | "struct literal"
            | "array or slice literal"
            | "map literal" => Some(Given::Stored),
            // The key is looked up, and these functions only read.
            "argument to delete" => None,
            _ if context.starts_with("argument to built-in ") => None,
            _ if context.starts_with("argument to unsafe.") => None,
            _ if context.starts_with("argument to ") => match self.operands {
                Operands::Kept => Some(Given::Stored),
                Operands::Passed => Some(Given::Passed),
                Operands::Read => None,
            },
            _ => None,
        }
    }

    /// Whether a function that only reads a parameter of the type gets
    /// the argument as it is, whatever its elements are: a slice (PAR-5,
    /// GEN-3). Whether the function only reads is known with its body.
    fn passed_as_it_is(&self, ty: TypeId, how: Given) -> bool {
        let is_slice = matches!(&*self.types.get(self.types.underlying(ty)), Type::Slice(_));
        how == Given::Passed && is_slice
    }

    /// The value of `expr` is given to a new owner: if it is a part
    /// `x[lo:hi]` of a slice or an array that stays, its elements are
    /// copied, which they must allow (OWN-S2). A view `&x[lo:hi]` names
    /// the same elements, `x = x[lo:hi]` keeps them, and a returned
    /// variable ends.
    pub(super) fn slice_given(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand, how: Given) {
        let expr = self.unparen(cx, expr);
        let ExprKind::Slice { x: of, .. } = cx.tree.exprs[expr].kind else {
            return;
        };
        if self.memo.is_none() || self.types.category(x.ty) != Category::NoCopy {
            return;
        }
        let of = self.unparen(cx, of);
        let reached = self.walked_type(of).is_none_or(|ty| self.reaches_out(ty));
        let place = match reached {
            true => Place::Elsewhere,
            false => self.place_of(cx, of),
        };
        let kept = self.self_moves.contains(&of);
        let returned = matches!(place, Place::Local(_)) && how == Given::Returned;
        if place == Place::Temporary || kept || returned {
            if returned {
                self.var_modes.insert(of, Transfer::Move);
            }
            return;
        }
        if self.passed_as_it_is(x.ty, how) {
            self.var_modes.insert(of, Transfer::Copy);
            return;
        }
        let Some((part, why)) = self.types.not_copied(x.ty, true) else {
            return;
        };
        if self.open(part) {
            return;
        }
        let text = self.expr_string(cx, expr);
        let code = match (place, why) {
            (_, NotCopied::Param) => Code::OwnGenericCopy,
            (Place::Local(_), _) => Code::OwnCopyLocal,
            _ => Code::OwnR3aNeedsOwnership,
        };
        let message = format!(
            "cannot copy the elements of {text}: {}",
            self.not_copied_text(part, why, true)
        );
        let fix = format!("use &{text} to name the same elements");
        let span = cx.tree.exprs[expr].span;
        self.error_with_fix(code, span, message, (Code::NoteOwnFixAddress, fix));
    }

    /// The value of the place `expr` is given to a new owner as a value
    /// of the type `ty`: a copy of it is, unless it cannot be copied
    /// (OWN-R3, OWN-R3a). A temporary is given as it is, and nothing is
    /// noted of a value that is copied by its bytes.
    pub(super) fn given(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand, ty: TypeId, how: Given) {
        let is_place = matches!(x.mode, Mode::Variable | Mode::MapIndex);
        if !self.own_rules(cx) || self.memo.is_none() || !is_place {
            return;
        }
        let category = self.types.category(ty);
        if category == Category::Trivial {
            return;
        }
        let expr = self.unparen(cx, expr);
        // What a pointer or a handle is read as is where it points.
        let direct = x.ty == ty;
        let place = match direct {
            true => self.place_of(cx, expr),
            false => Place::Elsewhere,
        };
        if place == Place::Temporary {
            return;
        }
        if self.swapping {
            self.var_modes.insert(expr, Transfer::Move);
            return;
        }
        let same = |target: &String| *target == self.expr_string(cx, expr);
        let assigned_over = self.target.as_ref().is_some_and(same);
        if assigned_over {
            self.self_moves.insert(expr);
        }
        if category == Category::Copyable {
            self.var_modes.insert(expr, Transfer::Copy);
            return;
        }
        let binding = match place {
            Place::Local(local) => self.bindings.get(&local).copied(),
            _ => None,
        };
        let element = binding.is_some();
        if self.passed_as_it_is(ty, how) {
            self.var_modes.insert(expr, Transfer::Copy);
            return;
        }
        let Some((part, why)) = self.types.not_copied(ty, true) else {
            return;
        };
        // Nothing is known of a type parameter whose constraint is in
        // error.
        if self.open(part) {
            return;
        }
        // What is assigned over is given once, and a returned variable
        // ends with its function.
        let returned = matches!(place, Place::Local(_)) && how == Given::Returned && !element;
        let over = assigned_over && why != NotCopied::Pinned;
        if over || returned {
            if over {
                self.target = None;
            }
            self.var_modes.insert(expr, Transfer::Move);
            return;
        }
        let text = self.expr_string(cx, expr);
        let shown = self.describe(cx, expr, x);
        let cause = self.not_copied_text(part, why, true);
        let (code, message, fix) = match (place, why) {
            (_, NotCopied::Pinned) => (
                Code::OwnPinnedCopy,
                format!("cannot copy {shown}: {cause}"),
                direct.then(|| (Code::NoteOwnFixAddress, format!("use &{text}"))),
            ),
            (Place::Local(_), _) if element => {
                let fix = format!(
                    "write &{text} in the range clause, and use mem.Take(&{text}) or \
                     mem.Replace(&{text}, v)"
                );
                (
                    Code::OwnR3aNeedsOwnership,
                    format!("cannot copy {shown} out of what is ranged over: {cause}"),
                    (binding == Some(Binding::Element)).then_some((Code::NoteOwnFixAlias, fix)),
                )
            }
            (Place::Local(_), _) => {
                let code = match why {
                    NotCopied::Param => Code::OwnGenericCopy,
                    _ => Code::OwnCopyLocal,
                };
                let fix = format!("use move({text}) to give the value, or &{text} to lend it");
                (
                    code,
                    format!("cannot copy {shown}: {cause}"),
                    Some((Code::NoteOwnFixMove, fix)),
                )
            }
            _ => {
                let code = match why {
                    NotCopied::Param => Code::OwnGenericCopy,
                    _ => Code::OwnR3aNeedsOwnership,
                };
                // An entry of a map has no address.
                let fix = match (&x.mode, direct, self.may_lack_zero(ty)) {
                    (Mode::MapIndex, ..) => None,
                    (_, false, true) => Some(format!("use mem.Replace({text}, v)")),
                    (_, false, false) => {
                        Some(format!("use mem.Take({text}) or mem.Replace({text}, v)"))
                    }
                    (_, true, true) => Some(format!("use &{text} or mem.Replace(&{text}, v)")),
                    (_, true, false) => Some(format!(
                        "use &{text}, mem.Take(&{text}) or mem.Replace(&{text}, v)"
                    )),
                };
                (
                    code,
                    format!("cannot copy {shown} out of its place: {cause}"),
                    fix.map(|fix| (Code::NoteOwnFixTake, fix)),
                )
            }
        };
        let span = cx.tree.exprs[expr].span;
        match fix {
            Some(fix) => self.error_with_fix(code, span, message, fix),
            None => self.error(code, span, message),
        }
    }

    /// The selector `expr` reaches a method of the value `x` of `base`. A
    /// value receiver gets a copy of `x` (PAR-1, CLO-5a); the copy method
    /// reads its receiver where it is, and what a called method does with
    /// a receiver that is not copied, the method says. A pointer receiver
    /// gets the address of a variable. A method that is not called is a
    /// value that holds its receiver.
    pub(super) fn method_selected(
        &mut self,
        cx: &Cx<'a>,
        (expr, name): (ExprId, Spur),
        base: ExprId,
        x: &Operand,
        pointer_receiver: bool,
    ) {
        let called = self.callee == Some(expr);
        // A pointer is the receiver itself.
        let is_variable = x.mode == Mode::Variable
            && self
                .walked_type(base)
                .is_some_and(|ty| !self.reaches_out(ty));
        let bound = pointer_receiver && is_variable;
        if bound {
            self.addressed(cx, base);
        }
        if !called && self.memo.is_some() {
            let holds = match bound {
                true => MethodValue::Address,
                false => MethodValue::Value,
            };
            self.method_values.insert(expr, holds);
        }
        if pointer_receiver || !self.own_rules(cx) {
            return;
        }
        let Some(holder) = self.selections.get(&expr).map(|selection| selection.holder) else {
            return;
        };
        // A method of an interface is called on the value where it is.
        if self.is_param(holder) || called && self.types.is_interface(holder) {
            return;
        }
        // The copy method, an operator and a conversion read the receiver
        // where it is (PAR-5).
        let read_in_place =
            called && (self.names.resolve(&name) == "Clone" || self.reads_operands(cx, expr));
        if !read_in_place {
            self.given(cx, base, x, holder, Given::Stored);
        }
    }

    /// Whether the selector `expr` reaches an operator method or a
    /// conversion method, which read their operands where they are.
    pub(super) fn reads_operands(&self, cx: &Cx<'a>, expr: ExprId) -> bool {
        let ExprKind::Selector { sel, .. } = cx.tree.exprs[expr].kind else {
            return false;
        };
        let Some(selection) = self.selections.get(&expr) else {
            return false;
        };
        let package = self.package_of(sel.name);
        let marks = self.types.method_marks(selection.holder, sel.name, package);
        marks.has(Marks::OPERATOR) || marks.has(Marks::CONVERT)
    }

    /// `x[lo:hi]` as the new value of `x` is the part of `x` itself: the
    /// rest of `x` is assigned over (OWN-R4).
    pub(super) fn sliced(&mut self, cx: &Cx<'a>, x: ExprId) {
        let x = self.unparen(cx, x);
        let same = |target: &String| *target == self.expr_string(cx, x);
        if self.memo.is_some() && self.target.as_ref().is_some_and(same) {
            self.self_moves.insert(x);
        }
    }

    /// Whether the two lists name the same places, each once, in another
    /// order: `a, b = b, a`, `xs[i], xs[j] = xs[j], xs[i]`. No bytes move
    /// before every index is evaluated, so no value is copied.
    pub(super) fn is_swap(&mut self, cx: &Cx<'a>, lhs: &[ExprId], rhs: &[ExprId]) -> bool {
        if !self.own_rules(cx) || lhs.len() < 2 || lhs.len() != rhs.len() {
            return false;
        }
        let all: Vec<ExprId> = lhs.iter().chain(rhs).copied().collect();
        if !all.iter().all(|&expr| self.is_plain_place(cx, expr)) {
            return false;
        }
        let texts = |this: &Self, exprs: &[ExprId]| {
            let mut texts: Vec<String> = exprs.iter().map(|&x| this.expr_string(cx, x)).collect();
            texts.sort();
            texts
        };
        let (left, right) = (texts(self, lhs), texts(self, rhs));
        left == right && left.windows(2).all(|pair| pair[0] != pair[1])
    }

    /// Whether the expression is written with names, fields, indexes,
    /// stars and operators only: it is the same place each time it is
    /// evaluated in one statement.
    pub(super) fn is_plain_place(&mut self, cx: &Cx<'a>, expr: ExprId) -> bool {
        let mut rest = vec![expr];
        while let Some(expr) = rest.pop() {
            self.steps += 1;
            match &cx.tree.exprs[expr].kind {
                ExprKind::Name(_) | ExprKind::BasicLit { .. } => {}
                ExprKind::Paren(x) | ExprKind::Selector { x, .. } => rest.push(*x),
                ExprKind::Unary { op, x } if *op != UnaryOp::Recv => rest.push(*x),
                ExprKind::Binary { x, y, .. } => rest.extend([*x, *y]),
                ExprKind::Index { x, indices } => {
                    rest.push(*x);
                    rest.extend(indices);
                }
                _ => return false,
            }
        }
        true
    }

    /// `mem.Take(&place)` leaves the zero value of the type in the place,
    /// which the type must have, and neither it nor `mem.Replace` moves a
    /// value that stays where it is (OWN-R4, OWN-R10).
    pub(super) fn taken(
        &mut self,
        cx: &Cx<'a>,
        (call, args): (ExprId, &[ExprId]),
        fun: ExprId,
        result: &Operand,
    ) {
        // `mem.Take[T]` names the function too.
        let mut fun = self.unparen(cx, fun);
        if let ExprKind::Index { x, .. } = cx.tree.exprs[fun].kind {
            fun = self.unparen(cx, x);
        }
        let Some(&Ref::Member { import, member }) = cx.body.uses.get(&fun) else {
            return;
        };
        let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
            return;
        };
        let name = self.names.resolve(&package.object(member).name);
        if package.path != "mem" || !matches!(name, "Take" | "Replace") || !result.is_value() {
            return;
        }
        self.required_taken(cx, name, args);
        if let Some(&first) = args.first() {
            let first = self.unparen(cx, first);
            let place = match cx.tree.exprs[first].kind {
                ExprKind::Unary {
                    op: UnaryOp::Addr,
                    x,
                } => Some(x),
                _ => None,
            };
            let through = place.is_none() && self.points_into_shared(cx, first);
            if place.is_some_and(|place| self.shared_changed(cx, place, "take")) {
                return;
            }
            if through {
                let message = format!(
                    "cannot take what {} points to: what a Shared holds is only read",
                    self.expr_string(cx, first)
                );
                self.error(Code::OwnSharedWrite, cx.tree.exprs[first].span, message);
                return;
            }
        }
        let ty = result.ty;
        let text = self.expr_string(cx, call);
        let span = cx.tree.exprs[call].span;
        if let Some((part, NotCopied::Pinned)) = self.types.not_copied(ty, true) {
            let cause = self.not_copied_text(part, NotCopied::Pinned, true);
            let message = format!("cannot use {text}: {cause}, and stays in its place");
            self.error(Code::OwnPinnedMove, span, message);
        } else if name == "Take" && self.may_lack_zero(ty) {
            let lacks = match self.is_param(ty) {
                true => "may be a type with no zero value",
                false => "has no zero value",
            };
            let message = format!(
                "cannot use {text}: {} {lacks} to leave in the place",
                self.type_string(ty)
            );
            let fix = "use mem.Replace, which leaves a value of yours".to_owned();
            self.error_with_fix(
                Code::NoZeroValue,
                span,
                message,
                (Code::NoteOwnFixTake, fix),
            );
        }
    }
}
