//! Function values (CLO-1 to CLO-9, gir.md §15): two words, the code and
//! the environment that the code is given first. The code of a function
//! literal is a function of its own; the code of a function of a package
//! that is used as a value is the function with an environment in front,
//! which the package makes once. A call through a function value knows
//! nothing of the function that runs, so it gives every argument away
//! (PAR-10).

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::ObjKind;
use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};
use gopp_span::{Dialect, Span as SourceSpan};
use gopp_syntax::ast::ExprId;

use super::expr::{Called, Check, Code, Target};
use super::func::{Built, FuncCx};
use super::{CalleeKey, Made};
use crate::{
    AggKind, Constant, ConstantKind, FuncId, Operand, Place, Rvalue, Type, TypeId, TypeInterner,
    UnwindAction,
};

impl FuncCx<'_, '_> {
    /// The type of the code of a function value of the type: that of
    /// its first word.
    fn code_type(&mut self, ty: TypeId) -> Option<TypeId> {
        match self.kind(ty) {
            Type::Tuple(parts) if self.memo.func_types.contains(&ty) => parts.first().copied(),
            _ => None,
        }
    }

    /// The function value of the code, which has no environment.
    fn bare(&mut self, code: FuncId, code_ty: TypeId) -> Rvalue {
        let bytes = self.bytes_ty();
        let code = Operand::Const(Constant {
            kind: ConstantKind::Func(code),
            ty: code_ty,
        });
        Rvalue::Aggregate(AggKind::Tuple, vec![code, self.zero(bytes)])
    }

    /// A function of a package as a value, for a place of the type
    /// `ty`: its code is the function with an environment in front,
    /// which the package makes once.
    pub fn func_value(&mut self, expr: ExprId, ty: TypeId, key: CalleeKey) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let shared = self.shared;
        let named = match key {
            CalleeKey::Object(object) => Some((shared.package, object)),
            CalleeKey::Member(import, member) => match &shared.package.object(import).kind {
                ObjKind::Import { package, .. } => Some((&**package, member)),
                _ => None,
            },
            CalleeKey::Method(..) => None,
        };
        let (Some((package, object)), Some(code_ty)) = (named, self.code_type(ty)) else {
            return Ok(self.placeholder(span, ty, "a function as a value"));
        };
        let called = match self.callee(key)? {
            Ok(called) => called,
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
        let entity = Entity::FuncValue(package, object);
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            return Ok(self.placeholder(span, ty, "a function without a symbol as a value"));
        };
        let asked = self.memo.funcs.contains_key(&symbol);
        let code = self.function_named(&symbol, code_ty)?;
        if !asked {
            let of = self.b.module().funcs[called.func].name().to_owned();
            let checked = package.object(object).ty;
            self.memo.glues.push((symbol, Made::Value(of), checked));
        }
        Ok(self.bare(code, code_ty))
    }

    /// A function literal, for a place of the type `ty`: its code is a
    /// function of its own, which is lowered after this one.
    pub fn literal(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        if self
            .body
            .captures
            .get(&expr)
            .is_some_and(|all| !all.is_empty())
        {
            let what = "a function literal that uses a variable of the function around it";
            return Ok(self.placeholder(span, ty, what));
        }
        let named = self.memo.literal(self.shared, self.file(), expr);
        let (Some((symbol, job)), Some(code_ty)) = (named, self.code_type(ty)) else {
            return Ok(self.placeholder(span, ty, "a function literal"));
        };
        let code = self.function_named(&symbol, code_ty)?;
        self.memo.queue(job, expr);
        Ok(self.bare(code, code_ty))
    }

    /// What a call of a function value calls, and the first argument of
    /// the call, its environment. None for what is no function value.
    pub fn value_called(
        &mut self,
        span: SourceSpan,
        callee: ExprId,
        spread: bool,
    ) -> Built<Option<Target>> {
        let types = self.shared.types;
        let Some(typed) = self.body.types.get(&callee) else {
            return Ok(None);
        };
        let checked = types.get(types.underlying(typed.ty));
        let declared = match &*checked {
            CheckedType::Func { variadic: true, .. } => {
                return Ok(Some(Err("a call of a function with `...`")));
            }
            CheckedType::Func { params, .. } => params.len(),
            _ => return Ok(None),
        };
        if spread {
            return Ok(Some(Err("a call with `...`")));
        }
        let ty = self.ty_of(callee);
        let Some(code_ty) = self.code_type(ty) else {
            return Ok(None);
        };
        let Type::Func(sig) = self.kind(code_ty) else {
            return Ok(None);
        };
        // The value is worked out before the arguments, as in Go.
        let value = self.operand_as(callee, ty)?;
        let value = match self.stable(value)? {
            Operand::Copy(place) | Operand::Move(place) => place,
            constant => self.slot_of(Rvalue::Use(constant))?,
        };
        let code = Operand::Copy(self.b.field(&value, 0)?);
        let code = Operand::Copy(self.slot_of(Rvalue::Use(code))?);
        if self.source.dialect() == Dialect::Go {
            self.checked(Check::Nil(code.clone(), span))?;
        }
        let env = Operand::Copy(self.b.field(&value, 1)?);
        let called = Called {
            code: Code::Known(code),
            views: sig.params.len().saturating_sub(declared + 1),
            lent: std::rc::Rc::from([]),
            worked: std::rc::Rc::from([]),
            params: sig.params.iter().copied().collect(),
            ret: sig.ret,
        };
        Ok(Some(Ok((called, vec![env]))))
    }

    /// The body of the code of the function with the symbol `of` and
    /// the type `ty` as a value: the function is called with what
    /// follows the environment, and what the function only reads of it
    /// ends here, because a call through a value gave it away.
    pub fn value_body(&mut self, of: &str, ty: CheckedTypeId) -> Built<Result<(), &'static str>> {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let sig = match self.memo.signature(shared, self.b.types(), None, ty) {
            Ok(sig) => sig,
            Err(what) => return Ok(Err(what)),
        };
        let Type::Func(plain) = self.kind(sig) else {
            return Ok(Err("a function type"));
        };
        let function = self.function_named(of, sig)?;
        let args = (0..plain.params.len())
            .map(|at| Operand::Copy(Place::local(self.b.param(at as u32 + 1))))
            .collect();
        let callee = Operand::Const(Constant {
            kind: ConstantKind::Func(function),
            ty: sig,
        });
        let result = Place::local(self.b.temp(plain.ret));
        let next = self.b.new_block();
        let unwind = UnwindAction::Continue;
        self.b
            .call(callee, args, result.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        let lent = shared.lent.get(of);
        if let (Some(lent), CheckedType::Func { params, .. }) = (lent, &*shared.types.get(ty)) {
            for (at, &param) in params.iter().enumerate() {
                let read = lent.get(at) == Some(&true);
                let Some(&lowered) = plain.params.get(at).filter(|_| read) else {
                    continue;
                };
                if self.flat(lowered) {
                    continue;
                }
                match self.ends_quietly(param)? {
                    Ok(ends) => self.end(Place::local(self.b.param(at as u32 + 1)), ends)?,
                    Err(what) => return Ok(Err(what)),
                }
            }
        }
        if plain.ret != TypeInterner::UNIT {
            let whole = Place::local(self.b.result());
            self.b.assign(whole, Rvalue::Use(Operand::Copy(result)))?;
        }
        self.b.ret()?;
        Ok(Ok(()))
    }
}
