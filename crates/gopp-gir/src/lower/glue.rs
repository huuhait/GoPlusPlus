//! The functions that the compiler makes for a type (gir.md §7). The
//! clone glue of a type makes the value at an address a copy of its
//! own: a slice gets a buffer of its own, a map a table of its own, and
//! what holds such values has each of them copied. The equality and the
//! hash glue are in `equal.rs`. A function asks for the glue of a type
//! by its symbol; the glue of a package is made once, after its
//! functions are lowered, so that the module does not depend on which
//! function asked first.

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::types::{CopyRule, Type as CheckedType, TypeId as CheckedTypeId};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::ExprId;

use super::func::{Built, FuncCx, deeper};
use super::{Callee, CalleeKey};
use crate::{
    CallConv, CastKind, Constant, ConstantKind, FuncId, FuncType, Operand, Place, Rvalue, Type,
    TypeId, TypeInterner,
};

/// How the value of a type is made a copy of its own.
enum Cloned {
    /// A buffer of its own for elements of the type; then each element
    /// by its own glue, if its copy is more than its bytes.
    Slice(TypeId, Option<CheckedTypeId>),
    /// A table of its own.
    Map,
    /// Each of these fields by its own glue.
    Fields(Vec<(u32, CheckedTypeId)>),
    /// Each of so many elements of the type by its glue.
    Elements(u64, TypeId, CheckedTypeId),
    /// What an option holds, if it is set.
    Held(CheckedTypeId),
    /// A `Unique`: a box of its own with a copy of the value, of this
    /// type, which is copied by its own glue if its copy is more than
    /// its bytes; it ends with its value, and then its box.
    Unique(TypeId, Option<CheckedTypeId>),
    /// A `Shared`: its copy is one more handle. With its last handle
    /// its value ends, by the glue of this type if the value owns
    /// anything, and then the handle that all of them had of the box.
    Shared(Option<CheckedTypeId>),
    /// A `Weak`: one more handle of the box, or one less.
    Weak,
    /// A string: its bytes are counted once more, or once less.
    Text,
    /// A struct of a type with a `drop` method. Its copy is what its
    /// method `Clone` gives; it ends by a call of `drop`, and then
    /// these fields by their own glue.
    Method(Vec<(u32, CheckedTypeId)>),
}

/// How a place ends its value.
#[derive(Clone, Copy)]
pub(super) enum Ends {
    /// By the drop glue of its type.
    Glue(FuncId),
    /// As a string: its bytes are counted once less, which is written
    /// where the string ends and calls nothing for a literal.
    Text,
    /// The place has the cell of a captured variable, which one less
    /// shares: the last ends the variable by the drop glue, if it has
    /// one, and frees the cell.
    Cell(Option<FuncId>),
    /// The place has the first of the variables of blocks that `defer`
    /// statements borrowed (PAN-3): each is let go, the one declared
    /// last first.
    Extended,
}

/// A function that the compiler makes for a type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum Glue {
    /// Makes the value at an address a copy of its own.
    Clone,
    /// Says whether the values at two addresses are equal.
    Eq,
    /// Gives the value at an address its hash, from a seed.
    Hash,
    /// Ends the value at an address: what it holds is freed.
    Drop,
}

/// The type of the glue of a kind. Glue takes values by their
/// addresses and is called as a function of C, also by the runtime.
pub(super) fn sig(types: &mut TypeInterner, glue: Glue) -> TypeId {
    let byte = types.int(8, false);
    let bytes = types.ptr(byte);
    let word = types.int(64, false);
    let (params, ret) = match glue {
        Glue::Clone | Glue::Drop => (vec![bytes], TypeInterner::UNIT),
        Glue::Eq => (vec![bytes, bytes], TypeInterner::BOOL),
        Glue::Hash => (vec![bytes, word], word),
    };
    types.intern(Type::Func(FuncType {
        params: params.into(),
        ret,
        has_env: false,
        cc: CallConv::C,
    }))
}

impl FuncCx<'_, '_> {
    pub fn glue_sig(&mut self, glue: Glue) -> TypeId {
        sig(self.b.types(), glue)
    }

    /// The function of the glue as a value: what the runtime is given
    /// to call.
    pub fn glue_value(&mut self, kind: Glue, glue: FuncId) -> Operand {
        let ty = self.glue_sig(kind);
        let kind = ConstantKind::Func(glue);
        Operand::Const(Constant { kind, ty })
    }

