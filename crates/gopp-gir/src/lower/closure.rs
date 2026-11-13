//! Function values (CLO-1 to CLO-9, gir.md §15): two words, the code and
//! the environment that the code is given first. The code of a function
//! literal is a function of its own; the code of a function of a package
//! that is used as a value is the function with an environment in front,
//! which the package makes once. A call through a function value knows
//! nothing of the function that runs, so it gives every argument away
//! (PAR-10).

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};
use gopp_sema::{LocalId as CheckedLocalId, MethodValue, ObjKind, SelectionKind};
use gopp_span::{Dialect, Span as SourceSpan};
use gopp_syntax::ast::{ExprId, ExprKind};
use lasso::Spur;

use super::expr::{Called, Check, Code, PlaceMode, Target};
use super::func::{Built, FuncCx};
use super::glue::{Ends, Glue};
use super::{CalleeKey, Made, Shared};
use crate::{
    AggKind, CastKind, Constant, ConstantKind, FuncId, Operand, Place, Rvalue, Type, TypeId,
    TypeInterner,
};

/// Where the cells of an environment start: after its count and the
/// number of its cells. Each cell has two words, its address and what
/// ends its variable.
const ENV_HEAD: u32 = 2;

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

    /// The type of the environment of a function literal that uses
    /// the variables: the count, the number of cells, and for each
    /// variable the address of its cell and what ends the variable.
    fn env_type(&mut self, captured: &[CheckedLocalId]) -> TypeId {
        let word = self.b.types().int(64, false);
        let drop = self.glue_sig(Glue::Drop);
        let mut fields = vec![word, word];
        for &local in captured {
            let declared = &self.body.locals[local.0 as usize];
            let of = self.lower(declared.ty, declared.span);
            let cell = self.cell_type(of);
            fields.extend([self.b.types().ptr(cell), drop]);
        }
        self.b.types().tuple(&fields)
    }

    /// A function literal, for a place of the type `ty`: its code is a
    /// function of its own, which is lowered after this one. A literal
    /// that uses variables of the functions around it has a new
    /// environment with the cells of those variables, each of which one
    /// more shares (CLO-2).
    pub fn literal(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let named = self.memo.literal(self.shared, self.file(), expr);
        let (Some((symbol, job)), Some(code_ty)) = (named, self.code_type(ty)) else {
            return Ok(self.placeholder(span, ty, "a function literal"));
        };
        let captures = self.body.captures.get(&expr).map(Vec::as_slice);
        let captured: Vec<CheckedLocalId> = (captures.unwrap_or_default().iter())
            .map(|capture| capture.local)
            .collect();
        let code = self.function_named(&symbol, code_ty)?;
        self.memo.queue(job, expr);
        if captured.is_empty() {
            return Ok(self.bare(code, code_ty));
        }
        // A literal whose value cannot outlive its variables has an
        // environment in this frame, with the address of each variable
        // where it is, or of its cell (CLO-1).
        if self.framed(expr) {
            let word = self.b.types().int(64, false);
            let mut parts = vec![self.b.const_int(word, u64::MAX)?];
            for &local in &captured {
                let slot = match self.cells.get(&local) {
                    Some(&cell) => cell,
                    None => {
                        let var = self.var(local)?;
                        let place = self.var_place(var)?;
                        let address = Rvalue::AddrOf(place, crate::Mutability::Mut);
                        self.b.assign_temp(address)?
                    }
                };
                parts.push(Operand::Copy(Place::local(slot)));
            }
            let env = self.slot_of(Rvalue::Aggregate(AggKind::Tuple, parts))?;
            let env = self.record(env)?;
            let code = Operand::Const(Constant {
                kind: ConstantKind::Func(code),
                ty: code_ty,
            });
            return Ok(Rvalue::Aggregate(AggKind::Tuple, vec![code, env]));
        }
        // Every variable that it uses has its cell here.
        let mut cells = Vec::with_capacity(captured.len());
        for local in &captured {
            match self.cells.get(local) {
                Some(&cell) => cells.push(cell),
                None => {
                    let what = "a function literal that uses a variable that has no cell";
                    return Ok(self.placeholder(span, ty, what));
                }
            }
        }
        let bytes = self.bytes_ty();
        let word = self.b.types().int(64, false);
        let env_ty = self.env_type(&captured);
        let count = self.b.const_int(word, captured.len() as u64)?;
        let made = self.runtime("_gopp_env_new", &[word], bytes, vec![count])?;
        let to = self.b.types().ptr(env_ty);
        let env = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, made.clone(), to))?;
        let env = self.b.deref(&Place::local(env))?;
        for (at, (&cell, &local)) in cells.iter().zip(&captured).enumerate() {
            let shared = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(Place::local(cell)), bytes);
            let shared = Operand::Copy(Place::local(self.b.assign_temp(shared)?));
            self.runtime_call("_gopp_cell_retain", &[bytes], vec![shared])?;
            let at = ENV_HEAD + 2 * at as u32;
            let slot = self.b.field(&env, at)?;
            self.b
                .assign(slot, Rvalue::Use(Operand::Copy(Place::local(cell))))?;
            let declared = &self.body.locals[local.0 as usize];
            let (checked, declared_at) = (declared.ty, declared.span);
            let of = self.lower(checked, declared_at);
            if !self.flat(of)
                && let Some(drop) = self.drop_glue(Some(checked), span)?
            {
                let slot = self.b.field(&env, at + 1)?;
                let drop = self.glue_value(Glue::Drop, drop);
                self.b.assign(slot, Rvalue::Use(drop))?;
            }
        }
        let code = Operand::Const(Constant {
            kind: ConstantKind::Func(code),
            ty: code_ty,
        });
        Ok(Rvalue::Aggregate(AggKind::Tuple, vec![code, made]))
    }

    /// At the start of the code of a function literal: the variables
    /// that it uses of the functions around it are other names of what
    /// the cells of its environment hold.
    pub fn open_env(&mut self) -> Built {
        if self.captured.is_empty() {
            return Ok(());
        }
        let captured = self.captured.clone();
        if self.in_frame {
            return self.open_frame(&captured);
        }
        let env_ty = self.env_type(&captured);
        let to = self.b.types().ptr(env_ty);
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let env = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, given, to))?;
        let env = self.b.deref(&Place::local(env))?;
        for (at, &local) in captured.iter().enumerate() {
            let slot = self.b.field(&env, ENV_HEAD + 2 * at as u32)?;
            let cell = self.b.assign_temp(Rvalue::Use(Operand::Copy(slot)))?;
            let declared = &self.body.locals[local.0 as usize];
            let of = self.lower(declared.ty, declared.span);
            self.name_cell(local, cell, of)?;
        }
        Ok(())
    }

    /// At the start of the code of a literal whose environment is in a
    /// frame: each variable is another name of what the address in the
    /// environment leads to, the variable itself or what its cell
    /// holds.
    fn open_frame(&mut self, captured: &[CheckedLocalId]) -> Built {
        let word = self.b.types().int(64, false);
        let mut fields = vec![word];
        let mut kinds = Vec::with_capacity(captured.len());
        for &local in captured {
            let declared = &self.body.locals[local.0 as usize];
            let of = self.lower(declared.ty, declared.span);
            let held = match self.celled(local) {
                true => self.cell_type(of),
                false => of,
            };
            fields.push(self.b.types().ptr(held));
            kinds.push(of);
        }
        let env_ty = self.b.types().tuple(&fields);
        let to = self.b.types().ptr(env_ty);
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let env = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, given, to))?;
        let env = self.b.deref(&Place::local(env))?;
        for (at, (&local, of)) in captured.iter().zip(kinds).enumerate() {
            let slot = self.b.field(&env, 1 + at as u32)?;
            let address = Rvalue::Use(Operand::Copy(slot));
            match self.celled(local) {
                true => {
                    let cell = self.b.assign_temp(address)?;
                    self.name_cell(local, cell, of)?;
                }
                false => {
                    let var = self.declare_alias(local, of)?;
                    self.b.assign(Place::local(var), address)?;
                }
            }
        }
        Ok(())
    }

    /// The next pass of a loop goes on with a new cell for the variable
    /// of the loop, which has a copy of what the variable holds now
    /// (CLO-8): a function value of this pass keeps the cell of this
    /// pass.
    pub fn next_cell(&mut self, local: CheckedLocalId) -> Built {
        let (Some(&cell), Some(&var)) = (self.cells.get(&local), self.vars_of(local)) else {
            return Ok(());
        };
        let declared = &self.body.locals[local.0 as usize];
        let (checked, span) = (declared.ty, declared.span);
        let of = self.lower(checked, span);
        let cell_ty = self.cell_type(of);
        let layout = self.b.types().layout(cell_ty, self.shared.layout);
        let word = self.b.types().int(64, false);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let bytes = self.bytes_ty();
        let made = self.runtime("_gopp_cell_new", &[word], bytes, vec![size])?;
        let to = self.b.types().ptr(cell_ty);
        let next = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, made, to))?;
        // The copy, by the rule of the type.
        let old = self.var_place(var)?;
        let mut value = Rvalue::Use(Operand::Copy(old));
        if !self.flat(of) {
            value = self.cloned_value(span, Some(checked), value)?;
        }
        let inside = self.b.deref(&Place::local(next))?;
        let inside = self.b.field(&inside, 1)?;
        let value = self.operand_of(value)?;
        self.b.assign(inside.clone(), Rvalue::Use(value))?;
        // The function lets the cell of this pass go and has the new
        // one in the same slot, which the scope of the loop ends.
        let drop = match self.flat(of) {
            true => None,
            false => self.drop_glue(Some(checked), span)?,
        };
        self.end(Place::local(cell), Ends::Cell(drop))?;
        let next = Operand::Copy(Place::local(next));
        self.b.assign(Place::local(cell), Rvalue::Use(next))?;
        let address = Rvalue::AddrOf(inside, crate::Mutability::Mut);
        self.b.assign(Place::local(var), address)
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
        let unwind = self.unwind()?;
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

    /// The method `name` of a declared type or of an interface type as
    /// what a method value or a method expression calls: the type of
    /// the method without its receiver, and whether the type is an
    /// interface.
    fn bound_method(
        shared: &Shared<'_>,
        ty: CheckedTypeId,
        name: Spur,
    ) -> Option<(CheckedTypeId, bool)> {
        let types = shared.types;
        if let Some(methods) = types.interface_methods(ty, shared.names) {
            let method = methods.iter().find(|method| method.name == name)?;
            return Some((method.sig, true));
        }
        match &*types.get(ty) {
            CheckedType::Named { id, .. } => {
                let method = shared.methods.get(&(*id, name))?;
                Some((method.sig, false))
            }
            _ => None,
        }
    }

    /// The method value `x.m`, for a place of the type `ty` (CLO-5):
    /// its code is the method with the receiver in its environment,
    /// which the package makes once. The receiver is worked out now: a
    /// copy of `x` for a method that takes a value, the address of `x`
    /// for one that takes an address, the interface value for a method
    /// of an interface. It lives in a cell of its own, which the
    /// environment has; a variable whose address is taken lives in a
    /// cell too, which the environment shares.
    pub fn method_value(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let shared = self.shared;
        let ExprKind::Selector { x, sel } = self.tree.exprs[expr].kind else {
            return Ok(self.placeholder(span, ty, "a method as a value"));
        };
        let selection = self.body.selections.get(&expr);
        let (Some(selection), Some(code_ty)) = (selection, self.code_type(ty)) else {
            return Ok(self.placeholder(span, ty, "a method as a value"));
        };
        let (holder, path) = (selection.holder, &selection.path[..]);
        let Some((_, dynamic)) = Self::bound_method(shared, holder, sel.name) else {
            return Ok(self.placeholder(span, ty, "a method of another package as a value"));
        };
        let entity = Entity::MethodValue {
            receiver: holder,
            name: sel.name,
        };
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            let what = "a method of a type without a name as a value";
            return Ok(self.placeholder(span, ty, what));
        };
        // The receiver, as the method takes it.
        let (bound, checked) = match dynamic {
            true => {
                let mut mode = PlaceMode::default();
                let base = self.place(x, &mut mode)?;
                let value = self.steps(base, path, span, &mut mode)?;
                let value = self.through(value, span, &mut mode)?;
                let of = self.b.place_ty(&value);
                if !self.is_iface(of) {
                    return Ok(self.placeholder(span, ty, "a method as a value"));
                }
                let copy =
                    self.cloned_value(span, Some(holder), Rvalue::Use(Operand::Copy(value)))?;
                (copy, Some(holder))
            }
            false => {
                let called = match self.callee(CalleeKey::Method(holder, sel.name))? {
                    Ok(called) => called,
                    Err(what) => return Ok(self.placeholder(span, ty, what)),
                };
                let Some(&wanted) = called.params.first() else {
                    return Ok(self.placeholder(span, ty, "a method as a value"));
                };
                if !path.is_empty() {
                    self.parts.insert(x, holder);
                }
                let value = self.receiver(span, x, path, wanted)?;
                let value = self.given(x, wanted, Rvalue::Use(value))?;
                let value = self.taken(x, wanted, value)?;
                self.parts.remove(&x);
                // A value is ended by the glue of its type; an address
                // owns nothing.
                let by_value = self.pointee(wanted) == wanted;
                (value, by_value.then_some(holder))
            }
        };
        let bound = self.operand_of(bound)?;
        let of = self.b.operand_ty(&bound);
        // The variable whose address the receiver is, if it is one of
        // this function: its cell lives as long as the value.
        let address = self.body.method_values.get(&expr) == Some(&MethodValue::Address);
        let root = match address {
            true => self
                .root_local(x)
                .filter(|root| self.cells.contains_key(root)),
            false => None,
        };
        let asked = self.memo.funcs.contains_key(&symbol);
        let code = self.function_named(&symbol, code_ty)?;
        if !asked {
            self.memo
                .glues
                .push((symbol, Made::MethodValue(sel.name), holder));
        }
        // The cell of the receiver, which the environment has alone.
        let bytes = self.bytes_ty();
        let word = self.b.types().int(64, false);
        let cell_ty = self.cell_type(of);
        let layout = self.b.types().layout(cell_ty, shared.layout);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let cell = self.runtime("_gopp_cell_new", &[word], bytes, vec![size])?;
        let to = self.b.types().ptr(cell_ty);
        let typed = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, cell.clone(), to))?;
        let inside = self.b.deref(&Place::local(typed))?;
        let inside = self.b.field(&inside, 1)?;
        self.b.assign(inside, Rvalue::Use(bound))?;
        let drop_sig = self.glue_sig(Glue::Drop);
        let mut fields = vec![word, word, to, drop_sig];
        let kept = match root {
            Some(root) => {
                let declared = &self.body.locals[root.0 as usize];
                let (root_checked, root_span) = (declared.ty, declared.span);
                let root_ty = self.lower(root_checked, root_span);
                let root_cell = self.cell_type(root_ty);
                fields.extend([self.b.types().ptr(root_cell), drop_sig]);
                Some((self.cells[&root], root_ty, root_checked))
            }
            None => None,
        };
        let env_ty = self.b.types().tuple(&fields);
        let count = self.b.const_int(word, 1 + u64::from(kept.is_some()))?;
        let made = self.runtime("_gopp_env_new", &[word], bytes, vec![count])?;
        let to = self.b.types().ptr(env_ty);
        let env = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, made.clone(), to))?;
        let env = self.b.deref(&Place::local(env))?;
        let slot = self.b.field(&env, ENV_HEAD)?;
        self.b
            .assign(slot, Rvalue::Use(Operand::Copy(Place::local(typed))))?;
        if let Some(checked) = checked.filter(|_| !self.flat(of))
            && let Some(drop) = self.drop_glue(Some(checked), span)?
        {
            let slot = self.b.field(&env, ENV_HEAD + 1)?;
            let drop = self.glue_value(Glue::Drop, drop);
            self.b.assign(slot, Rvalue::Use(drop))?;
        }
        if let Some((root_cell, root_ty, root_checked)) = kept {
            let shared_cell = Operand::Copy(Place::local(root_cell));
            let as_bytes = Rvalue::Cast(CastKind::PtrToPtr, shared_cell.clone(), bytes);
            let as_bytes = Operand::Copy(Place::local(self.b.assign_temp(as_bytes)?));
            self.runtime_call("_gopp_cell_retain", &[bytes], vec![as_bytes])?;
            let slot = self.b.field(&env, ENV_HEAD + 2)?;
            self.b.assign(slot, Rvalue::Use(shared_cell))?;
            if !self.flat(root_ty)
                && let Some(drop) = self.drop_glue(Some(root_checked), span)?
            {
                let slot = self.b.field(&env, ENV_HEAD + 3)?;
                let drop = self.glue_value(Glue::Drop, drop);
                self.b.assign(slot, Rvalue::Use(drop))?;
            }
        }
        let code = Operand::Const(Constant {
            kind: ConstantKind::Func(code),
            ty: code_ty,
        });
        Ok(Rvalue::Aggregate(AggKind::Tuple, vec![code, made]))
    }

    /// The method expression `T.m` or `(*T).m`, for a place of the type
    /// `ty`: the method as a function that takes its receiver first,
    /// which the package makes once and which has no environment.
    pub fn method_expr(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let shared = self.shared;
        let types = shared.types;
        let ExprKind::Selector { sel, .. } = self.tree.exprs[expr].kind else {
            return Ok(self.placeholder(span, ty, "a method as a value"));
        };
        let selection = self.body.selections.get(&expr);
        let (Some(selection), Some(code_ty)) = (selection, self.code_type(ty)) else {
            return Ok(self.placeholder(span, ty, "a method as a value"));
        };
        let holder = selection.holder;
        let method = match &*types.get(holder) {
            CheckedType::Named { id, .. } if selection.path.is_empty() => {
                shared.methods.get(&(*id, sel.name))
            }
            _ => None,
        };
        let Some(method) = method else {
            let what = "a method of an interface or of an embedded type as a function";
            return Ok(self.placeholder(span, ty, what));
        };
        // Whether the function takes an address and the method a value.
        let by_address = matches!(&*types.get(selection.recv), CheckedType::Pointer(_));
        let takes_address = matches!(&*types.get(method.receiver), CheckedType::Pointer(_));
        let pointer = by_address && !takes_address;
        let entity = Entity::MethodExpr {
            receiver: holder,
            name: sel.name,
            pointer,
        };
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            return Ok(self.placeholder(span, ty, "a method without a symbol as a function"));
        };
        if let Err(what) = self.callee(CalleeKey::Method(holder, sel.name))? {
            return Ok(self.placeholder(span, ty, what));
        }
        let asked = self.memo.funcs.contains_key(&symbol);
        let code = self.function_named(&symbol, code_ty)?;
        if !asked {
            let made = Made::MethodExpr {
                name: sel.name,
                pointer,
            };
            self.memo.glues.push((symbol, made, holder));
        }
        debug_assert_eq!(selection.kind, SelectionKind::MethodExpr);
        Ok(self.bare(code, code_ty))
    }

    /// The type of the code that the package makes for a method value
    /// (`first` is none) or a method expression of the declared type or
    /// the interface type `ty`: an environment, then for an expression
    /// the receiver as `first`, then the parameters of the method.
    pub fn bound_sig(
        shared: &Shared<'_>,
        memo: &mut super::Memo<'_>,
        types: &mut TypeInterner,
        (ty, name): (CheckedTypeId, Spur),
        first: Option<bool>,
    ) -> Result<TypeId, &'static str> {
        let Some((sig, _)) = Self::bound_method(shared, ty, name) else {
            return Err("a method that its type does not have");
        };
        let plain = memo.signature(shared, types, None, sig)?;
        let Type::Func(plain) = types.get(plain).clone() else {
            return Err("a function type");
        };
        let byte = types.int(8, false);
        let mut params = vec![types.ptr(byte)];
        if let Some(pointer) = first {
            let receiver = match &*shared.types.get(ty) {
                CheckedType::Named { id, .. } => shared.methods.get(&(*id, name)),
                _ => None,
            };
            let Some(receiver) = receiver.map(|method| method.receiver) else {
                return Err("a method that its type does not have");
            };
            let receiver = memo.lower_type(shared, types, receiver)?;
            params.push(match pointer {
                true => types.ptr(receiver),
                false => receiver,
            });
        }
        params.extend(plain.params.iter());
        Ok(types.intern(Type::Func(crate::FuncType {
            params: params.into(),
            ret: plain.ret,
            has_env: true,
            cc: crate::CallConv::Go,
        })))
    }

    /// The body of the code of a method value (`first` is none) or of a
    /// method expression of the type `ty`. A method value finds its
    /// receiver in the cell that its environment has; a method
    /// expression is given it after the environment, by its address if
    /// `first` says so. The method is called with it and with what
    /// follows, through the table of the interface value for a method
    /// of an interface. What the method only reads of what a call gave
    /// away ends here.
    pub fn bound_body(
        &mut self,
        ty: CheckedTypeId,
        name: Spur,
        first: Option<bool>,
    ) -> Built<Result<(), &'static str>> {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let Some((checked, dynamic)) = Self::bound_method(shared, ty, name) else {
            return Ok(Err("a method that its type does not have"));
        };
        let own = Self::bound_sig(shared, self.memo, self.b.types(), (ty, name), first);
        let own = match own.map(|own| (own, self.kind(own))) {
            Ok((id, Type::Func(own))) => (id, own),
            Ok(_) => return Ok(Err("a function type")),
            Err(what) => return Ok(Err(what)),
        };
        // How many parameters come before those of the method.
        let before = 1 + u32::from(first.is_some());
        let called = match dynamic {
            true => None,
            false => match self.callee(CalleeKey::Method(ty, name))? {
                Ok(called) => Some(called),
                Err(what) => return Ok(Err(what)),
            },
        };
        // The receiver where it is: in the cell of the environment, or
        // in the parameter.
        let held = match first {
            Some(_) => Place::local(self.b.param(1)),
            None => {
                let of = match &called {
                    Some(called) => called.params.first().copied(),
                    None => self.memo.lower_type(shared, self.b.types(), ty).ok(),
                };
                let Some(of) = of else {
                    return Ok(Err("a method without a receiver"));
                };
                let word = self.b.types().int(64, false);
                let cell = self.cell_type(of);
                let cell = self.b.types().ptr(cell);
                let drop = self.glue_sig(Glue::Drop);
                let env_ty = self.b.types().tuple(&[word, word, cell, drop]);
                let to = self.b.types().ptr(env_ty);
                let given = Operand::Copy(Place::local(self.b.param(0)));
                let env = self
                    .b
                    .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, given, to))?;
                let env = self.b.deref(&Place::local(env))?;
                let cell = self.b.field(&env, ENV_HEAD)?;
                let cell = self.slot_of(Rvalue::Use(Operand::Copy(cell)))?;
                let inside = self.b.deref(&cell)?;
                self.b.field(&inside, 1)?
            }
        };
        let (callee, receiver, ret) = match &called {
            None => {
                let methods = shared.types.interface_methods(ty, shared.names);
                let slot = methods
                    .and_then(|methods| methods.iter().position(|method| method.name == name));
                let Some(slot) = slot else {
                    return Ok(Err("a method that its interface does not have"));
                };
                let first_word = self.b.field(&held, 0)?;
                let first_word = self.slot_of(Rvalue::Use(Operand::Copy(first_word)))?;
                self.not_nil(&first_word)?;
                // What a table holds takes the same, without the
                // convention of an environment.
                let plain = Type::Func(crate::FuncType {
                    has_env: false,
                    ..own.1.clone()
                });
                let plain = self.b.types().intern(plain);
                let code = self.table_slot(&held, slot as u32, plain, None)?;
                let word = self.b.field(&held, 1)?;
                (code, self.record(word)?, own.1.ret)
            }
            Some(called) => {
                let Some(&wanted) = called.params.first() else {
                    return Ok(Err("a method without a receiver"));
                };
                let have = self.b.place_ty(&held);
                let read = called.lent.first() == Some(&true);
                let receiver = if wanted == have {
                    // A parameter of the expression is this function's
                    // own and goes on as it is; what the cell of a
                    // method value holds stays there.
                    match first {
                        Some(_) => Ok(Operand::Copy(held.clone())),
                        None => self.by_value(held.clone(), ty, read)?,
                    }
                } else if self.pointee(have) == wanted {
                    self.not_nil(&held)?;
                    let value = self.b.deref(&held)?;
                    self.by_value(value, ty, read)?
                } else {
                    Err("a method with a receiver of this kind as a value")
                };
                let receiver = match receiver {
                    Ok(receiver) => receiver,
                    Err(what) => return Ok(Err(what)),
                };
                let callee = Operand::Const(Constant {
                    kind: ConstantKind::Func(called.func),
                    ty: called.sig,
                });
                (callee, receiver, called.ret)
            }
        };
        let mut args = vec![receiver];
        for at in before..own.1.params.len() as u32 {
            args.push(Operand::Copy(Place::local(self.b.param(at))));
        }
        let result = Place::local(self.b.temp(ret));
        let next = self.b.new_block();
        let unwind = self.unwind()?;
        self.b
            .call(callee, args, result.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        if let (Some(called), CheckedType::Func { params, .. }) =
            (&called, &*shared.types.get(checked))
        {
            // The receiver of an expression that the method only read
            // where this function has it by value.
            let by_value = first == Some(false) && called.params.first() == Some(&own.1.params[1]);
            if by_value && called.lent.first() == Some(&true) && !self.flat(own.1.params[1]) {
                match self.ends_quietly(ty)? {
                    Ok(ends) => self.end(Place::local(self.b.param(1)), ends)?,
                    Err(what) => return Ok(Err(what)),
                }
            }
            for (at, &param) in params.iter().enumerate() {
                let read = called.lent.get(at + 1) == Some(&true);
                let Some(&lowered) = called.params.get(at + 1).filter(|_| read) else {
                    continue;
                };
                if self.flat(lowered) {
                    continue;
                }
                let slot = Place::local(self.b.param(before + at as u32));
                match self.ends_quietly(param)? {
                    Ok(ends) => self.end(slot, ends)?,
                    Err(what) => return Ok(Err(what)),
                }
            }
        }
        if ret != TypeInterner::UNIT {
            let whole = Place::local(self.b.result());
            self.b.assign(whole, Rvalue::Use(Operand::Copy(result)))?;
        }
        self.b.ret()?;
        Ok(Ok(()))
    }

    /// A call of the function value in the place, whose code has the
    /// type `code_ty`, with the arguments that follow the value in
    /// `given`: what the call gives, and its place.
    pub fn call_value(
        &mut self,
        span: SourceSpan,
        value: &Place,
        code_ty: TypeId,
        mut given: Vec<Operand>,
    ) -> Built<(TypeId, Place)> {
        let code = Operand::Copy(self.b.field(value, 0)?);
        let code = Operand::Copy(self.slot_of(Rvalue::Use(code))?);
        if self.source.dialect() == Dialect::Go {
            self.checked(Check::Nil(code.clone(), span))?;
        }
        let ret = match self.kind(code_ty) {
            Type::Func(sig) => sig.ret,
            _ => TypeInterner::UNIT,
        };
        // The environment in the place of the value.
        if let Some(first) = given.first_mut() {
            *first = Operand::Copy(self.b.field(value, 1)?);
        }
        let result = Place::local(self.b.temp(ret));
        let next = self.b.new_block();
        let unwind = self.unwind()?;
        self.b
            .call(code, given, result.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        Ok((ret, result))
    }

    /// `defer f(args)` for a function value `f`, a function literal
    /// too (CLO-3b, CLO-7): the value and the arguments are worked out
    /// now and kept, each for the statement, until the function is
    /// left. The type of the code of the value; none for what is no
    /// function value.
    pub fn defer_value(
        &mut self,
        callee: ExprId,
        args: &[ExprId],
        spread: bool,
    ) -> Built<Option<Result<TypeId, &'static str>>> {
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
        let ty = self.ty_of(callee);
        let Some(code_ty) = self.code_type(ty) else {
            return Ok(None);
        };
        let Type::Func(sig) = self.kind(code_ty) else {
            return Ok(None);
        };
        if spread || args.len() != declared {
            return Ok(Some(Err("a call with `...`")));
        }
        if sig.params.len() != declared + 1 {
            return Ok(Some(Err("a deferred call whose result may be a view")));
        }
        Ok(Some(Ok(code_ty)))
    }
}
