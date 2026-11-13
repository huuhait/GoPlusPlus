//! Interface values (TYP-5 to TYP-9, gir.md §15). An interface value is
//! two words: the first says the type of the value that it holds, the
//! second is the value itself if it is at most a word that owns
//! nothing, or the address of a box that the interface value owns
//! alone. The runtime copies, ends, compares and hashes interface
//! values by the descriptor of the type, which is static data that
//! every package that needs it says, and the linker keeps once.

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::SelectionKind;
use gopp_sema::types::{Member, Type as CheckedType, TypeId as CheckedTypeId};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{BinaryOp, ExprId, ExprKind};
use lasso::Spur;

use super::expr::{Called, Check, Code, PlaceMode};
use super::func::{Built, FuncCx};
use super::glue::{Ends, Glue};
use super::{CalleeKey, Made, Memo, Shared};
use crate::{
    AggKind, AssertKind, BinOp, CallConv, CastKind, Constant, ConstantKind, FuncId, FuncType,
    Global, GlobalId, Linkage, Mutability, Operand, Place, Rvalue, Type, TypeId, TypeInterner,
    UnwindAction,
};

/// The marks of a descriptor, as the runtime reads them.
const INLINE: u64 = 1;
const COMPARABLE: u64 = 2;

/// An interface value that is looked into: `x.(T)`, a type switch.
pub(super) struct Looked {
    /// The interface value, in a place.
    pub value: Place,
    /// Its first word.
    pub first: Operand,
    /// The address of the descriptor of the type of the value that it
    /// holds; null for nil.
    pub info: Operand,
    /// The type of the interface value, as the checker has it.
    pub checked: CheckedTypeId,
}

/// A method of an interface value that a selector names: the
/// expression of the value, the way to it, the interface type and the
/// name.
pub(super) type IfaceMethod<'a> = (ExprId, &'a [u32], CheckedTypeId, Spur);

/// The type of what a table of methods holds for the method `name` of
/// the declared type `ty`: the method with the address of the second
/// word of an interface value in the place of its receiver.
pub(super) fn thunk_sig(
    shared: &Shared<'_>,
    memo: &mut Memo<'_>,
    types: &mut TypeInterner,
    ty: CheckedTypeId,
    name: Spur,
) -> Result<TypeId, &'static str> {
    let method = match &*shared.types.get(ty) {
        CheckedType::Named { id, .. } => shared.methods.get(&(*id, name)),
        _ => None,
    };
    let Some(method) = method else {
        return Err("a method of another package in an interface");
    };
    let sig = memo.signature(shared, types, Some(method.receiver), method.sig)?;
    let Type::Func(sig) = types.get(sig).clone() else {
        return Err("a function type");
    };
    let byte = types.int(8, false);
    let mut params = vec![types.ptr(byte)];
    params.extend(sig.params.iter().skip(1));
    Ok(types.intern(Type::Func(FuncType {
        params: params.into(),
        ret: sig.ret,
        has_env: false,
        cc: CallConv::Go,
    })))
}

/// What `x.(T)` and a case of a type switch ask of an interface value.
#[derive(Clone, Copy)]
enum Wanted {
    /// A value of the type with the descriptor.
    Type(GlobalId),
    /// A value with the methods of the interface type whose methods
    /// the global says.
    Iface(GlobalId),
}

