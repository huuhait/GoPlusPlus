//! Assignments, in the two phases of Go: first the operands of the
//! places on the left and the values on the right, from left to right;
//! then the stores, from left to right, each with its checks (EXP-3).

use gopp_sema::LocalId as CheckedLocalId;
use gopp_sema::constant::Value;
use gopp_syntax::ast::{AssignOp, BinaryOp, ExprId, ExprKind};

use super::Shared;
use super::expr::{Check, PlaceMode};
use super::func::{Built, FuncCx};
use crate::{Constant, ConstantKind, Operand, Place, Rvalue, Type, TypeId};

/// What one expression on the left of an assignment takes a value for.
enum Dest {
    /// `_`: the value is evaluated and dropped.
    Blank,
    /// A variable that the statement declares.
    New(CheckedLocalId, TypeId),
    Place(Place, Vec<Check>),
}

impl FuncCx<'_, '_> {
    pub fn assign(&mut self, op: AssignOp, lhs: &[ExprId], rhs: &[ExprId]) -> Built {
        if let (AssignOp::Op(op), [x], [y]) = (op, lhs, rhs) {
            return self.op_assign(*x, op, Some(*y));
        }
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
        for (dest, value) in dests.into_iter().zip(values) {
            match dest {
                Dest::Blank => {}
                Dest::New(local, _) => {
                    let var = self.declare(local)?;
                    self.store(Place::local(var), value)?;
                }
                Dest::Place(place, checks) => {
                    for check in checks {
                        self.checked(check)?;
                    }
                    self.store(place, value)?;
                }
            }
        }
        Ok(())
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
        let mut place = self.place(x, &mut mode)?;
        if self.body.writes_through.contains(&inner) {
            place = self.deref(place, span, &mut mode)?;
        }
        let ty = self.b.place_ty(&place);
        let mut first = Operand::Copy(place.clone());
        if later {
            first = self.stable(first)?;
        }
        let (second, nonzero) = match y {
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
        };
        let value = self.arith(span, op, first, second, nonzero)?;
        self.b.assign(place, value)
    }

    pub fn inc_dec(&mut self, x: ExprId, inc: bool) -> Built {
        let op = match inc {
            true => BinaryOp::Add,
            false => BinaryOp::Sub,
        };
        self.op_assign(x, op, None)
    }
}
