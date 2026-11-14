//! Type tables (TYP-22, EXP-13, EXP-14). A value of a type with a
//! method that may be replaced starts with the address of a table, and
//! so does the part of each parent with such methods that is not at the
//! start of the value. A table says where its part is in the whole
//! value, and then has the code of each method that may be replaced.
//!
//! A value that is made all zero has no addresses yet. They are set
//! where they are first needed: before a call through a table, and
//! before a pointer to the value becomes one to a part of it. A value
//! without them is of the type that the code names, since a pointer to
//! a part of a larger value is made from one to the larger value.

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::SelectionKind;
use gopp_sema::types::{
    CopyRule, Marks, Member, PARENT, Type as CheckedType, TypeId as CheckedTypeId,
};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{ExprId, ExprKind};
use lasso::Spur;

use super::expr::{Called, Code, PlaceMode, Target};
use super::func::{Built, FuncCx};
use super::glue::Glue;
use super::iface::{Looked, Switched};
use super::{CalleeKey, Made, Memo, Shared};
use crate::{
    BinOp, CallConv, CastKind, Constant, ConstantKind, FuncType, GlobalId, Mutability, Operand,
    Place, Rvalue, Type, TypeId, TypeInterner,
};
use gopp_diag::{Code as Fault, Diagnostic};

impl Shared<'_> {
    /// Whether the method of the type may be replaced by a child, or
    /// replaces one: a call through a pointer runs what the table has.
    pub fn replaced(&self, holder: CheckedTypeId, name: Spur) -> bool {
        let marks = self.marks_of(holder, name);
        marks.has(Marks::VIRTUAL) || marks.has(Marks::ABSTRACT) || marks.has(Marks::OVERRIDE)
    }

    /// The type of the method `name` that the type `holder` declares
    /// with a body, or as one that a child gives a body.
    pub fn slot_method(&self, holder: CheckedTypeId, name: Spur) -> Option<CheckedTypeId> {
        if let Some(method) = self.method(holder, name) {
            return Some(method.sig);
        }
        if !self.marks_of(holder, name).has(Marks::ABSTRACT) {
            return None;
        }
        let package = match &*self.types.get(holder) {
            CheckedType::Named { id, .. } => self.types.home(*id).path,
            _ => None,
        };
        let package = package.unwrap_or(self.path);
        let found = self.types.lookup(holder, name, package, self.names, false);
        match found.ok()?.member {
            Member::Method { sig, .. } => Some(sig),
            Member::Field { .. } => None,
        }
    }

    /// The marks of the method `name` that the type `holder` declares.
    fn marks_of(&self, holder: CheckedTypeId, name: Spur) -> Marks {
        let exported = self.names.resolve(&name).starts_with(char::is_uppercase);
        let package = match &*self.types.get(holder) {
            CheckedType::Named { id, .. } => self.types.home(*id).path,
            _ => None,
        };
        self.types
            .method_marks(holder, name, package.filter(|_| !exported))
    }

    /// The methods that a table of the type has a place for, in their
    /// order: those of its first parent if that has a table, which the
    /// two share, then the methods that the type itself declares as
    /// ones that may be replaced.
    pub fn slots(&self, ty: CheckedTypeId) -> Vec<Spur> {
        let first = self.types.parents(ty).first().copied();
        let mut slots = match first.filter(|&first| self.types.has_table(first)) {
            Some(first) => self.slots(first),
            None => Vec::new(),
        };
        if let CheckedType::Named { id, .. } = &*self.types.get(ty) {
            for declared in self.types.declared_methods(*id) {
                let marks = declared.marks;
                if marks.has(Marks::VIRTUAL) || marks.has(Marks::ABSTRACT) {
                    slots.push(declared.method.name);
                }
            }
        }
        slots
    }

    /// Where a call of the method through a pointer to the type finds
    /// its code: the way to the part whose table has it, and its place
    /// in that table.
    pub fn slot_of(&self, ty: CheckedTypeId, name: Spur) -> Option<(Vec<u32>, usize)> {
        if let Some(at) = self.slots(ty).iter().position(|&slot| slot == name) {
            return Some((Vec::new(), at));
        }
        // The parts with tables of their own: every parent after the
        // first, of the type and of the first parents below it.
        let (mut way, mut current) = (Vec::new(), ty);
        loop {
            let parents = self.types.parents(current);
            for (at, &parent) in parents.iter().enumerate().skip(1) {
                if !self.types.has_table(parent) {
                    continue;
                }
                if let Some((rest, slot)) = self.slot_of(parent, name) {
                    way.push(PARENT | at as u32);
                    way.extend(rest);
                    return Some((way, slot));
                }
            }
            match parents.first() {
                Some(&first) if self.types.has_table(first) => {
                    way.push(PARENT);
                    current = first;
                }
                _ => return None,
            }
        }
    }

    /// The type and each of its ancestors with a table, with the way
    /// to its part in a value of the type: what a pointer to the value
    /// may be turned into (EXP-15).
    pub fn parts_of(&self, ty: CheckedTypeId) -> Vec<(Vec<u32>, CheckedTypeId)> {
        let mut found = vec![(Vec::new(), ty)];
        let mut at = 0;
        while let Some((way, of)) = found.get(at).cloned() {
            at += 1;
            for (index, parent) in self.types.parents(of).into_iter().enumerate() {
                if self.types.has_table(parent) {
                    let mut to = way.clone();
                    to.push(PARENT | index as u32);
                    found.push((to, parent));
                }
            }
        }
        found
    }

    /// The parts of a value of the type that start with the address of
    /// a table of their own: the way to each and its type, the value
    /// itself first.
    pub fn tables_of(&self, ty: CheckedTypeId) -> Vec<(Vec<u32>, CheckedTypeId)> {
        let mut found = Vec::new();
        self.tables_in(ty, Vec::new(), &mut found);
        found
    }

    fn tables_in(
        &self,
        ty: CheckedTypeId,
        mut way: Vec<u32>,
        found: &mut Vec<(Vec<u32>, CheckedTypeId)>,
    ) {
        found.push((way.clone(), ty));
        let mut current = ty;
        loop {
            let parents = self.types.parents(current);
            for (at, &parent) in parents.iter().enumerate().skip(1) {
                if self.types.has_table(parent) {
                    let mut to = way.clone();
                    to.push(PARENT | at as u32);
                    self.tables_in(parent, to, found);
                }
            }
            match parents.first() {
                Some(&first) if self.types.has_table(first) => {
                    way.push(PARENT);
                    current = first;
                }
                _ => return,
            }
        }
    }
}

