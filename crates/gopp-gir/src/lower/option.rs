//! Options of values that have no nil of their own (OWN-P9, OWN-P10): a
//! value and a flag that says whether the option is set. An option of a
//! pointer, a handle, an interface value or a function value is that
//! value, nil when it is unset; only its methods are lowered here.

use gopp_sema::basic::Basic;
use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};
use gopp_syntax::ast::{BinaryOp, ExprId, ExprKind};

use super::expr::PlaceMode;
use super::func::{Built, FuncCx};
use crate::{AggKind, BinOp, Operand, Place, Rvalue, Type, TypeId, UnOp};

impl FuncCx<'_, '_> {
    /// What an option of the type `ty` holds, if the expression is a
    /// value of that and no option: where an option is wanted, such a
    /// value is the option that is set.
    pub(super) fn held_by(&mut self, expr: ExprId, ty: TypeId) -> Option<TypeId> {
        let Type::Option(of) = self.kind(ty) else {
            return None;
        };
        let types = self.shared.types;
        let own = self.body.types.get(&expr)?.ty;
        let option = matches!(&*types.get(types.underlying(own)), CheckedType::Option(_));
        (own != types.basic(Basic::UntypedNil) && !option).then_some(of)
    }

    /// The option of the type `ty` that is set to the value of the
    /// expression, which is of the type `of` and the option's own.
    pub(super) fn set_option(&mut self, expr: ExprId, ty: TypeId, of: TypeId) -> Built<Rvalue> {
        let value = self.rvalue_as(expr, of)?;
        let value = self.taken(expr, of, value)?;
        let value = self.operand_of(value)?;
        let made = Rvalue::Aggregate(AggKind::Of(ty), vec![value, self.b.const_bool(true)]);
        if self.flat(ty) {
            return Ok(made);
        }
        // The temporary owns what the option holds until something
        // takes the option.
        let held = self.checked_as(expr, of);
        let checked = held.map(|held| self.shared.types.option(held));
        let place = self.slot_of(made)?;
        if let Some(ends) = self.ends_of(checked, self.tree.exprs[expr].span)? {
            self.own_temp(place.clone(), ends);
        }
        Ok(Rvalue::Use(Operand::Copy(place)))
    }

    /// Whether the option at the place is set: its flag, or that it is
    /// not nil for an option that is the value itself.
    fn is_set(&mut self, option: &Place) -> Built<Operand> {
        let ty = self.b.place_ty(option);
        if let Type::Option(_) = self.kind(ty) {
            return Ok(Operand::Copy(self.b.field(option, 1)?));
        }
        let held = Operand::Copy(option.clone());
        let set = match self.is_iface(ty) || self.memo.func_types.contains(&ty) {
            true => self.nil_iface(BinaryOp::Ne, held)?,
            false => Rvalue::Binary(BinOp::Ne, held, self.zero(ty)),
        };
        self.operand_of(set)
    }

    /// `o == nil` and `o != nil` for an option with a flag.
    pub(super) fn nil_option(&mut self, op: BinaryOp, option: Operand) -> Built<Rvalue> {
        let option = match option {
            Operand::Copy(place) | Operand::Move(place) => place,
            constant => self.slot_of(Rvalue::Use(constant))?,
        };
        let set = self.is_set(&option)?;
        Ok(match op {
            BinaryOp::Ne => Rvalue::Use(set),
            _ => Rvalue::Unary(UnOp::Not, set),
        })
    }

    /// The option that a call of `o.IsNil()` or of `o.Get()` asks, what
    /// it holds, and whether the call is of `Get`.
    pub(super) fn option_asked(&self, callee: ExprId) -> Option<(ExprId, CheckedTypeId, bool)> {
        let ExprKind::Selector { x, sel } = self.tree.exprs[callee].kind else {
            return None;
        };
        let types = self.shared.types;
        let of = self.body.types.get(&x)?.ty;
        let CheckedType::Option(held) = &*types.get(types.underlying(of)) else {
            return None;
        };
        match self.shared.names.resolve(&sel.name) {
            "IsNil" => Some((x, *held, false)),
            "Get" => Some((x, *held, true)),
            _ => None,
        }
    }

    /// `o.IsNil()`: whether the option is unset. `o.Get()`: a copy of
    /// what the option holds and whether it is set; the copy of an
    /// option that is unset is all zero, and nothing reads it
    /// (OWN-P10).
    pub(super) fn option_call(
        &mut self,
        expr: ExprId,
        (option, held, get): (ExprId, CheckedTypeId, bool),
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let place = self.place(option, &mut PlaceMode::default())?;
        let set = self.is_set(&place)?;
        if !get {
            return Ok(Rvalue::Unary(UnOp::Not, set));
        }
        let value = match self.kind(self.b.place_ty(&place)) {
            Type::Option(_) => self.b.field(&place, 0)?,
            _ => place,
        };
        let of = self.b.place_ty(&value);
        let value = Rvalue::Use(Operand::Copy(value));
        if self.flat(of) {
            let value = self.operand_of(value)?;
            return Ok(Rvalue::Aggregate(AggKind::Tuple, vec![value, set]));
        }
        // The copy is the caller's own, as a result of a call is.
        let value = self.cloned_value(span, Some(held), value)?;
        let value = self.operand_of(value)?;
        let both = self.slot_of(Rvalue::Aggregate(AggKind::Tuple, vec![value, set]))?;
        if let Some(ends) = self.ends_of(Some(held), span)? {
            let copy = self.b.field(&both, 0)?;
            self.own_temp(copy, ends);
        }
        Ok(Rvalue::Use(Operand::Copy(both)))
    }
}
