//! Assignments, in the two phases of Go: first the operands of the
//! places on the left and the values on the right, from left to right;
//! then the stores, from left to right, each with its checks (EXP-3).

use gopp_sema::LocalId as CheckedLocalId;
use gopp_sema::constant::Value;
use gopp_syntax::ast::{AssignOp, BinaryOp, ExprId, ExprKind};

use super::Shared;
use super::expr::{Check, PlaceMode};
use super::func::{Built, FuncCx};
use super::glue::Ends;
use super::map::Way;
use crate::{CastKind, Constant, ConstantKind, Mutability, Operand, Place, Rvalue, Type, TypeId};

/// What one expression on the left of an assignment takes a value for.
enum Dest {
    /// `_`: the value is evaluated and dropped.
    Blank,
    /// A variable that the statement declares.
    New(CheckedLocalId, TypeId),
    /// A place, the checks that it needs before it is written, and the
    /// drop glue for the value that it has, if that holds what is freed.
    Place(Place, Vec<Check>, Option<Ends>),
    /// `m[k]`, `m[a][b]`: the place that holds the outermost map, and
    /// each map on the way with its key by its address.
    Entry(Place, Vec<Check>, Way),
}

impl FuncCx<'_, '_> {
    pub fn assign(&mut self, op: AssignOp, lhs: &[ExprId], rhs: &[ExprId]) -> Built {
        if let (AssignOp::Op(op), [x], [y]) = (op, lhs, rhs) {
            return self.op_assign(*x, op, Some(*y));
        }
        let unparen = |expr: &ExprId| Shared::unparen(self.tree, *expr);
        if lhs.len() > 1
            && lhs
                .iter()
                .all(|left| self.body.swaps.contains(&unparen(left)))
        {
            return self.exchange(lhs, rhs);
        }
        // `s = append(s, ...)` and `s = s[i:j]` work on `s` where it is:
        // the slice on the right is not a copy.
        self.reused = match (op, lhs, rhs) {
            (AssignOp::Assign, [left], [right]) => self.reused_in(*left, *right),
            _ => None,
        };
        // What is evaluated after a place is worked out may change the
        // variables that it was reached through.
        let mut later = lhs.len() > 1;
        for &value in rhs {
            later |= self.has_effects(value);
        }
        let mut dests = Vec::with_capacity(lhs.len());
        for &left in lhs {
            dests.push(self.dest(left, op == AssignOp::Define, later)?);
        }
        let wanted: Vec<Option<TypeId>> = dests
            .iter()
            .map(|dest| match dest {
                Dest::Blank => None,
                Dest::New(_, ty) => Some(*ty),
                Dest::Place(place, ..) => Some(self.b.place_ty(place)),
                Dest::Entry(_, _, way) => way.last().map(|(of, _)| of.value),
            })
            .collect();
        // Each place is the new owner of its value.
        let mut values = self.owned_values(rhs, &wanted)?;
        // A store may change what a value that is stored after it
        // reads: `a, b = b, a`.
        if values.len() > 1 {
            let mut held = Vec::with_capacity(values.len());
            for (dest, value) in dests.iter().zip(values) {
                held.push(match dest {
                    Dest::Blank => value,
                    _ => {
                        let read = self.operand_of(value)?;
                        Rvalue::Use(self.stable(read)?)
                    }
                });
            }
            values = held;
        }
        self.reused = None;
        for (dest, value) in dests.into_iter().zip(values) {
            self.put(dest, value)?;
        }
        Ok(())
    }