/// The type of what a table has for a method of the type `sig`: the
/// method with the address of the part that the table is of in the
/// place of its receiver.
pub(super) fn slot_sig(
    shared: &Shared<'_>,
    memo: &mut Memo<'_>,
    types: &mut TypeInterner,
    sig: CheckedTypeId,
) -> Result<TypeId, &'static str> {
    let plain = memo.signature(shared, types, None, sig)?;
    let Type::Func(plain) = types.get(plain).clone() else {
        return Err("a function type");
    };
    let byte = types.int(8, false);
    let mut params = vec![types.ptr(byte)];
    params.extend(plain.params.iter());
    Ok(types.intern(Type::Func(FuncType {
        params: params.into(),
        ret: plain.ret,
        has_env: false,
        cc: CallConv::Go,
    })))
}

impl FuncCx<'_, '_> {
    /// How many bytes after the start of a value of the struct `ty` the
    /// part at the end of the way starts.
    fn part_offset(&mut self, mut ty: TypeId, way: &[u32]) -> Result<u64, &'static str> {
        let mut offset = 0;
        for &step in way {
            let Type::Struct(id) = self.kind(ty) else {
                return Err("a part of what is no struct");
            };
            let fields = self
                .b
                .types()
                .struct_fields(id)
                .unwrap_or_default()
                .to_vec();
            let at = usize::from(self.memo.tabled.contains(&ty)) + (step & !PARENT) as usize;
            let layouts: Result<Vec<_>, _> = fields
                .iter()
                .map(|&field| self.b.types().layout(field, self.shared.layout))
                .collect();
            let starts = layouts
                .ok()
                .and_then(|all| self.shared.layout.aggregate(&all));
            let (Some((_, starts)), Some(&part)) = (starts, fields.get(at)) else {
                return Err("a part of a value whose size is not known");
            };
            offset += starts[at];
            ty = part;
        }
        Ok(offset)
    }

    /// The record of a type with a table: its name, the size of a
    /// value and the glue that ends it and makes it a copy of its own,
    /// and the parts of its ancestors that a pointer to a value of it
    /// may be turned into, each by the record of its type and by where
    /// it starts. The address of the record says the type where the
    /// program runs.
    pub(super) fn whole(&mut self, ty: CheckedTypeId) -> Built<Result<GlobalId, &'static str>> {
        let shared = self.shared;
        let entity = Entity::PartTable {
            child: ty,
            parent: ty,
        };
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            return Ok(Err("a record of a type without a symbol"));
        };
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(Ok(known));
        }
        let lowered = match self.memo.lower_type(shared, self.b.types(), ty) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let found = shared.parts_of(ty);
        let (word, offset) = (self.b.types().int(64, false), self.b.types().int(64, true));
        let name = shared.types.runtime_name(ty, shared.names);
        let Operand::Const(name) = self.b.const_str(name.as_bytes()) else {
            return Ok(Err("a name of a type"));
        };
        let int = |value: u64| Constant {
            kind: ConstantKind::Int(value),
            ty: word,
        };
        // No value is of a type that leaves a method open: nothing asks
        // what ends one.
        let open = shared.types.is_abstract(ty, shared.names);
        let size = self.b.types().layout(lowered, shared.layout);
        let mut parts = vec![name, int(size.map_or(0, |size| size.size))];
        for kind in [Glue::Drop, Glue::Clone] {
            let copied = shared.types.copy_rule(ty);
            let by_bytes = match kind {
                Glue::Clone => matches!(copied, None | Some(CopyRule::Bytes)),
                _ => false,
            };
            let made = match open || by_bytes || self.flat(lowered) {
                true => None,
                false => match self.glue(kind, ty)? {
                    Ok(glue) => Some(glue),
                    Err(what) => return Ok(Err(what)),
                },
            };
            parts.push(Constant {
                kind: made.map_or(ConstantKind::Zero, ConstantKind::Func),
                ty: self.glue_sig(kind),
            });
        }
        parts.push(int(found.len() as u64 - 1));
        // The type itself comes first in `found`, and has no entry.
        for (way, part) in found.iter().skip(1) {
            self.memo.steps += 1;
            let said = match self.whole(*part)? {
                Ok(said) => said,
                Err(what) => return Ok(Err(what)),
            };
            let start = match self.part_offset(lowered, way) {
                Ok(start) => start,
                Err(what) => return Ok(Err(what)),
            };
            let record = self.b.module().globals[said].ty;
            parts.push(Constant {
                kind: ConstantKind::Global(said),
                ty: self.b.types().ptr(record),
            });
            parts.push(Constant {
                kind: ConstantKind::Int(start),
                ty: offset,
            });
        }
        let of: Vec<TypeId> = parts.iter().map(|part| part.ty).collect();
        let record = self.b.types().tuple(&of);
        Ok(Ok(self.said(symbol, record, parts)))
    }

    /// `p.(*T)` for a pointer or a handle `p` to a value with a table
    /// (EXP-15): the address of the part of `T` in the value, and
    /// whether the value has one. With `must`, the program ends if it
    /// has none. `None` if `p` is no such pointer.
    pub(super) fn part_asserted(
        &mut self,
        expr: ExprId,
        must: bool,
    ) -> Built<Option<Result<[Operand; 2], &'static str>>> {
        let shared = self.shared;
        let types = shared.types;
        let ExprKind::Assert { x, .. } = self.tree.exprs[expr].kind else {
            return Ok(None);
        };
        let pointee = |ty: CheckedTypeId| match &*types.get(types.underlying(ty)) {
            CheckedType::Pointer(to) | CheckedType::Unique(to) | CheckedType::Shared(to) => {
                Some(*to)
            }
            _ => None,
        };
        let Some(from) = self.body.types.get(&x).map(|typed| typed.ty) else {
            return Ok(None);
        };
        if !pointee(from).is_some_and(|of| types.has_table(of)) {
            return Ok(None);
        }
        let Some(typed) = self.body.types.get(&expr) else {
            return Ok(Some(Err("a value whose type is not known")));
        };
        let wanted = match &*types.get(typed.ty) {
            CheckedType::Tuple(parts) if parts.len() == 2 => parts[0],
            _ => typed.ty,
        };
        let Some(target) = pointee(wanted) else {
            return Ok(Some(Err("a pointer conversion to what is no pointer")));
        };
        let span = self.tree.exprs[expr].span;
        let ty = self.lower(wanted, span);
        let mut mode = PlaceMode::default();
        let base = self.place(x, &mut mode)?;
        let value = self.through(base, &mut mode)?;
        let record = match self.whole(target)? {
            Ok(record) => record,
            Err(what) => return Ok(Some(Err(what))),
        };
        let bytes = self.bytes_ty();
        let at = self.b.assign_temp(Rvalue::AddrOf(value, Mutability::Mut))?;
        let at = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(Place::local(at)), bytes);
        let at = Operand::Copy(Place::local(self.b.assign_temp(at)?));
        let record = self.info_word(record)?;
        let args = vec![at.clone(), record.clone()];
        let found = self.runtime("_gopp_part_of", &[bytes, bytes], bytes, args)?;
        let found = self.stable(found)?;
        let none = self.zero(bytes);
        let has = Rvalue::Binary(BinOp::Ne, found.clone(), none);
        let has = Operand::Copy(Place::local(self.b.assign_temp(has)?));
        if must {
            let [fails, held] = [(); 2].map(|()| self.b.new_block());
            self.b.switch(has.clone(), vec![(0, fails)], held)?;
            self.b.switch_to(fails);
            let around = self.b.loc();
            self.at(span);
            let name = types.runtime_name(from, shared.names);
            let name = self.b.const_str(name.as_bytes());
            let string = self.b.operand_ty(&name);
            let args = vec![at, record, name];
            self.runtime_call("_gopp_panic_part", &[bytes, bytes, string], args)?;
            self.b.unreachable()?;
            self.b.set_loc(around);
            self.b.switch_to(held);
        }
        let part = Rvalue::Cast(CastKind::PtrToPtr, found, ty);
        let part = Operand::Copy(Place::local(self.b.assign_temp(part)?));
        Ok(Some(Ok([part, has])))
    }

    /// A type switch on a pointer or a handle to a value with a table
    /// (EXP-15): each case asks for the part of its type in the value,
    /// and the first that the value has runs. With the pointer come,
    /// for each clause, the address of the part that its last case
    /// found. `None` if the switch looks into no such pointer.
    pub(super) fn part_switch(
        &mut self,
        x: ExprId,
        clauses: &[gopp_syntax::ast::CaseClause],
        bodies: &[crate::BlockId],
        otherwise: crate::BlockId,
    ) -> Built<Option<Result<Switched, &'static str>>> {
        let shared = self.shared;
        let types = shared.types;
        let pointee = |ty: CheckedTypeId| match &*types.get(types.underlying(ty)) {
            CheckedType::Pointer(to) | CheckedType::Unique(to) | CheckedType::Shared(to) => {
                Some(*to)
            }
            _ => None,
        };
        let Some(checked) = self.body.types.get(&x).map(|typed| typed.ty) else {
            return Ok(None);
        };
        if !pointee(checked).is_some_and(|of| types.has_table(of)) {
            return Ok(None);
        }
        let mut mode = PlaceMode::default();
        let base = self.place(x, &mut mode)?;
        let value = self.through(base, &mut mode)?;
        let bytes = self.bytes_ty();
        let pointer = self.b.assign_temp(Rvalue::AddrOf(value, Mutability::Mut))?;
        let pointer = Place::local(pointer);
        let at = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(pointer.clone()), bytes);
        let at = Operand::Copy(Place::local(self.b.assign_temp(at)?));
        let mut found = Vec::with_capacity(clauses.len());
        for (clause, &body) in clauses.iter().zip(bodies) {
            let mut last = None;
            for &case in clause.cases.iter().flatten() {
                let Some(target) = self.body.case_types.get(&case).and_then(|&ty| pointee(ty))
                else {
                    return Ok(Some(Err(
                        "a case of a switch on a pointer that is no pointer",
                    )));
                };
                let record = match self.whole(target)? {
                    Ok(record) => self.info_word(record)?,
                    Err(what) => return Ok(Some(Err(what))),
                };
                let args = vec![at.clone(), record];
                let part = self.runtime("_gopp_part_of", &[bytes, bytes], bytes, args)?;
                let part = self.stable(part)?;
                let none = self.zero(bytes);
                let has = Rvalue::Binary(BinOp::Ne, part.clone(), none);
                let has = Operand::Copy(Place::local(self.b.assign_temp(has)?));
                last = Some(part);
                let next = self.b.new_block();
                self.b.switch(has, vec![(0, next)], body)?;
                self.b.switch_to(next);
            }
            found.push(last);
        }
        self.b.goto(otherwise)?;
        let looked = Looked {
            value: pointer,
            first: at.clone(),
            info: at,
            checked,
        };
        Ok(Some(Ok((looked, found))))
    }

    /// The table of the part at the end of `way` of a value of the type
    /// `whole`; the part is of the type `head`. It says how far the
    /// part is from the start of the value and names the record of
    /// `whole`, and has for each method of `head` that may be replaced
    /// the code that runs for a value of `whole`: null for a method
    /// without a body.
    fn table(
        &mut self,
        whole: CheckedTypeId,
        way: &[u32],
        head: CheckedTypeId,
    ) -> Built<Result<GlobalId, &'static str>> {
        let shared = self.shared;
        let types = shared.types;
        let entity = match way.is_empty() {
            true => Entity::Table(whole),
            false => Entity::PartTable {
                child: whole,
                parent: head,
            },
        };
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            return Ok(Err("a table of a type without a symbol"));
        };
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(Ok(known));
        }
        let lowered = match self.memo.lower_type(shared, self.b.types(), whole) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let start = match self.part_offset(lowered, way) {
            Ok(start) => start as i64,
            Err(what) => return Ok(Err(what)),
        };
        let package = match &*types.get(whole) {
            CheckedType::Named { id, .. } => types.home(*id).path,
            _ => None,
        };
        let package = package.unwrap_or(shared.path);
        let bytes = self.bytes_ty();
        let offset = self.b.types().int(64, true);
        let record = match self.whole(whole)? {
            Ok(record) => record,
            Err(what) => return Ok(Err(what)),
        };
        let said = self.b.module().globals[record].ty;
        let said = self.b.types().ptr(said);
        let mut parts = vec![
            Constant {
                kind: ConstantKind::Int((-start) as u64),
                ty: offset,
            },
            Constant {
                kind: ConstantKind::Global(record),
                ty: said,
            },
        ];
        for name in shared.slots(head) {
            self.memo.steps += 1;
            let Ok(found) = types.lookup(whole, name, package, shared.names, false) else {
                return Ok(Err("a method that its type does not have"));
            };
            if !matches!(found.member, Member::Method { .. }) {
                return Ok(Err("a method that its type does not have"));
            }
            let Some(method) = shared.method(found.holder, name) else {
                // A method without a body is never called: no value is
                // of a type that leaves one open.
                parts.push(Constant {
                    kind: ConstantKind::Zero,
                    ty: bytes,
                });
                continue;
            };
            let sig = match slot_sig(shared, self.memo, self.b.types(), method.sig) {
                Ok(sig) => sig,
                Err(what) => return Ok(Err(what)),
            };
            let at = match self.part_offset(lowered, &found.path) {
                Ok(at) => at as i64,
                Err(what) => return Ok(Err(what)),
            };
            let by = at - start;
            let entity = Entity::Adjusted {
                receiver: found.holder,
                name,
                by,
            };
            let Ok(code) = symbol_of(&mut self.memo.namer, entity) else {
                return Ok(Err("a method without a symbol in a table"));
            };
            let asked = self.memo.funcs.contains_key(&code);
            let made = self.function_named(&code, sig)?;
            if !asked {
                let adjusted = Made::Adjusted { name, by };
                self.memo.glues.push((code, adjusted, found.holder));
            }
            parts.push(Constant {
                kind: ConstantKind::Func(made),
                ty: sig,
            });
        }
        let of: Vec<TypeId> = parts.iter().map(|part| part.ty).collect();
        let record = self.b.types().tuple(&of);
        Ok(Ok(self.said(symbol, record, parts)))
    }

    /// The first word of the struct at the place: the address of its
    /// table.
    fn table_word(&mut self, of: &Place) -> Built<Place> {
        let bytes = self.bytes_ty();
        let to = self.b.types().ptr(bytes);
        let at = self
            .b
            .assign_temp(Rvalue::AddrOf(of.clone(), Mutability::Mut))?;
        let at = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(Place::local(at)), to);
        let at = self.b.assign_temp(at)?;
        self.b.deref(&Place::local(at))
    }

    /// Writes the addresses of the tables of the type `checked` into
    /// the value at the place: a value that was put together from
    /// parts, each of which has those of its own type.
    pub(super) fn put_tables(
        &mut self,
        value: &Place,
        checked: CheckedTypeId,
    ) -> Built<Result<(), &'static str>> {
        let shared = self.shared;
        let bytes = self.bytes_ty();
        for (way, head) in shared.tables_of(checked) {
            let table = match self.table(checked, &way, head)? {
                Ok(table) => table,
                Err(what) => return Ok(Err(what)),
            };
            let mut part = value.clone();
            for &step in &way {
                part = self.part(&part, step)?;
            }
            let word = self.table_word(&part)?;
            let record = self.b.module().globals[table].ty;
            let address = Operand::Const(Constant {
                kind: ConstantKind::Global(table),
                ty: self.b.types().ptr(record),
            });
            self.b
                .assign(word, Rvalue::Cast(CastKind::PtrToPtr, address, bytes))?;
        }
        Ok(Ok(()))
    }

    /// Whether a value of the type holds a value with tables: by
    /// itself, in a field or in an element, never behind a pointer.
    pub(super) fn holds_tables(&mut self, ty: TypeId) -> bool {
        if let Some(&known) = self.memo.holds_tables.get(&ty) {
            return known;
        }
        let parts = match self.kind(ty) {
            Type::Struct(id) => (self.b.types().struct_fields(id).unwrap_or_default()).to_vec(),
            Type::Tuple(parts) => parts.to_vec(),
            Type::Array(of, count) if count > 0 => vec![of],
            _ => Vec::new(),
        };
        let own = self.memo.with_tables.contains_key(&ty);
        // A struct in a struct in a struct: as deep as the program
        // declares them.
        let holds = own
            || stacker::maybe_grow(64 * 1024, 1024 * 1024, || {
                parts.into_iter().any(|part| self.holds_tables(part))
            });
        self.memo.holds_tables.insert(ty, holds);
        holds
    }

    /// How many leaves a value of the type has (`TypeInterner::leaves`).
    fn leaf_count(&mut self, ty: TypeId) -> Option<usize> {
        let leaves = self.b.types().leaves(ty, crate::verify::MAX_LEAVES);
        leaves.map(|leaves| leaves.len())
    }

    /// Which leaves of a new value of the type are the addresses of
    /// tables, counted from `base`: for each the table.
    fn table_leaves(
        &mut self,
        ty: TypeId,
        base: usize,
        out: &mut std::collections::BTreeMap<usize, GlobalId>,
    ) -> Built<Result<(), &'static str>> {
        if !self.holds_tables(ty) {
            return Ok(Ok(()));
        }
        let too_many = "a value of more parts than static data has that holds values with tables";
        let parts: Vec<TypeId> = match self.kind(ty) {
            Type::Struct(id) => (self.b.types().struct_fields(id).unwrap_or_default()).to_vec(),
            Type::Tuple(parts) => parts.to_vec(),
            Type::Array(of, count) => {
                // Each element has the leaves of the first.
                let mut first = std::collections::BTreeMap::new();
                if let Err(what) = self.table_leaves(of, 0, &mut first)? {
                    return Ok(Err(what));
                }
                let Some(each) = self.leaf_count(of) else {
                    return Ok(Err(too_many));
                };
                for at in 0..count as usize {
                    out.extend(
                        first
                            .iter()
                            .map(|(leaf, table)| (base + at * each + leaf, *table)),
                    );
                }
                return Ok(Ok(()));
            }
            _ => Vec::new(),
        };
        // The fields, each with the tables of its own type; then the
        // tables of the type itself, which the parts of its parents
        // have in the place of their own.
        let mut starts = Vec::with_capacity(parts.len());
        let mut next = base;
        for &part in &parts {
            starts.push(next);
            let nested = stacker::maybe_grow(64 * 1024, 1024 * 1024, || {
                self.table_leaves(part, next, out)
            })?;
            if let Err(what) = nested {
                return Ok(Err(what));
            }
            let Some(leaves) = self.leaf_count(part) else {
                return Ok(Err(too_many));
            };
            next += leaves;
        }
        let Some(&checked) = self.memo.with_tables.get(&ty) else {
            return Ok(Ok(()));
        };
        for (way, head) in self.shared.tables_of(checked) {
            let table = match self.table(checked, &way, head)? {
                Ok(table) => table,
                Err(what) => return Ok(Err(what)),
            };
            // The part at the end of the way starts with the address.
            let (mut at, mut part) = (base, ty);
            for &step in &way {
                let Type::Struct(id) = self.kind(part) else {
                    return Ok(Err("a part of what is no struct"));
                };
                let fields = (self.b.types().struct_fields(id).unwrap_or_default()).to_vec();
                let field =
                    usize::from(self.memo.tabled.contains(&part)) + (step & !PARENT) as usize;
                for &before in fields.iter().take(field) {
                    let Some(leaves) = self.leaf_count(before) else {
                        return Ok(Err(too_many));
                    };
                    at += leaves;
                }
                let Some(&next) = fields.get(field) else {
                    return Ok(Err("a part that its type does not have"));
                };
                part = next;
            }
            out.insert(at, table);
        }
        Ok(Ok(()))
    }

    /// The global that a new value of the type starts as, if a value
    /// of the type holds a value with tables: all zero but for the
    /// addresses of the tables. A value has its tables from where it is
    /// made, so nothing writes them later into a value that is only
    /// read (TYP-22).
    pub(super) fn blank(&mut self, ty: TypeId) -> Option<GlobalId> {
        if let Some(&known) = self.memo.blanks.get(&ty) {
            return known;
        }
        let made = match self.blank_new(ty) {
            Ok(Ok(made)) => made,
            Ok(Err(what)) => {
                self.unmade(what);
                None
            }
            Err(error) => {
                self.unmade(&error.to_string());
                None
            }
        };
        self.memo.blanks.insert(ty, made);
        made
    }

    /// The address of what a new value of the type starts as, as the
    /// runtime takes it: none for a type whose new values are all
    /// zero.
    pub(super) fn blank_at(&mut self, ty: TypeId) -> Built<Option<Operand>> {
        let Some(blank) = self.blank(ty) else {
            return Ok(None);
        };
        let address = Operand::Const(Constant {
            kind: ConstantKind::Global(blank),
            ty: self.b.types().ptr(ty),
        });
        let bytes = self.bytes_ty();
        let at = Rvalue::Cast(CastKind::PtrToPtr, address, bytes);
        Ok(Some(Operand::Copy(Place::local(self.b.assign_temp(at)?))))
    }

    fn blank_new(&mut self, ty: TypeId) -> Built<Result<Option<GlobalId>, &'static str>> {
        if !self.holds_tables(ty) {
            return Ok(Ok(None));
        }
        let Some(leaves) = self.b.types().leaves(ty, crate::verify::MAX_LEAVES) else {
            let what = "a value of more parts than static data has that holds values with tables";
            return Ok(Err(what));
        };
        let mut tables = std::collections::BTreeMap::new();
        if let Err(what) = self.table_leaves(ty, 0, &mut tables)? {
            return Ok(Err(what));
        }
        let mut parts = Vec::with_capacity(leaves.len());
        for (at, leaf) in leaves.into_iter().enumerate() {
            self.memo.steps += 1;
            parts.push(match tables.get(&at) {
                Some(&table) => {
                    let record = self.b.module().globals[table].ty;
                    let kind = ConstantKind::Global(table);
                    let ty = self.b.types().ptr(record);
                    Constant { kind, ty }
                }
                None => Constant {
                    kind: ConstantKind::Zero,
                    ty: leaf,
                },
            });
        }
        // A symbol of the type as the module writes it, with what a
        // symbol does not have written as its number.
        let mut symbol = "gopp.blank.".to_owned();
        for letter in self.b.types().display(ty).bytes() {
            match letter.is_ascii_alphanumeric() || letter == b'_' || letter == b'.' {
                true => symbol.push(letter as char),
                false => symbol.push_str(&format!("${letter:02x}")),
            }
        }
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Ok(Ok(Some(known)));
        }
        Ok(Ok(Some(self.said(symbol, ty, parts))))
    }

    /// Says once that the value that a new value starts as could not
    /// be made, at the start of the package.
    fn unmade(&mut self, what: &str) {
        let (tree, _) = self.shared.files[0];
        let first = tree.decl_nodes.iter().next().map(|(_, decl)| decl.span);
        if let Some(span) = tree.package.map(|name| name.span).or(first) {
            let message = format!("not supported yet: {what}");
            let diagnostic = Diagnostic::new(Fault::GirUnsupported, span, message);
            self.memo.diagnostics.push(diagnostic);
        }
    }

    /// The call of a method that may be replaced, through a pointer or
    /// a handle (EXP-14): the code is read from the table of the part
    /// whose type declares the method first, and is given the address
    /// of that part. `None` if the call is no such call. Every argument
    /// is the callee's own: nothing is known of the method that runs.
    pub(super) fn table_called(
        &mut self,
        span: SourceSpan,
        callee: ExprId,
        args: &[ExprId],
        spread: bool,
    ) -> Built<Option<Target>> {
        let shared = self.shared;
        let types = shared.types;
        let ExprKind::Selector { x, sel } = self.tree.exprs[callee].kind else {
            return Ok(None);
        };
        let Some(selection) = self.body.selections.get(&callee) else {
            return Ok(None);
        };
        let through = selection.indirect && selection.kind == SelectionKind::Method;
        if !through || !shared.replaced(selection.holder, sel.name) {
            return Ok(None);
        }
        // What the pointer points to, as the code names it.
        let mut of = selection.recv;
        while let CheckedType::Pointer(to) | CheckedType::Unique(to) | CheckedType::Shared(to) =
            &*types.get(types.underlying(of))
        {
            of = *to;
        }
        let Some(checked) = self.body.types.get(&callee).map(|typed| typed.ty) else {
            return Ok(Some(Err("a method without a type")));
        };
        let rest = super::expr::rest_of(shared, checked);
        if spread && rest.is_none() {
            return Ok(Some(Err("a call with `...`")));
        }
        let declared = match &*types.get(checked) {
            CheckedType::Func { params, .. } => params.len(),
            _ => return Ok(Some(Err("a function type"))),
        };
        let plain = self.memo.signature(shared, self.b.types(), None, checked);
        let plain = match plain.map(|plain| self.kind(plain)) {
            Ok(Type::Func(plain)) => plain,
            Ok(_) => return Ok(Some(Err("a function type"))),
            Err(what) => return Ok(Some(Err(what))),
        };
        let mut mode = PlaceMode {
            held: args.iter().any(|&arg| self.has_effects(arg)),
            ..PlaceMode::default()
        };
        let base = self.place(x, &mut mode)?;
        let value = self.through(base, &mut mode)?;
        let (entry, receiver) = match self.table_entry(value, of, sel.name, checked)? {
            Ok(found) => found,
            Err(what) => return Ok(Some(Err(what))),
        };
        let _ = span;
        let mut params = vec![self.bytes_ty()];
        params.extend(plain.params.iter());
        let called = Called {
            code: Code::Known(entry),
            views: plain.params.len() - declared,
            lent: std::rc::Rc::from([]),
            worked: std::rc::Rc::from([]),
            params: params.into(),
            ret: plain.ret,
            rest,
        };
        Ok(Some(Ok((called, vec![receiver]))))
    }

    /// What a call of the method `name` runs for the value at the
    /// place, which the code names as one of the type `of`: the code
    /// that the table of the part whose type declares the method first
    /// has for it, and the address of that part, which the code takes
    /// as its receiver. `checked` is the type of the method.
    pub(super) fn table_entry(
        &mut self,
        value: Place,
        of: CheckedTypeId,
        name: Spur,
        checked: CheckedTypeId,
    ) -> Built<Result<(Operand, Operand), &'static str>> {
        let shared = self.shared;
        let Some((way, slot)) = shared.slot_of(of, name) else {
            return Ok(Err("a method that no table has"));
        };
        let code = match slot_sig(shared, self.memo, self.b.types(), checked) {
            Ok(code) => code,
            Err(what) => return Ok(Err(what)),
        };
        let mut part = value;
        for &step in &way {
            part = self.part(&part, step)?;
        }
        let bytes = self.bytes_ty();
        let at = self
            .b
            .assign_temp(Rvalue::AddrOf(part.clone(), Mutability::Mut))?;
        let at = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(Place::local(at)), bytes);
        let receiver = Operand::Copy(Place::local(self.b.assign_temp(at)?));
        // The table is read as one that ends with this method: where
        // the part is, the record of the type, then the methods.
        let word = Operand::Copy(self.table_word(&part)?);
        let table = Type::Array(bytes, slot as u64 + 3);
        let table = self.b.types().intern(table);
        let to = self.b.types().ptr(table);
        let table = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, word, to))?;
        let table = self.b.deref(&Place::local(table))?;
        let entry = Operand::Copy(self.b.const_index(&table, slot as u64 + 2)?);
        let entry = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, entry, code))?;
        Ok(Ok((Operand::Copy(Place::local(entry)), receiver)))
    }

    /// The way from the struct `child` to the part of its ancestor
    /// `parent` in it, by the places of the parts; `None` if it has no
    /// such part.
    fn way_to(&mut self, child: TypeId, parent: TypeId) -> Option<Vec<u32>> {
        if child == parent {
            return Some(Vec::new());
        }
        let lead = *self.memo.leads.get(&child)?;
        let Type::Struct(id) = self.kind(child) else {
            return None;
        };
        let parts: Vec<TypeId> = self.b.types().struct_fields(id)?.to_vec();
        let table = usize::from(self.memo.tabled.contains(&child));
        for (at, &part) in parts.iter().enumerate().take(lead as usize).skip(table) {
            self.memo.steps += 1;
            if let Some(mut way) = self.way_to(part, parent) {
                way.insert(0, at as u32);
                return Some(way);
            }
        }
        None
    }

    /// A pointer to a value where one to a part of a parent of it is
    /// wanted (EXP-13): the address of that part. `None` if the types
    /// are not those of such pointers.
    pub(super) fn upcast(
        &mut self,
        pointer: Operand,
        to: TypeId,
    ) -> Built<Option<Result<Rvalue, &'static str>>> {
        let from = self.b.operand_ty(&pointer);
        let (Type::Ptr(child), Type::Ptr(parent)) = (self.kind(from), self.kind(to)) else {
            return Ok(None);
        };
        let Some(way) = self.way_to(child, parent).filter(|way| !way.is_empty()) else {
            return Ok(None);
        };
        let held = self.slot_of(Rvalue::Use(pointer))?;
        let zero = Rvalue::Use(self.zero(to));
        let made = self.slot_of(zero)?;
        // An option of a pointer that is unset has no part.
        let none = self.zero(from);
        let set = Rvalue::Binary(BinOp::Ne, Operand::Copy(held.clone()), none);
        let set = Operand::Copy(Place::local(self.b.assign_temp(set)?));
        let [some, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(set, vec![(0, done)], some)?;
        self.b.switch_to(some);
        let value = self.b.deref(&held)?;
        let mut part = value;
        for &step in &way {
            part = self.b.field(&part, step)?;
        }
        self.b
            .assign(made.clone(), Rvalue::AddrOf(part, Mutability::Mut))?;
        self.b.goto(done)?;
        self.b.switch_to(done);
        Ok(Some(Ok(Rvalue::Use(Operand::Copy(made)))))
    }

    /// A `Unique`, a `Shared` or a `Weak` of a child where the same
    /// handle of a parent is wanted (EXP-13): the handle as its new
    /// owner's own, with the address of the part of the parent. `None`
    /// if the types are not those of such handles.
    pub(super) fn handle_upcast(
        &mut self,
        expr: ExprId,
        ty: TypeId,
        value: &Rvalue,
    ) -> Built<Option<Result<Rvalue, &'static str>>> {
        let Ok(own) = self.b.rvalue_ty(value) else {
            return Ok(None);
        };
        let kind_of = |memo: &Memo<'_>, ty: TypeId| {
            let all = [&memo.uniques, &memo.shareds, &memo.weaks];
            let at = all.iter().position(|handles| handles.contains_key(&ty))?;
            Some((at, all[at][&ty]))
        };
        let (Some((kind, child)), Some((wanted, parent))) =
            (kind_of(self.memo, own), kind_of(self.memo, ty))
        else {
            return Ok(None);
        };
        let Some(way) = self.way_to(child, parent).filter(|way| !way.is_empty()) else {
            return Ok(None);
        };
        if kind != wanted {
            return Ok(None);
        }
        let types = self.shared.types;
        let span = self.tree.exprs[expr].span;
        let held = self.checked_as(expr, own).and_then(|handle| {
            match &*types.get(types.underlying(handle)) {
                CheckedType::Unique(to) | CheckedType::Shared(to) | CheckedType::Weak(to) => {
                    Some(*to)
                }
                _ => None,
            }
        });
        let Some(held) = held else {
            return Ok(Some(Err("a handle of a value whose type is not known")));
        };
        // The type of the part, by the same way through the parents.
        let (mut of, mut at) = (held, child);
        for &step in &way {
            let before = usize::from(self.memo.tabled.contains(&at));
            let parents = types.parents(of);
            let Type::Struct(id) = self.kind(at) else {
                return Ok(Some(Err("a part of what is no struct")));
            };
            let part = self
                .b
                .types()
                .struct_fields(id)
                .and_then(|all| all.get(step as usize));
            let parent = (step as usize)
                .checked_sub(before)
                .and_then(|at| parents.get(at));
            let (Some(&part), Some(&parent)) = (part, parent) else {
                return Ok(Some(Err("a part of a parent that the type does not have")));
            };
            (of, at) = (parent, part);
        }
        let make = [CheckedType::Unique, CheckedType::Shared, CheckedType::Weak][kind];
        let Ok(handle) = types.handle(make, of) else {
            return Ok(Some(Err("a handle of this type")));
        };
        // The handle is the new owner's own: a copy if the checker says
        // so, the value itself otherwise.
        let value = self.given(expr, own, value.clone())?;
        let value = self.taken(expr, own, value)?;
        let from = self.operand_of(value)?;
        let from = self.stable(from)?;
        let zero = Rvalue::Use(self.zero(ty));
        let made = self.slot_of(zero)?;
        // An option of a handle that is unset has no part.
        let none = self.zero(own);
        let set = Rvalue::Binary(BinOp::Ne, from.clone(), none);
        let set = Operand::Copy(Place::local(self.b.assign_temp(set)?));
        let [some, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(set, vec![(0, done)], some)?;
        self.b.switch_to(some);
        let to = self.b.types().ptr(child);
        let whole = self.slot_of(Rvalue::Cast(CastKind::PtrToPtr, from, to))?;
        let whole = self.b.deref(&whole)?;
        let mut part = whole;
        for &step in &way {
            part = self.b.field(&part, step)?;
        }
        let part = self.b.assign_temp(Rvalue::AddrOf(part, Mutability::Mut))?;
        let part = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(Place::local(part)), ty);
        self.b.assign(made.clone(), part)?;
        self.b.goto(done)?;
        self.b.switch_to(done);
        if let Some(ends) = self.ends_of(Some(handle), span)? {
            self.own_temp(made.clone(), ends);
        }
        Ok(Some(Ok(Rvalue::Use(Operand::Copy(made)))))
    }

    /// The body of what a table has for the method `name` of the type
    /// `ty`: the receiver that it is given is `by` bytes before that of
    /// the method. What the method only reads of what the call gave
    /// away ends here.
    pub fn adjusted_body(
        &mut self,
        ty: CheckedTypeId,
        name: Spur,
        by: i64,
    ) -> Built<Result<(), &'static str>> {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let Some(checked) = shared.method(ty, name).map(|method| method.sig) else {
            return Ok(Err("a method that its type does not have"));
        };
        let called = match self.callee(CalleeKey::Method(ty, name))? {
            Ok(called) => called,
            Err(what) => return Ok(Err(what)),
        };
        let Some(&wanted) = called.params.first() else {
            return Ok(Err("a method without a receiver"));
        };
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let receiver = match by {
            0 => Rvalue::Cast(CastKind::PtrToPtr, given, wanted),
            by => {
                let word = self.b.types().int(64, false);
                let at = self
                    .b
                    .assign_temp(Rvalue::Cast(CastKind::PtrToInt, given, word))?;
                let by = self.b.const_int(word, by as u64)?;
                let at = Rvalue::Binary(BinOp::Add, Operand::Copy(Place::local(at)), by);
                let at = Operand::Copy(Place::local(self.b.assign_temp(at)?));
                Rvalue::Cast(CastKind::IntToPtr, at, wanted)
            }
        };
        let mut args = vec![Operand::Copy(Place::local(self.b.assign_temp(receiver)?))];
        for at in 1..called.params.len() as u32 {
            args.push(Operand::Copy(Place::local(self.b.param(at))));
        }
        let callee = Operand::Const(Constant {
            kind: ConstantKind::Func(called.func),
            ty: called.sig,
        });
        let result = Place::local(self.b.temp(called.ret));
        let next = self.b.new_block();
        let unwind = self.unwind()?;
        self.b
            .call(callee, args, result.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        if let CheckedType::Func { params, .. } = &*shared.types.get(checked) {
            for (at, &param) in params.iter().enumerate() {
                let read = called.lent.get(at + 1) == Some(&true);
                let Some(&lowered) = called.params.get(at + 1).filter(|_| read) else {
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
        if called.ret != TypeInterner::UNIT {
            let whole = Place::local(self.b.result());
            self.b.assign(whole, Rvalue::Use(Operand::Copy(result)))?;
        }
        self.b.ret()?;
        Ok(Ok(()))
    }
}