    /// Whether the type is declared with a `drop` method.
    fn drops(&self, ty: CheckedTypeId) -> bool {
        let types = self.shared.types;
        matches!(&*types.get(ty), CheckedType::Named { id, .. } if types.drops(*id))
    }

    /// How a value of the type is copied (`Glue::Clone`), or what of it
    /// ends (`Glue::Drop`).
    fn cloned(&mut self, kind: Glue, ty: CheckedTypeId) -> Result<Cloned, &'static str> {
        let shared = self.shared;
        let types = shared.types;
        let rule = types.copy_rule(ty);
        match (kind, rule) {
            (_, Some(CopyRule::SameBytes)) => return Ok(Cloned::Text),
            (
                Glue::Clone,
                Some(
                    CopyRule::Elements
                    | CopyRule::Parts
                    | CopyRule::Method
                    | CopyRule::Pointee
                    | CopyRule::Handle,
                ),
            ) => {}
            (Glue::Clone, None) => return Err("a copy of a value that cannot be copied"),
            (Glue::Clone, _) => return Err("a value of a type that is not copied by its parts"),
            // A value that cannot be copied ends as its parts do.
            _ => {}
        }
        let lower = |this: &mut Self, part| this.memo.lower_type(shared, this.b.types(), part);
        let under = types.get(types.underlying(ty));
        let how = match &*under {
            CheckedType::Slice(elem) => {
                let of = lower(self, *elem)?;
                Cloned::Slice(of, (!self.flat(of)).then_some(*elem))
            }
            CheckedType::Map { .. } => Cloned::Map,
            CheckedType::Struct(fields) => {
                // The parts of the parents come before the fields.
                // The address of a table, which owns nothing, comes
                // before them.
                let parents = types.parents(ty);
                let table = usize::from(shared.own_table(ty));
                let lead = (table + parents.len()) as u32;
                let parents = parents.iter().copied().enumerate();
                let parents = parents.map(|(at, parent)| (table + at, parent));
                let own = fields.iter().enumerate();
                let own = own.map(|(at, field)| (lead as usize + at, field.ty));
                let mut parts = Vec::new();
                for (at, part) in parents.chain(own) {
                    let of = lower(self, part)?;
                    if !self.flat(of) {
                        parts.push((at as u32, part));
                    }
                }
                // A value of a type with parents ends child first: its
                // fields from the last to the first, then its parents
                // from the last to the first (OWN-R2).
                if kind == Glue::Drop && lead > 0 {
                    parts.reverse();
                }
                Cloned::Fields(parts)
            }
            CheckedType::Array { len, elem } => Cloned::Elements(*len, lower(self, *elem)?, *elem),
            CheckedType::Unique(of) => {
                let value = lower(self, *of)?;
                Cloned::Unique(value, (!self.flat(value)).then_some(*of))
            }
            CheckedType::Shared(of) => {
                let value = lower(self, *of)?;
                let ends = kind == Glue::Drop && !self.flat(value);
                Cloned::Shared(ends.then_some(*of))
            }
            CheckedType::Weak(_) => Cloned::Weak,
            // An option of what is nil when it is all zero is that
            // value itself.
            CheckedType::Option(held) if lower(self, *held)? == lower(self, ty)? => {
                return self.cloned(kind, *held);
            }
            CheckedType::Option(held) => Cloned::Held(*held),
            _ if kind == Glue::Drop => return Err("the end of a value of this type"),
            _ => return Err("a copy of a value of this type"),
        };
        match (self.drops(ty), how) {
            (false, how) => Ok(how),
            (true, Cloned::Fields(parts)) => Ok(Cloned::Method(parts)),
            (true, _) => Err("a `drop` method of a type that is no struct"),
        }
    }

    /// The method of the type that its glue calls: `drop`, or `Clone`.
    /// `Clone` reads its receiver where it is (PAR-5): the glue lends
    /// it the value, so the method must do no more to it than read.
    fn glue_method(
        &mut self,
        kind: Glue,
        ty: CheckedTypeId,
    ) -> Built<Result<Callee, &'static str>> {
        let name = match kind {
            Glue::Drop => self.shared.drop,
            _ => self.shared.clone,
        };
        let called = match self.callee(CalleeKey::Method(ty, name))? {
            Ok(called) => called,
            Err(_) if kind == Glue::Drop => return Ok(Err("a `drop` method of another package")),
            Err(_) => return Ok(Err("a `Clone` method of another package")),
        };
        let lent = called.lent.first() == Some(&true);
        Ok(match kind == Glue::Clone && !lent {
            true => Err("a `Clone` method that does more than read its receiver"),
            false => Ok(called),
        })
    }

    /// The parts of the type that have glue of the kind of their own,
    /// which the glue of the type calls.
    fn cloned_parts(
        &mut self,
        kind: Glue,
        ty: CheckedTypeId,
    ) -> Built<Result<Vec<CheckedTypeId>, &'static str>> {
        let how = match self.cloned(kind, ty) {
            Ok(how) => how,
            Err(what) => return Ok(Err(what)),
        };
        Ok(Ok(match how {
            Cloned::Text | Cloned::Weak => Vec::new(),
            Cloned::Slice(_, each) | Cloned::Unique(_, each) | Cloned::Shared(each) => {
                each.into_iter().collect()
            }
            Cloned::Map => match self.map_of_type(ty) {
                Ok(_) => Vec::new(),
                Err(what) => return Ok(Err(what)),
            },
            Cloned::Fields(fields) => fields.iter().map(|&(_, part)| part).collect(),
            Cloned::Elements(_, _, part) | Cloned::Held(part) => vec![part],
            Cloned::Method(fields) => {
                if let Err(what) = self.glue_method(kind, ty)? {
                    return Ok(Err(what));
                }
                // The method makes the whole copy.
                match kind {
                    Glue::Drop => fields.iter().map(|&(_, part)| part).collect(),
                    _ => Vec::new(),
                }
            }
        }))
    }

    /// The glue of a kind for the type, declared in the module of the
    /// run and asked of the package; or why the type has none yet.
    pub fn glue(&mut self, kind: Glue, ty: CheckedTypeId) -> Built<Result<FuncId, &'static str>> {
        if let Some(&known) = self.memo.glue_of.get(&(kind, ty)) {
            return Ok(known);
        }
        let entity = match kind {
            Glue::Clone => Entity::CloneGlue(ty),
            Glue::Eq => Entity::EqGlue(ty),
            Glue::Hash => Entity::HashGlue(ty),
            Glue::Drop => Entity::DropGlue(ty),
        };
        // The runtime has the glue of every interface type, and counts
        // the environments of function values.
        let lowered = self.memo.lower_type(self.shared, self.b.types(), ty);
        if lowered.is_ok_and(|lowered| self.is_iface(lowered)) {
            let glue = Ok(self.iface_glue(kind)?);
            self.memo.glue_of.insert((kind, ty), glue);
            return Ok(glue);
        }
        if lowered.is_ok_and(|lowered| self.memo.func_types.contains(&lowered)) {
            let name = match kind {
                Glue::Clone => Ok("_gopp_func_clone"),
                Glue::Drop => Ok("_gopp_func_drop"),
                Glue::Eq | Glue::Hash => Err("a comparison of function values"),
            };
            let glue = match name {
                Ok(name) => {
                    let sig = self.glue_sig(kind);
                    Ok(self.function_named(name, sig)?)
                }
                Err(what) => Err(what),
            };
            self.memo.glue_of.insert((kind, ty), glue);
            return Ok(glue);
        }
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            let none = Err("a function for a type without a symbol");
            self.memo.glue_of.insert((kind, ty), none);
            return Ok(none);
        };
        let sig = self.glue_sig(kind);
        let glue = self.function_named(&symbol, sig)?;
        // Known before its parts are: a type may hold itself in a slice.
        self.memo.glue_of.insert((kind, ty), Ok(glue));
        // The parts that have glue of their own, which this one calls.
        let parts = match kind {
            Glue::Clone | Glue::Drop => self.cloned_parts(kind, ty)?,
            Glue::Eq | Glue::Hash => self.compared_parts(ty),
        };
        let mut made = parts.as_ref().map(|_| glue).map_err(|&what| what);
        for part in parts.unwrap_or_default() {
            if let Err(what) = deeper(|| self.glue(kind, part))? {
                made = Err(what);
            }
        }
        match made {
            Ok(_) => self.memo.glues.push((symbol, super::Made::Glue(kind), ty)),
            Err(_) => drop(self.memo.glue_of.insert((kind, ty), made)),
        }
        Ok(made)
    }

    /// The type of the checker that the expression has where its value
    /// is of the type `ty`: its own, or what it points to if it is read
    /// through a pointer, a `Unique` or a `Shared`.
    pub fn checked_as(&mut self, expr: ExprId, ty: TypeId) -> Option<CheckedTypeId> {
        let types = self.shared.types;
        // A part of the value that is the receiver of a method.
        if let Some(&part) = self.parts.get(&expr) {
            let lowered = self.memo.lower_type(self.shared, self.b.types(), part);
            if lowered == Ok(ty) {
                return Some(part);
            }
        }
        // A value that is put into an interface is of the interface
        // type there.
        if let (Some(&(_, to)), true) = (self.body.iface_conversions.get(&expr), self.is_iface(ty))
        {
            return Some(to);
        }
        let mut checked = types.default_type(self.body.types.get(&expr)?.ty);
        loop {
            self.memo.steps += 1;
            let lowered = self.memo.lower_type(self.shared, self.b.types(), checked);
            if lowered == Ok(ty) {
                return Some(checked);
            }
            checked = match &*types.get(types.underlying(checked)) {
                CheckedType::Pointer(to) | CheckedType::Unique(to) | CheckedType::Shared(to) => *to,
                _ => return None,
            };
        }
    }

    /// The type of the elements of a slice or an array type.
    pub fn element_of(&self, ty: CheckedTypeId) -> Option<CheckedTypeId> {
        let types = self.shared.types;
        match &*types.get(types.underlying(ty)) {
            CheckedType::Slice(elem) | CheckedType::Array { elem, .. } => Some(*elem),
            _ => None,
        }
    }

    /// The value as a copy that its new owner has for itself, made by
    /// the glue of its type `checked`.
    pub fn cloned_value(
        &mut self,
        span: SourceSpan,
        checked: Option<CheckedTypeId>,
        value: Rvalue,
    ) -> Built<Rvalue> {
        let copy = self.slot_of(value)?;
        let glue = match checked {
            Some(checked) => self.glue(Glue::Clone, checked)?,
            None => Err("a copy of a value whose type is not known"),
        };
        match glue {
            Ok(glue) => {
                let at = self.record(copy.clone())?;
                self.call_glue(glue, at)?;
            }
            Err(what) => self.unsupported(span, what),
        }
        Ok(Rvalue::Use(Operand::Copy(copy)))
    }

    /// Makes each of `count` elements from `first` on a copy of its
    /// own, by the glue of the type `checked` of an element.
    pub fn clone_each(
        &mut self,
        span: SourceSpan,
        first: Place,
        count: Operand,
        of: TypeId,
        checked: Option<CheckedTypeId>,
    ) -> Built {
        let glue = match checked {
            Some(checked) => self.glue(Glue::Clone, checked)?,
            None => Err("a copy of a value whose type is not known"),
        };
        match glue {
            Ok(glue) => self.each(first, count, of, glue),
            Err(what) => {
                self.unsupported(span, what);
                Ok(())
            }
        }
    }

    /// The bytes of the string in the place are counted once more, or
    /// once less; the last string that shares them frees them. A string
    /// without a buffer of its own, which a literal is, calls nothing.
    pub fn counted(&mut self, text: Place, more: bool) -> Built {
        let bytes = self.bytes_ty();
        let string = self.b.place_ty(&text);
        let backing = Operand::Copy(self.b.field(&text, 2)?);
        let own = Rvalue::Binary(crate::BinOp::Ne, backing, self.zero(bytes));
        let own = Operand::Copy(Place::local(self.b.assign_temp(own)?));
        let [counts, done] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(own, vec![(0, done)], counts)?;
        self.b.switch_to(counts);
        let name = match more {
            true => "_gopp_string_retain",
            false => "_gopp_string_release",
        };
        self.runtime_call(name, &[string], vec![Operand::Copy(text)])?;
        self.b.goto(done)?;
        self.b.switch_to(done);
        Ok(())
    }

    /// The value in the place ends.
    pub fn end(&mut self, place: Place, ends: Ends) -> Built {
        self.ending += 1;
        let ended = self.end_here(place, ends);
        self.ending -= 1;
        ended
    }

    fn end_here(&mut self, place: Place, ends: Ends) -> Built {
        match ends {
            Ends::Text => self.counted(place, false),
            Ends::Glue(glue) => {
                let at = self.record(place)?;
                self.call_glue(glue, at)
            }
            Ends::Cell(drop) => {
                let bytes = self.bytes_ty();
                let cell = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(place), bytes);
                let cell = Operand::Copy(Place::local(self.b.assign_temp(cell)?));
                let sig = self.glue_sig(Glue::Drop);
                let drop = match drop {
                    Some(drop) => self.glue_value(Glue::Drop, drop),
                    None => self.zero(sig),
                };
                self.runtime_call("_gopp_cell_release", &[bytes, sig], vec![cell, drop])
            }
            Ends::Extended => {
                let bytes = self.bytes_ty();
                let list = self.record(place)?;
                self.runtime_call("_gopp_extended_end", &[bytes], vec![list])
            }
        }
    }

    /// A call of clone or drop glue with the address of a value.
    pub fn call_glue(&mut self, glue: FuncId, at: Operand) -> Built {
        let callee = self.glue_value(Glue::Clone, glue);
        let nothing = Place::local(self.b.temp(TypeInterner::UNIT));
        let unwind = self.unwind()?;
        let next = self.b.new_block();
        self.b.call(callee, vec![at], nothing, Some(next), unwind)?;
        self.b.switch_to(next);
        Ok(())
    }

    /// The runtime calls the glue for each of `count` elements of the
    /// type `of` from `first` on.
    pub fn each(&mut self, first: Place, count: Operand, of: TypeId, glue: FuncId) -> Built {
        let word = self.b.types().int(64, false);
        let layout = self.b.types().layout(of, self.shared.layout);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let sig = self.glue_sig(Glue::Clone);
        let glue = self.glue_value(Glue::Clone, glue);
        let params = [self.bytes_ty(), word, word, sig];
        let args = vec![self.record(first)?, count, size, glue];
        self.runtime_call("_gopp_each", &params, args)
    }

    /// Each of the fields of the value is copied or ended by the glue
    /// of its type.
    fn fields_glue(
        &mut self,
        kind: Glue,
        value: &Place,
        fields: Vec<(u32, CheckedTypeId)>,
    ) -> Built<Result<(), &'static str>> {
        for (at, part) in fields {
            let glue = match self.glue(kind, part)? {
                Ok(glue) => glue,
                Err(what) => return Ok(Err(what)),
            };
            let field = self.b.field(value, at)?;
            let at = self.record(field)?;
            self.call_glue(glue, at)?;
        }
        Ok(Ok(()))
    }

    /// The body of the clone or of the drop glue of the type. Clone:
    /// the value at the address that the function is given becomes a
    /// copy of its own. Drop: what the value holds is freed, its parts
    /// first.
    pub fn walk_body(&mut self, kind: Glue, ty: CheckedTypeId) -> Built<Result<(), &'static str>> {
        let ends = kind == Glue::Drop;
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let lowered = match self.memo.lower_type(shared, self.b.types(), ty) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let how = match self.cloned(kind, ty) {
            Ok(how) => how,
            Err(what) => return Ok(Err(what)),
        };
        // Whether the value is a handle of a type with a table.
        let types = shared.types;
        let mut of = types.underlying(ty);
        while let CheckedType::Option(held) = &*types.get(of) {
            of = types.underlying(*held);
        }
        let tabled = match &*types.get(of) {
            CheckedType::Unique(to) | CheckedType::Shared(to) | CheckedType::Weak(to) => {
                types.has_table(*to)
            }
            _ => false,
        };
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let to = self.b.types().ptr(lowered);
        let typed = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, given.clone(), to))?;
        let value = self.b.deref(&Place::local(typed))?;
        let word = self.b.types().int(64, false);
        // The glue of the parts that this one calls.
        let part = match &how {
            Cloned::Slice(_, each) | Cloned::Unique(_, each) | Cloned::Shared(each) => *each,
            Cloned::Map | Cloned::Fields(_) | Cloned::Text | Cloned::Method(_) | Cloned::Weak => {
                None
            }
            Cloned::Elements(_, _, part) | Cloned::Held(part) => Some(*part),
        };
        let of_part = match part.map(|part| self.glue(kind, part)).transpose()? {
            Some(Err(what)) => return Ok(Err(what)),
            Some(Ok(glue)) => Some(glue),
            None => None,
        };
        match how {
            Cloned::Text => self.counted(value, !ends)?,
            Cloned::Slice(of, _) => {
                let layout = self.b.types().layout(of, shared.layout);
                let size = self
                    .b
                    .const_int(word, layout.map_or(0, |layout| layout.size))?;
                let bytes = self.bytes_ty();
                // A copy has its buffer before its elements are copied;
                // a slice that ends loses its elements first.
                if !ends {
                    let args = vec![given.clone(), size];
                    self.runtime_call("_gopp_slice_copy", &[bytes, word], args)?;
                }
                if let Some(glue) = of_part {
                    let zero = Rvalue::Use(self.zero(word));
                    let none = self.b.assign_temp(zero)?;
                    let first = self.b.index(&value, none)?;
                    let count = self.b.assign_temp(Rvalue::Len(value))?;
                    let count = Operand::Copy(Place::local(count));
                    self.each(first, count, of, glue)?;
                }
                if ends {
                    self.runtime_call("_gopp_slice_free", &[bytes], vec![given])?;
                }
            }
            Cloned::Map => {
                let of = match self.map_of_type(ty) {
                    Ok(of) => of,
                    Err(what) => return Ok(Err(what)),
                };
                let map = Operand::Copy(value.clone());
                match ends {
                    true => self.map_dropped(lowered, &of, map)?,
                    false => {
                        let copy = self.map_copied(lowered, &of, map)?;
                        self.b.assign(value, Rvalue::Use(copy))?;
                    }
                }
            }
            Cloned::Fields(fields) => {
                if let Err(what) = self.fields_glue(kind, &value, fields)? {
                    return Ok(Err(what));
                }
            }
            Cloned::Method(fields) => {
                let called = match self.glue_method(kind, ty)? {
                    Ok(called) => called,
                    Err(what) => return Ok(Err(what)),
                };
                let callee = Operand::Const(Constant {
                    kind: ConstantKind::Func(called.func),
                    ty: called.sig,
                });
                let result = Place::local(self.b.temp(called.ret));
                let next = self.b.new_block();
                // `drop` takes the address of the value, and `Clone`
                // reads the value and gives the copy, which takes its
                // place.
                let receiver = match ends {
                    true => Operand::Copy(Place::local(typed)),
                    false => Operand::Copy(value.clone()),
                };
                let unwind = self.unwind()?;
                self.b
                    .call(callee, vec![receiver], result.clone(), Some(next), unwind)?;
                self.b.switch_to(next);
                match ends {
                    true => {
                        if let Err(what) = self.fields_glue(kind, &value, fields)? {
                            return Ok(Err(what));
                        }
                    }
                    false => self.b.assign(value, Rvalue::Use(Operand::Copy(result)))?,
                }
            }
            Cloned::Elements(count, of, _) => {
                if let (Some(glue), true) = (of_part, count > 0) {
                    let first = self.b.const_index(&value, 0)?;
                    let count = self.b.const_int(word, count)?;
                    self.each(first, count, of, glue)?;
                }
            }
            Cloned::Unique(of, _) => {
                let [held, done] = [(); 2].map(|()| self.b.new_block());
                let bytes = self.bytes_ty();
                let handle = Operand::Copy(value.clone());
                let boxed = Rvalue::Cast(CastKind::PtrToPtr, handle.clone(), bytes);
                let boxed = Operand::Copy(Place::local(self.b.assign_temp(boxed)?));
                // A handle that gave its value up is nil.
                let null = self.zero(self.b.operand_ty(&handle));
                let set = self
                    .b
                    .assign_temp(Rvalue::Binary(crate::BinOp::Ne, handle, null))?;
                self.b
                    .switch(Operand::Copy(Place::local(set)), vec![(0, done)], held)?;
                self.b.switch_to(held);
                // A handle of a type with a table may hold a child: the
                // runtime ends or copies the whole value by its table,
                // and leaves a value without one to this code.
                if tabled && ends {
                    let bool = TypeInterner::BOOL;
                    let args = vec![boxed.clone()];
                    let plain = self.runtime("_gopp_whole_end", &[bytes], bool, args)?;
                    let here = self.b.new_block();
                    self.b.switch(plain, vec![(0, done)], here)?;
                    self.b.switch_to(here);
                }
                if tabled && !ends {
                    let args = vec![boxed.clone()];
                    let copy = self.runtime("_gopp_whole_copy", &[bytes], bytes, args)?;
                    let copy = self.stable(copy)?;
                    let none = self.zero(bytes);
                    let made = Rvalue::Binary(crate::BinOp::Ne, copy.clone(), none);
                    let made = Operand::Copy(Place::local(self.b.assign_temp(made)?));
                    let [whole, here] = [(); 2].map(|()| self.b.new_block());
                    self.b.switch(made, vec![(0, here)], whole)?;
                    self.b.switch_to(whole);
                    let copy = Rvalue::Cast(CastKind::PtrToPtr, copy, lowered);
                    self.b.assign(value.clone(), copy)?;
                    self.b.goto(done)?;
                    self.b.switch_to(here);
                }
                match ends {
                    // The value ends, then its box.
                    true => {
                        if let Some(glue) = of_part {
                            self.call_glue(glue, boxed.clone())?;
                        }
                        self.runtime_call("_gopp_box_free", &[bytes], vec![boxed])?;
                    }
                    // A box of its own with the bytes, which the glue
                    // of the value makes a copy of its own.
                    false => {
                        let layout = self.b.types().layout(of, shared.layout);
                        let size = self
                            .b
                            .const_int(word, layout.map_or(0, |layout| layout.size))?;
                        let made = self.runtime(
                            "_gopp_iface_box",
                            &[bytes, word],
                            bytes,
                            vec![boxed, size],
                        )?;
                        let copy = Rvalue::Cast(CastKind::PtrToPtr, made.clone(), lowered);
                        self.b.assign(value, copy)?;
                        if let Some(glue) = of_part {
                            self.call_glue(glue, made)?;
                        }
                    }
                }
                self.b.goto(done)?;
                self.b.switch_to(done);
            }
            Cloned::Shared(_) | Cloned::Weak => {
                let [held, done] = [(); 2].map(|()| self.b.new_block());
                let bytes = self.bytes_ty();
                let handle = Operand::Copy(value.clone());
                let at = Rvalue::Cast(CastKind::PtrToPtr, handle.clone(), bytes);
                let at = Operand::Copy(Place::local(self.b.assign_temp(at)?));
                // A handle that gave its value up is nil.
                let null = self.zero(self.b.operand_ty(&handle));
                let set = self
                    .b
                    .assign_temp(Rvalue::Binary(crate::BinOp::Ne, handle, null))?;
                self.b
                    .switch(Operand::Copy(Place::local(set)), vec![(0, done)], held)?;
                self.b.switch_to(held);
                // The counts of a handle of a type with a table are those
                // of the whole value that it may be a part of.
                let counts = match tabled {
                    true => [
                        "_gopp_part_retain",
                        "_gopp_part_weak_retain",
                        "_gopp_part_weak_release",
                        "_gopp_part_release",
                    ],
                    false => [
                        "_gopp_shared_retain",
                        "_gopp_weak_retain",
                        "_gopp_weak_release",
                        "_gopp_shared_release",
                    ],
                };
                match (matches!(how, Cloned::Weak), ends) {
                    (false, false) => self.runtime_call(counts[0], &[bytes], vec![at])?,
                    (true, false) => self.runtime_call(counts[1], &[bytes], vec![at])?,
                    (true, true) => self.runtime_call(counts[2], &[bytes], vec![at])?,
                    (false, true) => {
                        let bool = TypeInterner::BOOL;
                        let last = self.runtime(counts[3], &[bytes], bool, vec![at.clone()])?;
                        let ended = self.b.new_block();
                        self.b.switch(last, vec![(0, done)], ended)?;
                        self.b.switch_to(ended);
                        if let Some(glue) = of_part {
                            self.call_glue(glue, at.clone())?;
                        }
                        self.runtime_call("_gopp_weak_release", &[bytes], vec![at])?;
                    }
                }
                self.b.goto(done)?;
                self.b.switch_to(done);
            }
            Cloned::Held(_) => {
                let [held, done] = [(); 2].map(|()| self.b.new_block());
                let set = Operand::Copy(self.b.field(&value, 1)?);
                self.b.switch(set, vec![(0, done)], held)?;
                self.b.switch_to(held);
                if let Some(glue) = of_part {
                    let inside = self.b.field(&value, 0)?;
                    let at = self.record(inside)?;
                    self.call_glue(glue, at)?;
                }
                self.b.goto(done)?;
                self.b.switch_to(done);
            }
        }
        self.b.ret()?;
        Ok(Ok(()))
    }
}