    /// `a, b = b, a`: the places exchange what they have (OWN-R4). No
    /// value is made and none ends: the bytes of all are read first and
    /// stored as they are. With two places that is right also where
    /// the two are one, as in `xs[i], xs[j] = xs[j], xs[i]` with equal
    /// indexes. With three or more of which two may be one, a value
    /// could be lost and another kept twice: each place gets a copy
    /// then, as in any other assignment, and so does an entry of a map.
    fn exchange(&mut self, lhs: &[ExprId], rhs: &[ExprId]) -> Built {
        let mut dests = Vec::with_capacity(lhs.len());
        for &left in lhs {
            dests.push(self.dest(left, false, true)?);
        }
        let wanted: Vec<Option<TypeId>> = dests
            .iter()
            .map(|dest| match dest {
                Dest::Place(place, ..) => Some(self.b.place_ty(place)),
                Dest::Entry(_, _, way) => way.last().map(|(of, _)| of.value),
                Dest::Blank | Dest::New(..) => None,
            })
            .collect();
        let places = dests.iter().all(|dest| matches!(dest, Dest::Place(..)));
        let as_they_are = places && (lhs.len() == 2 || self.apart(lhs));
        let given: Vec<ExprId> = (rhs.iter())
            .map(|&value| Shared::unparen(self.tree, value))
            .collect();
        // A copy for each place is one that the types must have.
        if !as_they_are {
            let what = match places {
                true => "this value for an exchange of places that may be one place",
                false => "this value for an exchange with an entry of a map",
            };
            for &value in rhs {
                let span = self.tree.exprs[value].span;
                let ty = self.body.types.get(&value).map(|typed| typed.ty);
                if ty.is_some_and(|ty| self.uncopied(span, ty, what, None)) {
                    return Ok(());
                }
            }
        }
        // The marks of the checker are set aside while the values are
        // read: nothing is moved out of a place here.
        let aside = match as_they_are {
            true => &mut self.lent,
            false => &mut self.copied,
        };
        aside.extend(&given);
        let values = match as_they_are {
            true => self.values(rhs, &wanted),
            false => self.owned_values(rhs, &wanted),
        };
        for value in &given {
            self.lent.remove(value);
            self.copied.remove(value);
        }
        let values = values?;
        // Every value is read before any is stored.
        let mut held = Vec::with_capacity(values.len());
        for value in values {
            let read = self.operand_of(value)?;
            held.push(Rvalue::Use(self.stable(read)?));
        }
        for (dest, value) in dests.into_iter().zip(held) {
            match (as_they_are, dest) {
                (true, Dest::Place(place, checks, _)) => {
                    for check in checks {
                        self.checked(check)?;
                    }
                    self.store(place, value)?;
                }
                (_, dest) => self.put(dest, value)?,
            }
        }
        Ok(())
    }

    /// Whether the places are variables of the function, each another
    /// one: no two of them are one place.
    fn apart(&self, places: &[ExprId]) -> bool {
        places.iter().all(|&place| {
            let inner = Shared::unparen(self.tree, place);
            let direct = matches!(self.tree.exprs[inner].kind, ExprKind::Name(_))
                && !self.body.writes_through.contains(&inner);
            let plain = matches!(self.denoted(inner), Some(gopp_sema::Ref::Local(local))
                if self.plain_var(*local));
            direct && plain
        })
    }

    /// The store of one value, with the checks of its place.
    fn put(&mut self, dest: Dest, value: Rvalue) -> Built {
        let place = match dest {
            Dest::Blank => return Ok(()),
            Dest::New(local, _) => {
                let var = self.declare(local)?;
                (self.var_place(var)?, None)
            }
            Dest::Place(place, checks, old) => {
                for check in checks {
                    self.checked(check)?;
                }
                (place, old)
            }
            Dest::Entry(map, checks, way) => {
                for check in checks {
                    self.checked(check)?;
                }
                let old = way.last().and_then(|(of, _)| of.drop);
                (self.entry_at(map, &way)?, old.map(Ends::Glue))
            }
        };
        // The value that the place has ends before the new one is
        // there.
        let (place, old) = place;
        if let Some(ends) = old {
            self.end(place.clone(), ends)?;
        }
        self.store(place, value)
    }

    /// The expression inside `right` that names the place `left` again
    /// and whose slice `right` takes over: the first argument of
    /// `append`, or what is sliced.
    fn reused_in(&self, left: ExprId, right: ExprId) -> Option<ExprId> {
        let right = Shared::unparen(self.tree, right);
        let again = match &self.tree.exprs[right].kind {
            ExprKind::Slice { x, .. } => *x,
            ExprKind::Call { fun, args, .. } => {
                let fun = Shared::unparen(self.tree, *fun);
                let append = matches!(
                    self.body.uses.get(&fun),
                    Some(gopp_sema::Ref::Universe(gopp_sema::Predeclared::Append))
                );
                match (append, args.first()) {
                    (true, Some(&first)) => first,
                    _ => return None,
                }
            }
            _ => return None,
        };
        let again = Shared::unparen(self.tree, again);
        self.same_place(left, again).then_some(again)
    }

