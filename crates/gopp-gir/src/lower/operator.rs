//! `@Operator` methods (EXP-16 to EXP-20): an expression that the
//! checker found a method for is a call of the method, with the operand
//! that has the method as its receiver.

use gopp_sema::OperatorUse;
use gopp_sema::types::{PARENT, Type as CheckedType};
use gopp_syntax::ast::{BinaryOp, ExprId, ExprKind};

use super::CalleeKey;
use super::func::{Built, FuncCx};
use crate::{BinOp, Rvalue, TypeId, UnOp};

impl FuncCx<'_, '_> {
    /// The call of the method `used` that stands for `expr` or for a
    /// part of it, with the receiver and the other operands.
    pub(super) fn operator_call(
        &mut self,
        expr: ExprId,
        used: &OperatorUse,
        receiver: ExprId,
        args: &[ExprId],
    ) -> Built<Result<Rvalue, &'static str>> {
        let types = self.shared.types;
        // A `@Convert` method is found by its name alone, and so is the
        // method that a constraint lists: of the `@Operator` methods of
        // that name which the type has, it is the one of that type.
        let listed = match used.method {
            Some(_) => None,
            None => {
                let mut all = types.operators(used.holder, used.name).into_iter();
                all.find(|operator| types.identical(operator.sig, used.sig))
            }
        };
        let holder = listed.as_ref().map_or(used.holder, |listed| listed.holder);
        let key = match (used.method, &listed) {
            (Some(which), _) => CalleeKey::Operator(holder, which),
            (None, Some(listed)) => CalleeKey::Operator(holder, listed.at),
            (None, None) => CalleeKey::Method(holder, used.name),
        };
        let called = match self.callee(key)? {
            Ok(called) => called,
            Err(_) => {
                return Ok(Err(
                    "an operator method of a generic type or a type parameter",
                ));
            }
        };
        // A method of an ancestor has the part of that parent as its
        // receiver (EXP-30).
        let mut own = self.body.types.get(&receiver).map(|typed| typed.ty);
        while let Some(
            CheckedType::Pointer(to) | CheckedType::Unique(to) | CheckedType::Shared(to),
        ) = own.map(|own| types.get(types.underlying(own))).as_deref()
        {
            own = Some(*to);
        }
        let way = own.and_then(|own| types.ancestor_path(own, holder));
        let way: Vec<u32> = way.unwrap_or_default();
        let way: Vec<u32> = way.iter().map(|step| step | PARENT).collect();
        let holder = (!way.is_empty()).then_some(holder);
        self.method_call(expr, called, Some((receiver, &way)), holder, args)
            .map(Ok)
    }

    /// `x[i] = v` by a `SetIndex` method: a call with the indices and
    /// then the value.
    pub(super) fn operator_store(
        &mut self,
        place: ExprId,
        used: &OperatorUse,
        value: ExprId,
    ) -> Built {
        let span = self.tree.exprs[place].span;
        let ExprKind::Index { x, indices } = &self.tree.exprs[place].kind else {
            self.unsupported(span, "an operator method");
            return Ok(());
        };
        let mut args = indices.clone();
        args.push(value);
        if let Err(what) = self.operator_call(place, used, *x, &args)? {
            self.unsupported(span, what);
        }
        Ok(())
    }

    /// Whether the tag of a switch is equal to a case by an `Eq`
    /// method. The tag is read again for each case: one that does more
    /// than read was worked out before, by the switch.
    pub(super) fn operator_case(
        &mut self,
        tag: ExprId,
        case: ExprId,
        used: &OperatorUse,
    ) -> Built<Result<Rvalue, &'static str>> {
        if self.has_effects(tag) {
            return Ok(Err("a tag that does more than read, compared by a method"));
        }
        let (receiver, other) = match used.swapped {
            true => (case, tag),
            false => (tag, case),
        };
        self.operator_call(case, used, receiver, &[other])
    }

    /// The function that `for ... := range x` walks by the `Iter`
    /// method of `x` (RNG-6b): the call of the method, whose value
    /// has the type `given`.
    pub(super) fn operator_iter(
        &mut self,
        x: ExprId,
        used: &OperatorUse,
        given: gopp_sema::types::TypeId,
    ) -> Built<Result<Rvalue, &'static str>> {
        let outer = self.iterated.replace((x, given));
        let made = self.operator_call(x, used, x, &[]);
        self.iterated = outer;
        made
    }

    /// The type of what the `Iter` method gives.
    pub(super) fn iter_given(&self, used: &OperatorUse) -> Option<gopp_sema::types::TypeId> {
        match &*self.shared.types.get(used.sig) {
            CheckedType::Func { results, .. } => results.first().copied(),
            _ => None,
        }
    }

    /// The value of an expression that an `@Operator` method stands
    /// for: `a + b`, `-a`, `a == b`, `a < b`, and a call of such a
    /// method by its name. `a != b` is not what `Eq` says, and an order
    /// is what `Cmp` says against zero (EXP-20).
    pub(super) fn operator(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let Some(used) = self.body.operators.get(&expr).cloned() else {
            return Ok(self.placeholder(span, ty, "an operator method"));
        };
        let made = match &self.tree.exprs[expr].kind {
            &ExprKind::Binary { op, x, y } => {
                // The receiver of a `@Right` method is on the right.
                let (receiver, other) = match used.swapped {
                    true => (y, x),
                    false => (x, y),
                };
                // The left operand is worked out first, also where it
                // is the argument of a method of the right one.
                let first = used.swapped && self.has_effects(x);
                if first {
                    self.work_out(x, false)?;
                }
                let said = self.operator_call(expr, &used, receiver, &[other])?;
                if first {
                    self.forget(&[super::Shared::unparen(self.tree, x)]);
                }
                let said = match said {
                    Ok(said) => said,
                    Err(what) => return Ok(self.placeholder(span, ty, what)),
                };
                let order = match (op, used.swapped) {
                    (BinaryOp::Ne, _) => {
                        let said = self.operand_of(said)?;
                        return Ok(Rvalue::Unary(UnOp::Not, said));
                    }
                    (BinaryOp::Lt, false) | (BinaryOp::Gt, true) => BinOp::Lt,
                    (BinaryOp::Le, false) | (BinaryOp::Ge, true) => BinOp::Le,
                    (BinaryOp::Gt, false) | (BinaryOp::Lt, true) => BinOp::Gt,
                    (BinaryOp::Ge, false) | (BinaryOp::Le, true) => BinOp::Ge,
                    _ => return Ok(said),
                };
                let said = self.operand_of(said)?;
                let zero = self.zero(self.b.operand_ty(&said));
                return Ok(Rvalue::Binary(order, said, zero));
            }
            &ExprKind::Unary { x, .. } => self.operator_call(expr, &used, x, &[])?,
            ExprKind::Call { fun, args, .. } => {
                let callee = super::Shared::unparen(self.tree, *fun);
                match self.tree.exprs[callee].kind {
                    ExprKind::Selector { x, .. } if self.body.types.contains_key(&callee) => {
                        self.operator_call(expr, &used, x, args)?
                    }
                    // `T(a)` by a `@Convert` method of `a`.
                    _ => match args[..] {
                        [value] => self.operator_call(expr, &used, value, &[])?,
                        _ => Err("a conversion by a method"),
                    },
                }
            }
            ExprKind::Index { x, indices } => self.operator_call(expr, &used, *x, indices)?,
            _ => Err("an operator method"),
        };
        match made {
            Ok(made) => Ok(made),
            Err(what) => Ok(self.placeholder(span, ty, what)),
        }
    }
}
