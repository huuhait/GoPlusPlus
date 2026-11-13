//! Assignments, in the two phases of Go: first the operands of the
//! places on the left and the values on the right, from left to right;
//! then the stores, from left to right, each with its checks (EXP-3).

use gopp_sema::LocalId as CheckedLocalId;
use gopp_sema::constant::Value;
use gopp_syntax::ast::{AssignOp, BinaryOp, ExprId, ExprKind};

use super::Shared;
use super::expr::{Check, PlaceMode};
use super::func::{Built, FuncCx};
use super::map::MapOf;
use crate::{CastKind, Constant, ConstantKind, Operand, Place, Rvalue, Type, TypeId};

/// What one expression on the left of an assignment takes a value for.
enum Dest {
    /// `_`: the value is evaluated and dropped.
    Blank,
    /// A variable that the statement declares.
    New(CheckedLocalId, TypeId),
    Place(Place, Vec<Check>),
    /// `m[k]`: the place that holds the map, and the key by its address.
    Entry(Place, Vec<Check>, MapOf, Operand),
}

impl FuncCx<'_, '_> {
    pub fn assign(&mut self, op: AssignOp, lhs: &[ExprId], rhs: &[ExprId]) -> Built {
        if let (AssignOp::Op(op), [x], [y]) = (op, lhs, rhs) {
            return self.op_assign(*x, op, Some(*y));
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
                Dest::Place(place, _) => Some(self.b.place_ty(place)),
                Dest::Entry(_, _, of, _) => Some(of.value),
            })
            .collect();
        let mut values = self.values(rhs, &wanted)?;
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

    /// The store of one value, with the checks of its place.
    fn put(&mut self, dest: Dest, value: Rvalue) -> Built {
        let place = match dest {
            Dest::Blank => return Ok(()),
            Dest::New(local, _) => Place::local(self.declare(local)?),
            Dest::Place(place, checks) => {
                for check in checks {
                    self.checked(check)?;
                }
                place
            }
            Dest::Entry(map, checks, of, key) => {
                for check in checks {
                    self.checked(check)?;
                }
                self.map_entry(map, &of, key)?
            }
        };
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
            Dest::Place(place, _) => self.b.place_ty(place),
            Dest::Entry(_, _, of, _) => of.value,
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
        // An element whose copy is more than its bytes. In a Go++ file
        // the variable is only read, so it needs no copy of its own
        // (RNG-1); a Go file may write it.
        if !self.flat(wanted) && self.source.dialect() == gopp_span::Dialect::Go {
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
        if let Some((map, index)) = self.map_index(inner) {
            let Some(of) = self.map_of(map) else {
                return Ok(Dest::Blank);
            };
            let map = self.map_place(map, &mut mode)?;
            let key = self.key_at(index, &of)?;
            return Ok(Dest::Entry(map, mode.later.unwrap_or_default(), of, key));
        }
        let mut place = self.place(left, &mut mode)?;
        // A pointer that is assigned what it points to is written
        // through.
        if self.body.writes_through.contains(&inner) {
            place = self.deref(place, node.span, &mut mode)?;
        }
        Ok(Dest::Place(place, mode.later.unwrap_or_default()))
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
        if let Some((map, index)) = self.map_index(inner) {
            let Some(of) = self.map_of(map) else {
                return Ok(());
            };
            let map = self.map_place(map, &mut mode)?;
            let key = self.key_at(index, &of)?;
            let now = self.stable(Operand::Copy(map.clone()))?;
            let [first, _] = self.map_get(now, &of, key.clone())?;
            let (second, nonzero) = self.second(op, y, of.value)?;
            let value = self.arith(span, op, first, second, nonzero)?;
            let entry = self.map_entry(map, &of, key)?;
            return self.b.assign(entry, value);
        }
        let mut place = self.place(x, &mut mode)?;
        if self.body.writes_through.contains(&inner) {
            place = self.deref(place, span, &mut mode)?;
        }
        let ty = self.b.place_ty(&place);
        let mut first = Operand::Copy(place.clone());
        if later {
            first = self.stable(first)?;
        }
        let (second, nonzero) = self.second(op, y, ty)?;
        let value = self.arith(span, op, first, second, nonzero)?;
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
