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
use gopp_sema::types::{Marks, Member, PARENT, Type as CheckedType, TypeId as CheckedTypeId};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{ExprId, ExprKind};
use lasso::Spur;

use super::expr::{Called, Code, PlaceMode, Target};
use super::func::{Built, FuncCx};
use super::{CalleeKey, Made, Memo, Shared};
use crate::{
    BinOp, CallConv, CastKind, Constant, ConstantKind, FuncType, GlobalId, Mutability, Operand,
    Place, Rvalue, Type, TypeId, TypeInterner,
};

impl Shared<'_> {
    /// Whether the method of the type may be replaced by a child, or
    /// replaces one: a call through a pointer runs what the table has.
    pub fn replaced(&self, holder: CheckedTypeId, name: Spur) -> bool {
        let exported = self.names.resolve(&name).starts_with(char::is_uppercase);
        let package = match &*self.types.get(holder) {
            CheckedType::Named { id, .. } => self.types.home(*id).path,
            _ => None,
        };
        let marks = self
            .types
            .method_marks(holder, name, package.filter(|_| !exported));
        marks.has(Marks::VIRTUAL) || marks.has(Marks::ABSTRACT) || marks.has(Marks::OVERRIDE)
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

    /// The table of the part at the end of `way` of a value of the type
    /// `whole`; the part is of the type `head`. It says how far the
    /// part is from the start of the value, and has for each method of
    /// `head` that may be replaced the code that runs for a value of
    /// `whole`: null for a method without a body.
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
        let mut parts = vec![Constant {
            kind: ConstantKind::Int((-start) as u64),
            ty: offset,
        }];
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

    /// Gives the value at the place, of the type `checked`, the
    /// addresses of its tables if it has none yet: a value that was
    /// made all zero.
    pub(super) fn set_tables(
        &mut self,
        value: &Place,
        checked: CheckedTypeId,
    ) -> Built<Result<(), &'static str>> {
        let shared = self.shared;
        let types = shared.types;
        // No value is of a type that leaves a method open.
        if !types.has_table(checked) || types.is_abstract(checked, shared.names) {
            return Ok(Ok(()));
        }
        let bytes = self.bytes_ty();
        let word = self.table_word(value)?;
        let none = self.zero(bytes);
        let set = self
            .b
            .assign_temp(Rvalue::Binary(BinOp::Ne, Operand::Copy(word), none))?;
        let [unset, done] = [(); 2].map(|()| self.b.new_block());
        self.b
            .switch(Operand::Copy(Place::local(set)), vec![(0, unset)], done)?;
        self.b.switch_to(unset);
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
        self.b.goto(done)?;
        self.b.switch_to(done);
        Ok(Ok(()))
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
        let Some((way, slot)) = shared.slot_of(of, sel.name) else {
            return Ok(Some(Err("a method that no table has")));
        };
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
        let code = match slot_sig(shared, self.memo, self.b.types(), checked) {
            Ok(code) => code,
            Err(what) => return Ok(Some(Err(what))),
        };
        let mut mode = PlaceMode {
            held: args.iter().any(|&arg| self.has_effects(arg)),
            ..PlaceMode::default()
        };
        let base = self.place(x, &mut mode)?;
        let value = self.through(base, &mut mode)?;
        if let Err(what) = self.set_tables(&value, of)? {
            return Ok(Some(Err(what)));
        }
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
        // The table is read as one that ends with this method: a word
        // that says where the part is, then the methods.
        let word = Operand::Copy(self.table_word(&part)?);
        let table = Type::Array(bytes, slot as u64 + 2);
        let table = self.b.types().intern(table);
        let to = self.b.types().ptr(table);
        let table = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, word, to))?;
        let table = self.b.deref(&Place::local(table))?;
        let entry = Operand::Copy(self.b.const_index(&table, slot as u64 + 1)?);
        let entry = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, entry, code))?;
        let _ = span;
        let mut params = vec![bytes];
        params.extend(plain.params.iter());
        let called = Called {
            code: Code::Known(Operand::Copy(Place::local(entry))),
            views: plain.params.len() - declared,
            lent: std::rc::Rc::from([]),
            worked: std::rc::Rc::from([]),
            params: params.into(),
            ret: plain.ret,
            rest,
        };
        Ok(Some(Ok((called, vec![receiver]))))
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
    /// are not those of such pointers. `checked` is the type of what
    /// the pointer points to.
    pub(super) fn upcast(
        &mut self,
        pointer: Operand,
        to: TypeId,
        checked: Option<CheckedTypeId>,
    ) -> Built<Option<Result<Rvalue, &'static str>>> {
        let from = self.b.operand_ty(&pointer);
        let (Type::Ptr(child), Type::Ptr(parent)) = (self.kind(from), self.kind(to)) else {
            return Ok(None);
        };
        let Some(way) = self.way_to(child, parent).filter(|way| !way.is_empty()) else {
            return Ok(None);
        };
        let Some(checked) = checked else {
            return Ok(Some(Err(
                "a pointer to a part of a value whose type is not known",
            )));
        };
        let held = self.slot_of(Rvalue::Use(pointer))?;
        let value = self.b.deref(&held)?;
        if let Err(what) = self.set_tables(&value, checked)? {
            return Ok(Some(Err(what)));
        }
        let mut part = value;
        for &step in &way {
            part = self.b.field(&part, step)?;
        }
        Ok(Some(Ok(Rvalue::AddrOf(part, Mutability::Mut))))
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
