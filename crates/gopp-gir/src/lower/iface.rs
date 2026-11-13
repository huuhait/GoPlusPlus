//! Interface values (TYP-5 to TYP-9, gir.md §15). An interface value is
//! two words: the first says the type of the value that it holds, the
//! second is the value itself if it is at most a word that owns
//! nothing, or the address of a box that the interface value owns
//! alone. The runtime copies, ends, compares and hashes interface
//! values by the descriptor of the type, which is static data that
//! every package that needs it says, and the linker keeps once.

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::types::TypeId as CheckedTypeId;
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{BinaryOp, ExprId, ExprKind};

use super::expr::PlaceMode;
use super::func::{Built, FuncCx};
use super::glue::{Ends, Glue};
use super::{Memo, Shared};
use crate::{
    AggKind, BinOp, CastKind, Constant, ConstantKind, FuncId, Global, GlobalId, Linkage,
    Mutability, Operand, Place, Rvalue, Type, TypeId,
};

/// The marks of a descriptor, as the runtime reads them.
const INLINE: u64 = 1;
const COMPARABLE: u64 = 2;

/// An interface value that is looked into: `x.(T)`, a type switch.
pub(super) struct Looked {
    /// The interface value, in a place.
    pub value: Place,
    /// Its first word.
    pub itab: Operand,
    /// The type of the interface value, as the checker has it.
    pub checked: CheckedTypeId,
}

/// The parts of `x.(T)`.
struct Assertion {
    /// What `x` is.
    looked: Looked,
    /// The descriptor of `T`.
    info: GlobalId,
    /// `T`, as the checker has it and as GIR has it.
    checked: CheckedTypeId,
    ty: TypeId,
}

impl FuncCx<'_, '_> {
    /// Whether the type of GIR is that of the interface values.
    pub fn is_iface(&self, ty: TypeId) -> bool {
        self.memo.iface == Some(ty)
    }

    /// The function of the runtime that is the glue of a kind for every
    /// interface type.
    pub fn iface_glue(&mut self, kind: Glue) -> Built<FuncId> {
        let name = match kind {
            Glue::Clone => "_gopp_iface_clone",
            Glue::Drop => "_gopp_iface_drop",
            Glue::Eq => "_gopp_iface_eq",
            Glue::Hash => "_gopp_iface_hash",
        };
        let sig = self.glue_sig(kind);
        self.function_named(name, sig)
    }

    /// The type of a descriptor: `gopp_type` of the runtime.
    fn info_type(&mut self) -> TypeId {
        let bytes = self.bytes_ty();
        let glue = [Glue::Clone, Glue::Drop, Glue::Eq, Glue::Hash].map(|kind| self.glue_sig(kind));
        let types = self.b.types();
        let word = types.int(64, false);
        let string = types.intern(Type::String);
        let fields = [
            ("type", bytes),
            ("size", word),
            ("flags", word),
            ("clone", glue[0]),
            ("drop", glue[1]),
            ("eq", glue[2]),
            ("hash", glue[3]),
            ("name", string),
        ];
        Memo::runtime(types, "gopp.type", &fields)
    }

    /// Whether a value of the type of GIR is the second word of an
    /// interface value itself: it owns nothing and is no more than a
    /// word (TYP-8).
    fn inline(&mut self, ty: TypeId) -> bool {
        let layout = self.b.types().layout(ty, self.shared.layout);
        let word = self.shared.layout.pointer().size;
        self.flat(ty) && layout.is_ok_and(|layout| layout.size <= word)
    }

