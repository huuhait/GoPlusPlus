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
use crate::{
    CallConv, CastKind, Constant, ConstantKind, FuncId, FuncType, Operand, Place, Rvalue, Type,
    TypeId, TypeInterner, UnwindAction,
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
    /// A string: its bytes are counted once more, or once less.
    Text,
}

/// How a place ends its value.
#[derive(Clone, Copy)]
pub(super) enum Ends {
    /// By the drop glue of its type.
    Glue(FuncId),
    /// As a string: its bytes are counted once less, which is written
    /// where the string ends and calls nothing for a literal.
    Text,
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

    fn cloned(&mut self, ty: CheckedTypeId) -> Result<Cloned, &'static str> {
        let shared = self.shared;
        let types = shared.types;
        match types.copy_rule(ty) {
            Some(CopyRule::Elements | CopyRule::Parts) => {}
            Some(CopyRule::SameBytes) => return Ok(Cloned::Text),
            Some(CopyRule::Method) => return Err("a value of a type with a `Clone` method"),
            _ => return Err("a value of a type that is not copied by its parts"),
        }
        let lower = |this: &mut Self, part| this.memo.lower_type(shared, this.b.types(), part);
        let kind = types.get(types.underlying(ty));
        Ok(match &*kind {
            CheckedType::Slice(elem) => {
                let of = lower(self, *elem)?;
                Cloned::Slice(of, (!self.flat(of)).then_some(*elem))
            }
            CheckedType::Map { .. } => Cloned::Map,
            CheckedType::Struct(fields) => {
                let mut parts = Vec::new();
                for (at, field) in fields.iter().enumerate() {
                    let of = lower(self, field.ty)?;
                    if !self.flat(of) {
                        parts.push((at as u32, field.ty));
                    }
                }
                Cloned::Fields(parts)
            }
            CheckedType::Array { len, elem } => Cloned::Elements(*len, lower(self, *elem)?, *elem),
            CheckedType::Option(held) => Cloned::Held(*held),
            _ => return Err("a copy of a value of this type"),
        })
    }

    /// The parts of the type that have clone glue of their own, which
    /// the glue of the type calls.
    fn cloned_parts(&mut self, ty: CheckedTypeId) -> Result<Vec<CheckedTypeId>, &'static str> {
        Ok(match self.cloned(ty)? {
            Cloned::Text => Vec::new(),
            Cloned::Slice(_, each) => each.into_iter().collect(),
            Cloned::Map => self.map_of_type(ty).map(|_| Vec::new())?,
            Cloned::Fields(fields) => fields.iter().map(|&(_, part)| part).collect(),
            Cloned::Elements(_, _, part) | Cloned::Held(part) => vec![part],
        })
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
            Glue::Clone | Glue::Drop => self.cloned_parts(ty),
            Glue::Eq | Glue::Hash => self.compared_parts(ty),
        };
        let mut made = parts.as_ref().map(|_| glue).map_err(|&what| what);
        for part in parts.unwrap_or_default() {
            if let Err(what) = deeper(|| self.glue(kind, part))? {
                made = Err(what);
            }
        }
        match made {
            Ok(_) => self.memo.glues.push((symbol, kind, ty)),
            Err(_) => drop(self.memo.glue_of.insert((kind, ty), made)),
        }
        Ok(made)
    }

    /// The type of the checker that the expression has where its value
    /// is of the type `ty`: its own, or what it points to if it is read
    /// through a pointer.
    pub fn checked_as(&mut self, expr: ExprId, ty: TypeId) -> Option<CheckedTypeId> {
        let types = self.shared.types;
        let mut checked = types.default_type(self.body.types.get(&expr)?.ty);
        loop {
            self.memo.steps += 1;
            let lowered = self.memo.lower_type(self.shared, self.b.types(), checked);
            if lowered == Ok(ty) {
                return Some(checked);
            }
            checked = match &*types.get(types.underlying(checked)) {
                CheckedType::Pointer(to) => *to,
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
        match ends {
            Ends::Text => self.counted(place, false),
            Ends::Glue(glue) => {
                let at = self.record(place)?;
                self.call_glue(glue, at)
            }
        }
    }

    /// A call of clone or drop glue with the address of a value.
    pub fn call_glue(&mut self, glue: FuncId, at: Operand) -> Built {
        let callee = self.glue_value(Glue::Clone, glue);
        let nothing = Place::local(self.b.temp(TypeInterner::UNIT));
        let next = self.b.new_block();
        let unwind = UnwindAction::Continue;
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
        let how = match self.cloned(ty) {
            Ok(how) => how,
            Err(what) => return Ok(Err(what)),
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
            Cloned::Slice(_, each) => *each,
            Cloned::Map | Cloned::Fields(_) | Cloned::Text => None,
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
                    let none = self.b.assign_temp(Rvalue::Use(self.zero(word)))?;
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
                for (at, part) in fields {
                    let glue = match self.glue(kind, part)? {
                        Ok(glue) => glue,
                        Err(what) => return Ok(Err(what)),
                    };
                    let field = self.b.field(&value, at)?;
                    let at = self.record(field)?;
                    self.call_glue(glue, at)?;
                }
            }
            Cloned::Elements(count, of, _) => {
                if let (Some(glue), true) = (of_part, count > 0) {
                    let first = self.b.const_index(&value, 0)?;
                    let count = self.b.const_int(word, count)?;
                    self.each(first, count, of, glue)?;
                }
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