/// The parts of `x.(T)`.
struct Assertion {
    /// What `x` is.
    looked: Looked,
    wanted: Wanted,
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
            ("methods", bytes),
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
        let methods = match self.method_table(ty)? {
            Ok(methods) => methods,
            Err(what) => return Ok(Err(what)),
        };
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
        let bytes = self.bytes_ty();
        parts.push(match methods {
            Some(methods) => {
                let record = self.b.module().globals[methods].ty;
                let ty = self.b.types().ptr(record);
                let kind = ConstantKind::Global(methods);
                Constant { kind, ty }
            }
            None => Constant {
                kind: ConstantKind::Zero,
                ty: bytes,
            },
        });
        self.b.module().globals[id].parts = parts;
        self.memo.globals.insert(symbol, id);
        Ok(Ok(id))
    }

    /// The address of a descriptor or of a table of methods, as a word
    /// of an interface value.
    fn info_word(&mut self, info: GlobalId) -> Built<Operand> {
        let record = self.b.module().globals[info].ty;
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
        let lowered = self.lower(from, span);
        let value = self.rvalue_plain(expr, lowered)?;
        let value = self.taken(expr, lowered, value)?;
        self.empty_moved()?;
        self.boxed(span, ty, (from, to), value)
    }

    /// The value of the type `from`, which is its own, in a value of
    /// the interface type `to` for a place of the type `ty`.
    pub fn boxed(
        &mut self,
        span: SourceSpan,
        ty: TypeId,
        (from, to): (CheckedTypeId, CheckedTypeId),
        value: Rvalue,
    ) -> Built<Rvalue> {
        if !self.is_iface(ty) {
            return Ok(self.placeholder(span, ty, "a value in an interface of this kind"));
        }
        // What the first word is the address of: the descriptor of the
        // type, or the table of the methods of the interface for it.
        let first = match self.empty_iface(to) {
            true => self.type_info(from)?,
            false => self.itab(from, to)?,
        };
        let lowered = self.lower(from, span);
        let info = match first {
            Ok(info) => info,
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
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
        let first = Operand::Copy(self.b.field(&value, 0)?);
        let first = Operand::Copy(self.slot_of(Rvalue::Use(first))?);
        // The first word is the address of a descriptor or of a table
        // whose first word is one: both are read alike.
        let info = self.info_of(first.clone())?;
        Ok(Ok(Looked {
            value,
            first,
            info,
            checked,
        }))
    }

    /// Whether the interface value holds what is wanted; and for an
    /// interface type, the first word that the value has as a value of
    /// that type, which is null if it has none.
    fn holds(&mut self, looked: &Looked, wanted: Wanted) -> Built<(Operand, Option<Operand>)> {
        let (holds, table) = match wanted {
            Wanted::Type(info) => {
                let wanted = self.info_word(info)?;
                let holds = Rvalue::Binary(BinOp::Eq, looked.info.clone(), wanted);
                (holds, None)
            }
            Wanted::Iface(itype) => {
                let bytes = self.bytes_ty();
                let args = vec![looked.first.clone(), self.info_word(itype)?];
                let table = self.runtime("_gopp_iface_table", &[bytes, bytes], bytes, args)?;
                let none = self.zero(bytes);
                let holds = Rvalue::Binary(BinOp::Ne, table.clone(), none);
                (holds, Some(table))
            }
        };
        let holds = Operand::Copy(Place::local(self.b.assign_temp(holds)?));
        Ok((holds, table))
    }

    /// What `x.(T)` asks for a type `T`: its descriptor, or the methods
    /// of an interface type.
    fn wanted(&mut self, ty: CheckedTypeId) -> Built<Result<Wanted, &'static str>> {
        Ok(match self.shared.types.is_interface(ty) {
            true => self.itype(ty)?.map(Wanted::Iface),
            false => self.type_info(ty)?.map(Wanted::Type),
        })
    }

    /// The value that the interface value holds, as what was wanted of
    /// it: the value of the type `ty` where it is, or the interface
    /// value with the first word `table`.
    fn held_as(&mut self, looked: &Looked, table: Option<Operand>, ty: TypeId) -> Built<Place> {
        match table {
            None => self.inside(looked, ty),
            Some(table) => {
                let data = Operand::Copy(self.b.field(&looked.value, 1)?);
                self.slot_of(Rvalue::Aggregate(AggKind::Of(ty), vec![table, data]))
            }
        }
    }

    /// Whether the interface value is nil.
    fn holds_nothing(&mut self, looked: &Looked) -> Built<Operand> {
        let bytes = self.bytes_ty();
        let none = self.zero(bytes);
        let nothing = Rvalue::Binary(BinOp::Eq, looked.first.clone(), none);
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
        let checked = wanted;
        let ty = self.lower(checked, span);
        let wanted = match self.wanted(checked)? {
            Ok(wanted) => wanted,
            Err(what) => return Ok(Err(what)),
        };
        Ok(self.looked(x)?.map(|looked| Assertion {
            looked,
            wanted,
            checked,
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
        let Assertion { looked, wanted, .. } = match self.assertion(expr)? {
            Ok(parts) if parts.ty == ty => parts,
            Ok(_) => return Ok(self.placeholder(span, ty, "a value whose type is not known")),
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
        let (holds, table) = self.holds(&looked, wanted)?;
        let [fails, held] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(holds, vec![(0, fails)], held)?;
        self.b.switch_to(fails);
        let around = self.b.loc();
        self.at(span);
        let bytes = self.bytes_ty();
        match wanted {
            Wanted::Type(info) => {
                let shared = self.shared;
                let name = shared.types.runtime_name(looked.checked, shared.names);
                let name = self.b.const_str(name.as_bytes());
                let string = self.b.operand_ty(&name);
                let args = vec![looked.first.clone(), self.info_word(info)?, name];
                self.runtime_call("_gopp_panic_assert", &[bytes, bytes, string], args)?;
            }
            Wanted::Iface(itype) => {
                let args = vec![looked.first.clone(), self.info_word(itype)?];
                self.runtime_call("_gopp_panic_assert_iface", &[bytes, bytes], args)?;
            }
        }
        self.b.unreachable()?;
        self.b.set_loc(around);
        self.b.switch_to(held);
        Ok(Rvalue::Use(Operand::Copy(
            self.held_as(&looked, table, ty)?,
        )))
    }

    /// `v, ok := x.(T)`: a copy of the value that `x` holds and `true`,
    /// or the zero of `T` and `false`.
    pub fn asserted_ok(&mut self, expr: ExprId) -> Built<Option<[Operand; 2]>> {
        let inner = Shared::unparen(self.tree, expr);
        let span = self.tree.exprs[inner].span;
        let Assertion {
            looked,
            wanted,
            checked,
            ty,
        } = match self.assertion(inner)? {
            Ok(parts) => parts,
            Err(what) => {
                self.unsupported(span, what);
                return Ok(None);
            }
        };
        let (holds, table) = self.holds(&looked, wanted)?;
        let holds = self.stable(holds)?;
        let value = self.slot_of(Rvalue::Use(self.zero(ty)))?;
        let [held, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(holds.clone(), vec![(0, done)], held)?;
        self.b.switch_to(held);
        let inside = self.held_as(&looked, table, ty)?;
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
    /// first case that it holds a value of runs. With the value comes,
    /// for each clause, the first word that the value has as a value of
    /// the interface type of the last case of the clause, if that is
    /// one.
    pub fn type_switch(
        &mut self,
        x: ExprId,
        clauses: &[gopp_syntax::ast::CaseClause],
        bodies: &[crate::BlockId],
        otherwise: crate::BlockId,
    ) -> Built<Option<(Looked, Vec<Option<Operand>>)>> {
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
        let mut tables = Vec::with_capacity(clauses.len());
        for (clause, &body) in clauses.iter().zip(bodies) {
            let mut last = None;
            for &case in clause.cases.iter().flatten() {
                let wanted = match self.body.case_types.get(&case) {
                    None => None,
                    Some(&ty) => match self.wanted(ty)? {
                        Ok(wanted) => Some(wanted),
                        Err(what) => {
                            let span = self.tree.exprs[case].span;
                            self.unsupported(span, what);
                            continue;
                        }
                    },
                };
                let holds = match wanted {
                    None => self.holds_nothing(&looked)?,
                    Some(wanted) => {
                        let (holds, table) = self.holds(&looked, wanted)?;
                        last = table;
                        holds
                    }
                };
                let next = self.b.new_block();
                self.b.switch(holds, vec![(0, next)], body)?;
                self.b.switch_to(next);
            }
            tables.push(last);
        }
        self.b.goto(otherwise)?;
        Ok(Some((looked, tables)))
    }

    /// The variable of a clause of a type switch, declared in the scope
    /// of the clause with its own copy of what the switch looks into:
    /// the value that is held for a clause with one type, as that type
    /// (`table` is its first word as a value of an interface type), and
    /// the interface value itself for any other clause (TYP-9).
    pub fn switch_var(
        &mut self,
        looked: &Looked,
        table: Option<Operand>,
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
        let from = match one {
            true => self.held_as(looked, table, ty)?,
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

impl<'a> FuncCx<'a, '_> {
    /// The address of the descriptor that the first word of an
    /// interface value with methods leads to: the first word of its
    /// table, or null for nil, which has no table.
    fn info_of(&mut self, first: Operand) -> Built<Operand> {
        let bytes = self.bytes_ty();
        let info = self.slot_of(Rvalue::Use(self.zero(bytes)))?;
        let set = Rvalue::Binary(BinOp::Ne, first.clone(), self.zero(bytes));
        let set = Operand::Copy(Place::local(self.b.assign_temp(set)?));
        let [reads, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(set, vec![(0, done)], reads)?;
        self.b.switch_to(reads);
        let to = self.b.types().ptr(bytes);
        let table = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, first, to))?;
        let word = self.b.deref(&Place::local(table))?;
        self.b
            .assign(info.clone(), Rvalue::Use(Operand::Copy(word)))?;
        self.b.goto(done)?;
        self.b.switch_to(done);
        Ok(Operand::Copy(info))
    }

    /// What the table of the methods of the declared type `ty`, or of
    /// the pointers to it, holds for its method `name`: declared in the
    /// module of the run, and asked of the package.
    fn thunk(
        &mut self,
        ty: CheckedTypeId,
        name: Spur,
        pointer: bool,
    ) -> Built<Result<FuncId, &'static str>> {
        let entity = Entity::IfaceThunk {
            receiver: ty,
            name,
            pointer,
        };
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            return Ok(Err("a method without a symbol in an interface"));
        };
        if let Some(&known) = self.memo.funcs.get(&symbol) {
            return Ok(Ok(known));
        }
        // What cannot be called cannot be asked for.
        if let Err(what) = self.callee(CalleeKey::Method(ty, name))? {
            return Ok(Err(what));
        }
        let shared = self.shared;
        let sig = match thunk_sig(shared, self.memo, self.b.types(), ty, name) {
            Ok(sig) => sig,
            Err(what) => return Ok(Err(what)),
        };
        let thunk = self.function_named(&symbol, sig)?;
        self.memo
            .glues
            .push((symbol, Made::Thunk { name, pointer }, ty));
        Ok(Ok(thunk))
    }

    /// The table of the methods of the interface type `iface` for the
    /// values of the type `from`, said in the module of the run: the
    /// descriptor of `from`, then what is called for each method of the
    /// interface, in the order of their names.
    fn itab(
        &mut self,
        from: CheckedTypeId,
        iface: CheckedTypeId,
    ) -> Built<Result<GlobalId, &'static str>> {
        if let Some(&known) = self.memo.itabs.get(&(from, iface)) {
            return Ok(known);
        }
        let made = self.itab_new(from, iface)?;
        self.memo.itabs.insert((from, iface), made);
        Ok(made)
    }

    fn itab_new(
        &mut self,
        from: CheckedTypeId,
        iface: CheckedTypeId,
    ) -> Built<Result<GlobalId, &'static str>> {
        let shared = self.shared;
        let types = shared.types;
        let Ok(symbol) = symbol_of(&mut self.memo.namer, Entity::ITab(from, iface)) else {
            return Ok(Err("a value of a type without a symbol in an interface"));
        };
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(Ok(known));
        }
        let info = match self.type_info(from)? {
            Ok(info) => info,
            Err(what) => return Ok(Err(what)),
        };
        let Some(methods) = types.interface_methods(iface, shared.names) else {
            return Ok(Err("a value in an interface of this kind"));
        };
        let pointer = matches!(&*types.get(types.underlying(from)), CheckedType::Pointer(_));
        let bytes = self.bytes_ty();
        let record = self.b.module().globals[info].ty;
        let mut parts = vec![Constant {
            kind: ConstantKind::Global(info),
            ty: self.b.types().ptr(record),
        }];
        for method in &methods {
            let package = method.package.unwrap_or(shared.path);
            let found = types.lookup(from, method.name, package, shared.names, false);
            let holder = match found {
                Ok(found) if !found.path.is_empty() => {
                    return Ok(Err("a method of an embedded type in an interface"));
                }
                Ok(found) if matches!(found.member, Member::Method { .. }) => found.holder,
                _ => return Ok(Err("a value without the methods of its interface")),
            };
            let thunk = match self.thunk(holder, method.name, pointer)? {
                Ok(thunk) => thunk,
                Err(what) => return Ok(Err(what)),
            };
            parts.push(Constant {
                kind: ConstantKind::Func(thunk),
                ty: self.b.module().funcs[thunk].sig(),
            });
        }
        let table = Type::Array(bytes, parts.len() as u64);
        let ty = self.b.types().intern(table);
        let id = self.b.module().globals.alloc(Global {
            name: symbol.clone(),
            ty,
            link: Linkage::LinkOnceOdr,
            mutable: false,
            parts,
        });
        self.memo.globals.insert(symbol, id);
        Ok(Ok(id))
    }

    /// The key of a method, which is the same for the method of a type
    /// and the method of an interface that it is: the name, the package
    /// of a name that is not exported, and the type.
    fn method_key(&mut self, method: &gopp_sema::types::Method) -> Result<Vec<u8>, &'static str> {
        let shared = self.shared;
        let Ok(sig) = symbol_of(&mut self.memo.namer, Entity::TypeInfo(method.sig)) else {
            return Err("a method of a type without a symbol in an interface");
        };
        let package = method.package.map(|package| shared.names.resolve(&package));
        let name = shared.names.resolve(&method.name);
        Ok(format!("{name}|{}|{sig}", package.unwrap_or_default()).into_bytes())
    }

    /// Whether a value of the struct type has methods of a type that
    /// it embeds.
    fn embeds_methods(&mut self, ty: CheckedTypeId) -> bool {
        let types = self.shared.types;
        let mut rest = vec![ty];
        while let Some(ty) = rest.pop() {
            self.memo.steps += 1;
            let under = types.get(types.underlying(ty));
            let fields = match &*under {
                CheckedType::Struct(fields) => fields,
                CheckedType::Pointer(to) => {
                    rest.push(*to);
                    continue;
                }
                _ => continue,
            };
            for field in fields.iter().filter(|field| field.embedded) {
                let mut held = field.ty;
                if let CheckedType::Pointer(to) = &*types.get(held) {
                    held = *to;
                }
                if types.declares_methods(held) || types.is_interface(held) {
                    return true;
                }
                rest.push(held);
            }
        }
        false
    }

    /// The methods of the type that the tables of interfaces are made
    /// of while the program runs, said in the module of the run: the
    /// key of each and what a table holds for it, in the order of the
    /// keys. None for a type without methods.
    fn method_table(&mut self, ty: CheckedTypeId) -> Built<Result<Option<GlobalId>, &'static str>> {
        let shared = self.shared;
        let types = shared.types;
        if self.embeds_methods(ty) {
            return Ok(Err("a method of an embedded type in an interface"));
        }
        let (holder, pointer) = match &*types.get(ty) {
            CheckedType::Pointer(to) => (*to, true),
            _ => (ty, false),
        };
        let CheckedType::Named { id, .. } = &*types.get(holder) else {
            return Ok(Ok(None));
        };
        let mut methods = Vec::new();
        for method in types.declared_methods(*id) {
            // A value of a type of a Go file has the methods that take
            // a value only.
            let gopp = (shared.methods.get(&(*id, method.method.name))).is_some_and(|m| m.gopp);
            if method.pointer_receiver && !pointer && !gopp {
                continue;
            }
            let key = match self.method_key(&method.method) {
                Ok(key) => key,
                Err(what) => return Ok(Err(what)),
            };
            match self.thunk(holder, method.method.name, pointer)? {
                Ok(thunk) => methods.push((key, thunk)),
                Err(_) => {
                    return Ok(Err(
                        "a value in an interface whose type has a method that is not supported yet",
                    ));
                }
            }
        }
        if methods.is_empty() {
            return Ok(Ok(None));
        }
        let Ok(symbol) = symbol_of(&mut self.memo.namer, Entity::MethodTable(ty)) else {
            return Ok(Err("a value of a type without a symbol in an interface"));
        };
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(Ok(Some(known)));
        }
        methods.sort();
        let bytes = self.bytes_ty();
        let word = self.b.types().int(64, false);
        let mut parts = vec![Constant {
            kind: ConstantKind::Int(methods.len() as u64),
            ty: word,
        }];
        for (key, thunk) in &methods {
            if let Operand::Const(key) = self.b.const_str(key) {
                parts.push(key);
            }
            parts.push(Constant {
                kind: ConstantKind::Func(*thunk),
                ty: self.b.module().funcs[*thunk].sig(),
            });
        }
        let types = self.b.types();
        let string = types.intern(Type::String);
        let entry = types.tuple(&[string, bytes]);
        let all = types.intern(Type::Array(entry, methods.len() as u64));
        let record = types.tuple(&[word, all]);
        Ok(Ok(Some(self.said(symbol, record, parts))))
    }

    /// A global of the module of the run that the program never writes
    /// and every package that needs it says, with its static data.
    fn said(&mut self, symbol: String, ty: TypeId, parts: Vec<Constant>) -> GlobalId {
        let id = self.b.module().globals.alloc(Global {
            name: symbol.clone(),
            ty,
            link: Linkage::LinkOnceOdr,
            mutable: false,
            parts,
        });
        self.memo.globals.insert(symbol, id);
        id
    }

    /// The methods of the interface type, said in the module of the
    /// run for what the runtime is asked of a value while the program
    /// runs: the name of the type, and the key of each method in the
    /// order of the tables of the interface.
    fn itype(&mut self, iface: CheckedTypeId) -> Built<Result<GlobalId, &'static str>> {
        if let Some(&known) = self.memo.itypes.get(&iface) {
            return Ok(known);
        }
        let made = self.itype_new(iface);
        self.memo.itypes.insert(iface, made);
        Ok(made)
    }

    fn itype_new(&mut self, iface: CheckedTypeId) -> Result<GlobalId, &'static str> {
        let shared = self.shared;
        let Some(methods) = shared.types.interface_methods(iface, shared.names) else {
            return Err("an interface of this kind as the type of a value");
        };
        let Ok(symbol) = symbol_of(&mut self.memo.namer, Entity::IfaceDesc(iface)) else {
            return Err("an interface type without a symbol");
        };
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(known);
        }
        let name = shared.types.runtime_name(iface, shared.names);
        let mut texts = vec![name.into_bytes()];
        for method in &methods {
            texts.push(self.method_key(method)?);
        }
        let word = self.b.types().int(64, false);
        let mut parts = Vec::with_capacity(texts.len() + 1);
        for (at, text) in texts.iter().enumerate() {
            if at == 1 {
                parts.push(Constant {
                    kind: ConstantKind::Int(methods.len() as u64),
                    ty: word,
                });
            }
            if let Operand::Const(text) = self.b.const_str(text) {
                parts.push(text);
            }
        }
        if methods.is_empty() {
            parts.push(Constant {
                kind: ConstantKind::Int(0),
                ty: word,
            });
        }
        let types = self.b.types();
        let string = types.intern(Type::String);
        let keys = types.intern(Type::Array(string, methods.len() as u64));
        let record = types.tuple(&[string, word, keys]);
        Ok(self.said(symbol, record, parts))
    }

    /// How a place of the type ends its value, without a report: for
    /// what the package makes.
    fn ends_quietly(&mut self, ty: CheckedTypeId) -> Built<Result<Ends, &'static str>> {
        let shared = self.shared;
        let lowered = self.memo.lower_type(shared, self.b.types(), ty);
        if let Ok(Type::String) = lowered.map(|lowered| self.kind(lowered)) {
            return Ok(Ok(Ends::Text));
        }
        Ok(self.glue(Glue::Drop, ty)?.map(Ends::Glue))
    }

    /// The body of what a table of methods holds for the method `name`
    /// of the declared type `ty`. It is given the address of the second
    /// word of the interface value: that is where the value is if the
    /// table is that of the type and the value is in the word itself,
    /// and what the word points to otherwise, in a box or where a
    /// pointer that is held points. A method that takes its receiver by
    /// value gets the value there, or a copy if it does more than read
    /// it. Every other argument is the callee's own (PAR-10), so what
    /// the method only reads ends here.
    pub fn thunk_body(
        &mut self,
        ty: CheckedTypeId,
        name: Spur,
        pointer: bool,
    ) -> Built<Result<(), &'static str>> {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let called = match self.callee(CalleeKey::Method(ty, name))? {
            Ok(called) => called,
            Err(_) => return Ok(Err("a method of another package in an interface")),
        };
        let lowered = match self.memo.lower_type(shared, self.b.types(), ty) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let word = Operand::Copy(Place::local(self.b.param(0)));
        let to = self.b.types().ptr(lowered);
        let at = match !pointer && self.inline(lowered) {
            true => self.slot_of(Rvalue::Cast(CastKind::PtrToPtr, word, to))?,
            false => {
                let to = self.b.types().ptr(to);
                let typed = self
                    .b
                    .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, word, to))?;
                let held = self.b.deref(&Place::local(typed))?;
                self.slot_of(Rvalue::Use(Operand::Copy(held)))?
            }
        };
        let Some(&wanted) = called.params.first() else {
            return Ok(Err("a method without a receiver"));
        };
        let receiver = if wanted == to {
            Operand::Copy(at)
        } else if wanted == lowered {
            // A pointer that is held may be nil in a Go file.
            if pointer {
                let null = self.zero(to);
                let set = Rvalue::Binary(BinOp::Ne, Operand::Copy(at.clone()), null);
                let set = Operand::Copy(Place::local(self.b.assign_temp(set)?));
                let next = self.b.new_block();
                let nothing = (AssertKind::Nil, Vec::new());
                self.b
                    .assert(set, true, nothing, next, UnwindAction::Continue)?;
                self.b.switch_to(next);
            }
            let value = Operand::Copy(self.b.deref(&at)?);
            let own = !self.flat(lowered) && called.lent.first() != Some(&true);
            match own {
                false => value,
                true => {
                    let copy = self.slot_of(Rvalue::Use(value))?;
                    match self.kind(lowered) {
                        Type::String => self.counted(copy.clone(), true)?,
                        _ => match self.glue(Glue::Clone, ty)? {
                            Ok(glue) => {
                                let at = self.record(copy.clone())?;
                                self.call_glue(glue, at)?;
                            }
                            Err(what) => return Ok(Err(what)),
                        },
                    }
                    Operand::Copy(copy)
                }
            }
        } else {
            return Ok(Err("a method with a receiver of this kind in an interface"));
        };
        let mut args = vec![receiver];
        for at in 1..called.params.len() {
            args.push(Operand::Copy(Place::local(self.b.param(at as u32))));
        }
        let callee = Operand::Const(Constant {
            kind: ConstantKind::Func(called.func),
            ty: called.sig,
        });
        let result = Place::local(self.b.temp(called.ret));
        let next = self.b.new_block();
        let unwind = UnwindAction::Continue;
        self.b
            .call(callee, args, result.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        let method = match &*shared.types.get(ty) {
            CheckedType::Named { id, .. } => shared.methods.get(&(*id, name)),
            _ => None,
        };
        let params = method.map(|method| shared.types.get(method.sig));
        if let Some(CheckedType::Func { params, .. }) = params.as_deref() {
            for (at, &param) in params.iter().enumerate() {
                let at = at + 1;
                let read = called.lent.get(at) == Some(&true);
                let Some(&lowered) = called.params.get(at).filter(|_| read) else {
                    continue;
                };
                if self.flat(lowered) {
                    continue;
                }
                match self.ends_quietly(param)? {
                    Ok(ends) => self.end(Place::local(self.b.param(at as u32)), ends)?,
                    Err(what) => return Ok(Err(what)),
                }
            }
        }
        if called.ret != TypeInterner::UNIT {
            let whole = Place::local(self.b.result());
            self.b.assign(whole, Rvalue::Use(Operand::Copy(result)))?;
        }
        self.b.ret()?;
        Ok(Ok(()))
    }

    /// The method of an interface value that the selector of a call
    /// names, if it names one.
    pub fn iface_method(&self, callee: ExprId) -> Option<IfaceMethod<'a>> {
        let ExprKind::Selector { x, sel } = &self.tree.exprs[callee].kind else {
            return None;
        };
        let selection = self.body.selections.get(&callee)?;
        let dynamic = selection.kind == SelectionKind::Method
            && self.shared.types.is_interface(selection.holder);
        dynamic.then_some((*x, &selection.path[..], selection.holder, sel.name))
    }

    /// What a call of a method of an interface value calls, and the
    /// first argument of the call: the address of the second word of
    /// the interface value, which stays where it is. The table of the
    /// value is read when the arguments are worked out, so the code
    /// that is called is that of the value that the word is of. Every
    /// argument is the callee's own: nothing is known of the method
    /// that runs (PAR-10).
    pub fn iface_called(
        &mut self,
        span: SourceSpan,
        (x, path, iface, name): IfaceMethod<'_>,
        args: &[ExprId],
        spread: bool,
    ) -> Built<Result<(Called, Vec<Operand>), &'static str>> {
        if spread {
            return Ok(Err("a call with `...`"));
        }
        let shared = self.shared;
        let types = shared.types;
        let Some(methods) = types.interface_methods(iface, shared.names) else {
            return Ok(Err("a method of an interface of this kind"));
        };
        let Some(slot) = methods.iter().position(|method| method.name == name) else {
            return Ok(Err("a method that its interface does not have"));
        };
        let checked = types.get(methods[slot].sig);
        let declared = match &*checked {
            CheckedType::Func { variadic: true, .. } => {
                return Ok(Err("a call of a function with `...`"));
            }
            CheckedType::Func { params, .. } => params.len(),
            _ => return Ok(Err("a function type")),
        };
        let sig = self
            .memo
            .signature(shared, self.b.types(), None, methods[slot].sig);
        let sig = match sig.map(|sig| self.kind(sig)) {
            Ok(Type::Func(sig)) => sig,
            Ok(_) => return Ok(Err("a function type")),
            Err(what) => return Ok(Err(what)),
        };
        let bytes = self.bytes_ty();
        let mut params = vec![bytes];
        params.extend(sig.params.iter());
        let code = self.b.types().intern(Type::Func(FuncType {
            params: params.clone().into(),
            ret: sig.ret,
            has_env: false,
            cc: CallConv::Go,
        }));
        // An argument may change the variable that holds the value.
        let mut mode = PlaceMode {
            held: args.iter().any(|&arg| self.has_effects(arg)),
            ..PlaceMode::default()
        };
        let base = self.place(x, &mut mode)?;
        let value = self.steps(base, path, span, &mut mode)?;
        let value = self.through(value, span, &mut mode)?;
        if !self.is_iface(self.b.place_ty(&value)) {
            return Ok(Err("a method of an interface of this kind"));
        }
        let word = self.b.field(&value, 1)?;
        let word = self.record(word)?;
        let nilable = types.has_nil(iface, self.source.dialect());
        let called = Called {
            code: Code::Slot(value, slot as u32, code, nilable),
            views: sig.params.len() - declared,
            lent: std::rc::Rc::from([]),
            params: params.into(),
            ret: sig.ret,
        };
        Ok(Ok((called, vec![word])))
    }

    /// The code of a method of the interface value in the place, read
    /// from its table of methods; the program ends as in Go if the
    /// value may be nil and is.
    pub fn table_slot(
        &mut self,
        span: SourceSpan,
        value: &Place,
        slot: u32,
        code: TypeId,
        nilable: bool,
    ) -> Built<Operand> {
        let first = Operand::Copy(self.b.field(value, 0)?);
        let first = Operand::Copy(self.slot_of(Rvalue::Use(first))?);
        if nilable {
            self.checked(Check::Nil(first.clone(), span))?;
        }
        // The table is as long as the interface has methods; it is read
        // as one that ends with this method.
        let bytes = self.bytes_ty();
        let table = Type::Array(bytes, u64::from(slot) + 2);
        let table = self.b.types().intern(table);
        let to = self.b.types().ptr(table);
        let table = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, first, to))?;
        let table = self.b.deref(&Place::local(table))?;
        let entry = Operand::Copy(self.b.const_index(&table, u64::from(slot) + 1)?);
        let cast = Rvalue::Cast(CastKind::PtrToPtr, entry, code);
        Ok(Operand::Copy(Place::local(self.b.assign_temp(cast)?)))
    }

    /// The interface value of the expression for a place of the
    /// interface type `to`, which has other methods: the value that is
    /// held stays what it is, and the first word is its table for the
    /// methods of `to`, which the runtime finds.
    pub fn iface_change(&mut self, expr: ExprId, ty: TypeId, to: CheckedTypeId) -> Built<Rvalue> {
        let value = self.rvalue_plain(expr, ty)?;
        self.changed(self.tree.exprs[expr].span, ty, to, value)
    }

    /// The interface value for a place of the interface type `to`,
    /// which has other methods than its own type.
    pub fn changed(
        &mut self,
        span: SourceSpan,
        ty: TypeId,
        to: CheckedTypeId,
        value: Rvalue,
    ) -> Built<Rvalue> {
        // A value of an interface without methods says its type by
        // any first word.
        if self.empty_iface(to) {
            return Ok(value);
        }
        let itype = match self.itype(to)? {
            Ok(itype) => itype,
            Err(what) => return Ok(self.placeholder(span, ty, what)),
        };
        // The value where it is: a temporary that owns it is known by
        // its place.
        let value = match value {
            Rvalue::Use(Operand::Copy(place)) => place,
            other => self.slot_of(other)?,
        };
        // The table of the value for the methods of the place: the
        // checker saw that every value of the type has them.
        let bytes = self.bytes_ty();
        let first = Operand::Copy(self.b.field(&value, 0)?);
        let args = vec![first, self.info_word(itype)?];
        let table = self.runtime("_gopp_iface_table", &[bytes, bytes], bytes, args)?;
        let data = Operand::Copy(self.b.field(&value, 1)?);
        let changed = self.slot_of(Rvalue::Aggregate(AggKind::Of(ty), vec![table, data]))?;
        // What owned the value gives it to the new words.
        if self.take_temp(&value) {
            let drop = self.iface_glue(Glue::Drop)?;
            self.own_temp(changed.clone(), Ends::Glue(drop));
        }
        Ok(Rvalue::Use(Operand::Copy(changed)))
    }
}