    /// The descriptor of the type, said in the module of the run; or
    /// why a value of the type is not held by an interface value yet.
    pub fn type_info(&mut self, ty: CheckedTypeId) -> Built<Result<GlobalId, &'static str>> {
        if let Some(&known) = self.memo.infos.get(&ty) {
            return Ok(known);
        }
        let made = self.type_info_new(ty)?;
        self.memo.infos.insert(ty, made);
        Ok(made)
    }

    fn type_info_new(&mut self, ty: CheckedTypeId) -> Built<Result<GlobalId, &'static str>> {
        let shared = self.shared;
        let Ok(symbol) = symbol_of(&mut self.memo.namer, Entity::TypeInfo(ty)) else {
            return Ok(Err("a value of a type without a symbol in an interface"));
        };
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(Ok(known));
        }
        let lowered = match self.memo.lower_type(shared, self.b.types(), ty) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let Ok(layout) = self.b.types().layout(lowered, shared.layout) else {
            return Ok(Err("a value of a type without a size in an interface"));
        };
        let mut flags = 0;
        if self.inline(lowered) {
            flags |= INLINE;
        }
        // The glue of each kind, or none where the bytes say all.
        let mut glue = [None; 4];
        if !self.flat(lowered) {
            for (at, kind) in [Glue::Clone, Glue::Drop].into_iter().enumerate() {
                match self.glue(kind, ty)? {
                    Ok(made) => glue[at] = Some(made),
                    Err(what) => return Ok(Err(what)),
                }
            }
        }
        if shared.types.comparable(ty) {
            flags |= COMPARABLE;
            if !self.equal_by_bytes(ty, lowered) {
                for (at, kind) in [Glue::Eq, Glue::Hash].into_iter().enumerate() {
                    match self.glue(kind, ty)? {
                        Ok(made) => glue[2 + at] = Some(made),
                        Err(what) => return Ok(Err(what)),
                    }
                }
            }
        }
        let record = self.info_type();
        let word = self.b.types().int(64, false);
        let itself = self.b.types().ptr(record);
        let name = shared.types.runtime_name(ty, shared.names);
        let name = match self.b.const_str(name.as_bytes()) {
            Operand::Const(name) => name,
            // A string of the module is a constant.
            _ => return Ok(Err("a type without a name in an interface")),
        };
        let id = self.b.module().globals.alloc(Global {
            name: symbol.clone(),
            ty: record,
            link: Linkage::LinkOnceOdr,
            mutable: false,
            parts: Vec::new(),
        });
        let int = |value: u64| Constant {
            kind: ConstantKind::Int(value),
            ty: word,
        };
        let mut parts = vec![
            Constant {
                kind: ConstantKind::Global(id),
                ty: itself,
            },
            int(layout.size),
            int(flags),
        ];
        for (kind, made) in [Glue::Clone, Glue::Drop, Glue::Eq, Glue::Hash]
            .into_iter()
            .zip(glue)
        {
            let ty = self.glue_sig(kind);
            let kind = made.map_or(ConstantKind::Zero, ConstantKind::Func);
            parts.push(Constant { kind, ty });
        }
        parts.push(name);
        self.b.module().globals[id].parts = parts;
        self.memo.globals.insert(symbol, id);
        Ok(Ok(id))
    }

    /// The address of a descriptor, as the first word of an interface
    /// value.
    fn info_word(&mut self, info: GlobalId) -> Built<Operand> {
        let record = self.info_type();
        let ty = self.b.types().ptr(record);
        let kind = ConstantKind::Global(info);
        let address = Operand::Const(Constant { kind, ty });
        let bytes = self.bytes_ty();
        let cast = Rvalue::Cast(CastKind::PtrToPtr, address, bytes);
        Ok(Operand::Copy(Place::local(self.b.assign_temp(cast)?)))
    }

    /// Whether the interface type has no method: its values say their
    /// type by the descriptor itself.
    fn empty_iface(&self, iface: CheckedTypeId) -> bool {
        let shared = self.shared;
        let methods = shared.types.interface_methods(iface, shared.names);
        methods.is_some_and(|methods| methods.is_empty())
    }

    /// The value of the expression, read as the type `from`, in a value
    /// of the interface type `to`, for a place of the type `ty` (TYP-6).
    /// The interface value is new and its own: it holds the value
    /// itself if the expression gives it up, a copy otherwise.
    pub fn in_iface(
        &mut self,
        expr: ExprId,
        ty: TypeId,
        from: CheckedTypeId,
        to: CheckedTypeId,
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        if !self.is_iface(ty) {
            return Ok(self.placeholder(span, ty, "a value in an interface of this kind"));
        }
        if !self.empty_iface(to) {
            return Ok(self.placeholder(span, ty, "a value in an interface with methods"));
        }
        let lowered = self.lower(from, span);
        let info = match self.type_info(from)? {
            Ok(info) => info,
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
        let value = self.rvalue_plain(expr, lowered)?;
        let value = self.taken(expr, lowered, value)?;
        self.empty_moved()?;
        let bytes = self.bytes_ty();
        let layout = self.b.types().layout(lowered, self.shared.layout);
        let size = layout.map_or(0, |layout| layout.size);
        let data = match (self.inline(lowered), size) {
            (true, 0) => self.zero(bytes),
            // The bytes of the value are the first of the word, and the
            // rest of the word is zero.
            (true, _) => {
                let word = self.slot_of(Rvalue::Use(self.zero(bytes)))?;
                let at = self
                    .b
                    .assign_temp(Rvalue::AddrOf(word.clone(), Mutability::Mut))?;
                let to = self.b.types().ptr(lowered);
                let at = Operand::Copy(Place::local(at));
                let typed = self
                    .b
                    .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, at, to))?;
                let inside = self.b.deref(&Place::local(typed))?;
                let value = self.operand_of(value)?;
                self.b.assign(inside, Rvalue::Use(value))?;
                Operand::Copy(word)
            }
            (false, _) => {
                let slot = self.slot_of(value)?;
                let at = self.record(slot)?;
                let word = self.b.types().int(64, false);
                let size = self.b.const_int(word, size)?;
                self.runtime("_gopp_iface_box", &[bytes, word], bytes, vec![at, size])?
            }
        };
        let itab = self.info_word(info)?;
        let made = self.slot_of(Rvalue::Aggregate(AggKind::Of(ty), vec![itab, data]))?;
        let drop = self.iface_glue(Glue::Drop)?;
        self.own_temp(made.clone(), Ends::Glue(drop));
        Ok(Rvalue::Use(Operand::Copy(made)))
    }

    /// The interface value of the expression, in a place, with its
    /// first word.
    fn looked(&mut self, x: ExprId) -> Built<Result<Looked, &'static str>> {
        let Some(typed) = self.body.types.get(&x) else {
            return Ok(Err("a value whose type is not known"));
        };
        let checked = typed.ty;
        let mut mode = PlaceMode::default();
        let value = self.place(x, &mut mode)?;
        let span = self.tree.exprs[x].span;
        let value = self.through(value, span, &mut mode)?;
        if !self.is_iface(self.b.place_ty(&value)) {
            return Ok(Err("a value in an interface of this kind"));
        }
        if !self.empty_iface(checked) {
            return Ok(Err("a value in an interface with methods"));
        }
        let itab = Operand::Copy(self.b.field(&value, 0)?);
        let itab = Operand::Copy(self.slot_of(Rvalue::Use(itab))?);
        Ok(Ok(Looked {
            value,
            itab,
            checked,
        }))
    }

    /// Whether the interface value holds a value of the type.
    fn holds(&mut self, looked: &Looked, info: GlobalId) -> Built<Operand> {
        let wanted = self.info_word(info)?;
        let holds = Rvalue::Binary(BinOp::Eq, looked.itab.clone(), wanted);
        Ok(Operand::Copy(Place::local(self.b.assign_temp(holds)?)))
    }

    /// Whether the interface value is nil.
    fn holds_nothing(&mut self, looked: &Looked) -> Built<Operand> {
        let bytes = self.bytes_ty();
        let none = self.zero(bytes);
        let nothing = Rvalue::Binary(BinOp::Eq, looked.itab.clone(), none);
        Ok(Operand::Copy(Place::local(self.b.assign_temp(nothing)?)))
    }

    /// The value of the type `ty` that the interface value holds, where
    /// it is: in the box, or in the second word.
    fn inside(&mut self, looked: &Looked, ty: TypeId) -> Built<Place> {
        let data = self.b.field(&looked.value, 1)?;
        let at = match self.inline(ty) {
            true => self.record(data)?,
            false => Operand::Copy(data),
        };
        let to = self.b.types().ptr(ty);
        let typed = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, at, to))?;
        self.b.deref(&Place::local(typed))
    }

    /// A copy of the value in the place for an owner of its own, by the
    /// rule of its type `checked`, with how the copy ends.
    fn copy_of(
        &mut self,
        span: SourceSpan,
        place: Place,
        checked: CheckedTypeId,
    ) -> Built<(Place, Option<Ends>)> {
        let ty = self.b.place_ty(&place);
        let value = Rvalue::Use(Operand::Copy(place));
        if self.flat(ty) {
            return Ok((self.slot_of(value)?, None));
        }
        if let Type::String = self.kind(ty) {
            let copy = self.slot_of(value)?;
            self.counted(copy.clone(), true)?;
            return Ok((copy, Some(Ends::Text)));
        }
        let copy = match self.cloned_value(span, Some(checked), value)? {
            Rvalue::Use(Operand::Copy(copy)) => copy,
            other => self.slot_of(other)?,
        };
        Ok((copy, self.ends_of(Some(checked), span)?))
    }

    fn assertion(&mut self, expr: ExprId) -> Built<Result<Assertion, &'static str>> {
        let ExprKind::Assert { x, .. } = self.tree.exprs[expr].kind else {
            return Ok(Err("this expression"));
        };
        let Some(typed) = self.body.types.get(&expr) else {
            return Ok(Err("a value whose type is not known"));
        };
        let span = self.tree.exprs[expr].span;
        // With a second value the expression has the type of both.
        let types = self.shared.types;
        let wanted = match &*types.get(typed.ty) {
            gopp_sema::types::Type::Tuple(parts) if parts.len() == 2 => parts[0],
            _ => typed.ty,
        };
        if types.is_interface(wanted) {
            return Ok(Err("a type assertion to an interface type"));
        }
        let ty = self.lower(wanted, span);
        let info = match self.type_info(wanted)? {
            Ok(info) => info,
            Err(what) => return Ok(Err(what)),
        };
        Ok(self.looked(x)?.map(|looked| Assertion {
            looked,
            info,
            checked: wanted,
            ty,
        }))
    }

    /// Whether the expression is `x.(T)`.
    pub fn is_assertion(&self, expr: ExprId) -> bool {
        let inner = Shared::unparen(self.tree, expr);
        matches!(self.tree.exprs[inner].kind, ExprKind::Assert { .. })
    }

    /// `x.(T)`: the value that `x` holds, read where it is; the program
    /// ends if `x` holds no `T`, as in Go.
    pub fn asserted(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let Assertion { looked, info, .. } = match self.assertion(expr)? {
            Ok(parts) if parts.ty == ty => parts,
            Ok(_) => return Ok(self.placeholder(span, ty, "a value whose type is not known")),
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
        let holds = self.holds(&looked, info)?;
        let [fails, held] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(holds, vec![(0, fails)], held)?;
        self.b.switch_to(fails);
        let around = self.b.loc();
        self.at(span);
        let shared = self.shared;
        let name = shared.types.runtime_name(looked.checked, shared.names);
        let name = self.b.const_str(name.as_bytes());
        let bytes = self.bytes_ty();
        let string = self.b.operand_ty(&name);
        let args = vec![looked.itab.clone(), self.info_word(info)?, name];
        self.runtime_call("_gopp_panic_assert", &[bytes, bytes, string], args)?;
        self.b.unreachable()?;
        self.b.set_loc(around);
        self.b.switch_to(held);
        Ok(Rvalue::Use(Operand::Copy(self.inside(&looked, ty)?)))
    }

    /// `v, ok := x.(T)`: a copy of the value that `x` holds and `true`,
    /// or the zero of `T` and `false`.
    pub fn asserted_ok(&mut self, expr: ExprId) -> Built<Option<[Operand; 2]>> {
        let inner = Shared::unparen(self.tree, expr);
        let span = self.tree.exprs[inner].span;
        let Assertion {
            looked,
            info,
            checked,
            ty,
        } = match self.assertion(inner)? {
            Ok(parts) => parts,
            Err(what) => {
                self.unsupported(span, what);
                return Ok(None);
            }
        };
        let holds = self.holds(&looked, info)?;
        let holds = self.stable(holds)?;
        let value = self.slot_of(Rvalue::Use(self.zero(ty)))?;
        let [held, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(holds.clone(), vec![(0, done)], held)?;
        self.b.switch_to(held);
        let inside = self.inside(&looked, ty)?;
        let (copy, ends) = self.copy_of(span, inside, checked)?;
        self.b
            .assign(value.clone(), Rvalue::Use(Operand::Copy(copy)))?;
        self.b.goto(done)?;
        self.b.switch_to(done);
        if let Some(ends) = ends {
            self.own_temp(value.clone(), ends);
        }
        Ok(Some([Operand::Copy(value), holds]))
    }

    /// `x == nil` and `x != nil` for an interface value: its first word
    /// says it.
    pub fn nil_iface(&mut self, op: BinaryOp, value: Operand) -> Built<Rvalue> {
        let value = self.slot_of(Rvalue::Use(value))?;
        let itab = Operand::Copy(self.b.field(&value, 0)?);
        let bytes = self.bytes_ty();
        let none = self.zero(bytes);
        let op = match op {
            BinaryOp::Ne => BinOp::Ne,
            _ => BinOp::Eq,
        };
        Ok(Rvalue::Binary(op, itab, none))
    }

    /// A type switch: the interface value is looked into once, and the
    /// first case that it holds a value of runs. The variable of a
    /// clause with one type is a copy of the value that is held; in any
    /// other clause it is a copy of the interface value (TYP-9).
    pub fn type_switch(
        &mut self,
        x: ExprId,
        clauses: &[gopp_syntax::ast::CaseClause],
        bodies: &[crate::BlockId],
        otherwise: crate::BlockId,
    ) -> Built<Option<Looked>> {
        let span = self.tree.exprs[x].span;
        // What the value is made of lives as long as the switch.
        let mark = self.temps.len();
        let looked = match self.looked(x)? {
            Ok(looked) => looked,
            Err(what) => {
                self.unsupported(span, what);
                self.b.goto(otherwise)?;
                return Ok(None);
            }
        };
        self.keep_temps(mark)?;
        for (clause, &body) in clauses.iter().zip(bodies) {
            for &case in clause.cases.iter().flatten() {
                let holds = match self.body.case_types.get(&case) {
                    None => self.holds_nothing(&looked)?,
                    Some(&ty) if self.shared.types.is_interface(ty) => {
                        let span = self.tree.exprs[case].span;
                        self.unsupported(span, "a case of an interface type");
                        continue;
                    }
                    Some(&ty) => match self.type_info(ty)? {
                        Ok(info) => self.holds(&looked, info)?,
                        Err(what) => {
                            let span = self.tree.exprs[case].span;
                            self.unsupported(span, what);
                            continue;
                        }
                    },
                };
                let next = self.b.new_block();
                self.b.switch(holds, vec![(0, next)], body)?;
                self.b.switch_to(next);
            }
        }
        self.b.goto(otherwise)?;
        Ok(Some(looked))
    }

    /// The variable of a clause of a type switch, declared in the scope
    /// of the clause with its own copy of what the switch looks into.
    pub fn switch_var(
        &mut self,
        looked: &Looked,
        local: gopp_sema::LocalId,
        clause: &gopp_syntax::ast::CaseClause,
    ) -> Built {
        let declared = &self.body.locals[local.0 as usize];
        let (checked, span) = (declared.ty, declared.span);
        let ty = self.lower(checked, span);
        let one = match clause.cases.as_deref() {
            Some([case]) => self.body.case_types.contains_key(case),
            _ => false,
        };
        let from = match one && !self.is_iface(ty) {
            true => self.inside(looked, ty)?,
            false => looked.value.clone(),
        };
        if self.b.place_ty(&from) != ty {
            self.unsupported(span, "a variable of a type switch of this type");
            return Ok(());
        }
        let (copy, _) = self.copy_of(span, from, checked)?;
        let var = self.declare(local)?;
        self.b
            .assign(Place::local(var), Rvalue::Use(Operand::Copy(copy)))
    }
}