    /// Gives a variable of a range clause its value for one pass of the
    /// loop: a new variable of the pass with `define`, a place that is
    /// assigned to without it.
    pub fn set_range_var(&mut self, left: ExprId, define: bool, value: Operand) -> Built {
        let dest = self.dest(left, define, false)?;
        let wanted = match &dest {
            Dest::Blank => return Ok(()),
            Dest::New(_, ty) => *ty,
            Dest::Place(place, ..) => self.b.place_ty(place),
            Dest::Entry(_, _, way) => match way.last() {
                Some((of, _)) => of.value,
                None => return Ok(()),
            },
        };
        // The loop counts in a type of its own.
        let have = self.b.operand_ty(&value);
        let integers = matches!(
            (self.kind(have), self.kind(wanted)),
            (Type::Int { .. }, Type::Int { .. })
        );
        let mut value = match have != wanted && integers {
            true => Rvalue::Cast(CastKind::IntToInt, value, wanted),
            false => Rvalue::Use(value),
        };
        // An element whose copy is more than its bytes. A new variable
        // is only read: it is the element as it is, and owns nothing
        // (RNG-1). A place that is assigned to owns what it gets: it has
        // a copy.
        // A variable that a function literal uses has a copy, in a
        // cell.
        let view = matches!(&dest, Dest::New(local, _) if !self.celled(*local));
        if let (Dest::New(local, _), true, false) = (&dest, view, self.flat(wanted)) {
            let var = self.declare_view(*local)?;
            return self.store(Place::local(var), value);
        }
        if !self.flat(wanted) {
            let checked = match &dest {
                Dest::New(local, _) => Some(self.body.locals[local.0 as usize].ty),
                _ => self.checked_as(left, wanted),
            };
            value = self.cloned_value(self.tree.exprs[left].span, checked, value)?;
        }
        self.put(dest, value)
    }

    /// `&v` in a range clause: for one pass of the loop `v` is another
    /// name of the element, which is not copied (RNG-1a).
    pub fn set_range_alias(&mut self, named: ExprId, element: Place) -> Built {
        let named = &self.tree.exprs[Shared::unparen(self.tree, named)];
        let Some(&local) = self.body.defs.get(&named.span.lo) else {
            return Ok(());
        };
        let var = self.declare_alias(local, self.b.place_ty(&element))?;
        let address = Rvalue::AddrOf(element, crate::Mutability::Mut);
        self.b.assign(Place::local(var), address)
    }

    /// What `left` takes its value for. With `later`, more is evaluated
    /// before the store.
    fn dest(&mut self, left: ExprId, define: bool, later: bool) -> Built<Dest> {
        let inner = Shared::unparen(self.tree, left);
        let node = &self.tree.exprs[inner];
        if let ExprKind::Name(name) = node.kind {
            if name == self.shared.blank {
                return Ok(Dest::Blank);
            }
            if let (true, Some(&local)) = (define, self.body.defs.get(&node.span.lo)) {
                let declared = &self.body.locals[local.0 as usize];
                let ty = self.lower(declared.ty, declared.span);
                return Ok(Dest::New(local, ty));
            }
        }
        if self.body.index_stores.contains_key(&inner) {
            self.unsupported(node.span, "an operator method");
            return Ok(Dest::Blank);
        }
        let mut mode = PlaceMode {
            later: Some(Vec::new()),
            held: later,
        };
        if self.map_index(inner).is_some() {
            let Some((map, way)) = self.entries(inner, &mut mode)? else {
                return Ok(Dest::Blank);
            };
            return Ok(Dest::Entry(map, mode.later.unwrap_or_default(), way));
        }
        let mut place = self.place(left, &mut mode)?;
        // A pointer that is assigned what it points to is written
        // through.
        if self.body.writes_through.contains(&inner) {
            place = self.deref(place, &mut mode)?;
        }
        // `s = append(s, ...)` and `s = s[i:j]` keep what `s` has.
        let ty = self.b.place_ty(&place);
        let old = match self.flat(ty) || self.reused.is_some() {
            true => None,
            false => {
                let checked = self.checked_as(left, ty);
                self.ends_of(checked, node.span)?
            }
        };
        Ok(Dest::Place(place, mode.later.unwrap_or_default(), old))
    }

    /// `x op= y`; `x++` and `x--` without a `y`.
    fn op_assign(&mut self, x: ExprId, op: BinaryOp, y: Option<ExprId>) -> Built {
        let inner = Shared::unparen(self.tree, x);
        let span = self.tree.exprs[inner].span;
        if self.body.op_assigns.contains_key(&inner) || self.body.index_stores.contains_key(&inner)
        {
            self.unsupported(span, "an operator method");
            return Ok(());
        }
        let later = y.is_some_and(|y| self.has_effects(y));
        let mut mode = PlaceMode {
            later: None,
            held: later,
        };
        // An entry of a map is read, and then written where the map has
        // it after the value is worked out.
        if self.map_index(inner).is_some() {
            let Some((map, way)) = self.entries(inner, &mut mode)? else {
                return Ok(());
            };
            let Some(&(last, _)) = way.last() else {
                return Ok(());
            };
            // The value is read through the maps as they are.
            let mut first = self.stable(Operand::Copy(map.clone()))?;
            for (of, key) in &way {
                [first, _] = self.map_get(first, of, key.clone())?;
            }
            let (second, nonzero) = self.second(op, y, last.value)?;
            let value = self.arith(span, op, first, second, nonzero)?;
            let value = self.stored(value)?;
            let entry = self.entry_at(map, &way)?;
            return self.put_over(entry, value);
        }
        let mut place = self.place(x, &mut mode)?;
        if self.body.writes_through.contains(&inner) {
            place = self.deref(place, &mut mode)?;
        }
        let ty = self.b.place_ty(&place);
        // `s += x` grows the string where it is, if its bytes are its
        // own and have room behind them (STM-8).
        if let (Type::String, BinaryOp::Add) = (self.kind(ty), op) {
            let (more, _) = self.second(op, y, ty)?;
            self.empty_moved()?;
            let at = self.b.assign_temp(Rvalue::AddrOf(place, Mutability::Mut))?;
            let params = [self.b.types().ptr(ty), ty];
            let args = vec![Operand::Copy(Place::local(at)), more];
            return self.runtime_call("_gopp_string_append", &params, args);
        }
        let mut first = Operand::Copy(place.clone());
        if later {
            first = self.stable(first)?;
        }
        let (second, nonzero) = self.second(op, y, ty)?;
        let value = self.arith(span, op, first, second, nonzero)?;
        let value = self.stored(value)?;
        self.put_over(place, value)
    }

    /// The value of `x op= y` before it is stored: a string that was
    /// joined is in a slot, since the old one ends first.
    fn stored(&mut self, value: Rvalue) -> Built<Rvalue> {
        let ty = self.b.rvalue_ty(&value)?;
        match self.flat(ty) {
            true => Ok(value),
            false => Ok(Rvalue::Use(self.operand_of(value)?)),
        }
    }

    /// `place = value` for `x op= y`: a string that the place has ends,
    /// after the new one was made of it.
    fn put_over(&mut self, place: Place, value: Rvalue) -> Built {
        self.empty_moved()?;
        if let Type::String = self.kind(self.b.place_ty(&place)) {
            self.end(place.clone(), Ends::Text)?;
        }
        self.b.assign(place, value)
    }

    /// The right operand of `x op= y` for an `x` of the type `ty`, and
    /// whether it is known not to be zero; one for `x++` and `x--`.
    fn second(&mut self, op: BinaryOp, y: Option<ExprId>, ty: TypeId) -> Built<(Operand, bool)> {
        Ok(match y {
            Some(y) => {
                // The count of a shift has a type of its own.
                let shift = matches!(op, BinaryOp::Shl | BinaryOp::Shr);
                let known = self.body.types.get(&y);
                let nonzero = known
                    .is_some_and(|typed| typed.value != Value::Unknown && typed.value.sign() != 0);
                let second = match shift {
                    true => {
                        let constant = known.is_some_and(|typed| typed.value != Value::Unknown);
                        let counted = match constant {
                            true => super::Memo::word(self.shared, self.b.types(), false),
                            false => self.ty_of(y),
                        };
                        self.operand_as(y, counted)?
                    }
                    false => self.operand_as(y, ty)?,
                };
                (second, nonzero)
            }
            None => {
                let one = match self.kind(ty) {
                    Type::Float { bits: 32 } => ConstantKind::Float(u64::from(1f32.to_bits())),
                    Type::Float { .. } => ConstantKind::Float(1f64.to_bits()),
                    _ => ConstantKind::Int(1),
                };
                (Operand::Const(Constant { kind: one, ty }), true)
            }
        })
    }

    pub fn inc_dec(&mut self, x: ExprId, inc: bool) -> Built {
        let op = match inc {
            true => BinaryOp::Add,
            false => BinaryOp::Sub,
        };
        self.op_assign(x, op, None)
    }
}
