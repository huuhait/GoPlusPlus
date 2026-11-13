//! Equality and hashes of values that are more than one scalar (TYP-3,
//! gir.md §5). Two structs are equal when their fields that have a name
//! are, two arrays when their elements are. The equality glue of a type
//! compares the values at two addresses, the hash glue gives the value
//! at an address a hash from a seed, so that equal values have one
//! hash: both are what a map asks for keys that it cannot compare by
//! their bytes.

use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{BinaryOp, ExprId};

use super::func::{Built, FuncCx};
use super::glue::Glue;
use crate::{BinOp, CastKind, Operand, Place, Rvalue, Type, TypeId, TypeInterner, UnOp};

/// How two values of a type are compared, and how one is hashed.
enum Compared {
    /// By their bytes, of which there are so many: no float, no string
    /// and no padding is among them.
    Bytes(u64),
    /// As GIR compares two of them: a number, a boolean or a pointer.
    Scalar,
    /// As strings.
    Text,
    /// Each field that has a name: its number and its type.
    Fields(Vec<(u32, CheckedTypeId)>),
    /// Each of so many elements of the type.
    Elements(u64, TypeId, CheckedTypeId),
    /// As interface values: by the runtime, which is their glue.
    Held,
}

fn slot(local: crate::LocalId) -> Operand {
    Operand::Copy(Place::local(local))
}

impl FuncCx<'_, '_> {
    fn size_of(&mut self, ty: TypeId) -> u64 {
        let layout = self.b.types().layout(ty, self.shared.layout);
        layout.map_or(0, |layout| layout.size)
    }

    fn compared(&mut self, ty: CheckedTypeId) -> Result<Compared, &'static str> {
        let shared = self.shared;
        let lowered = self.memo.lower_type(shared, self.b.types(), ty)?;
        match self.kind(lowered) {
            Type::String => return Ok(Compared::Text),
            Type::Bool | Type::Int { .. } | Type::Float { .. } | Type::Ptr(_) => {
                return Ok(Compared::Scalar);
            }
            _ => {}
        }
        if self.is_iface(lowered) {
            return Ok(Compared::Held);
        }
        if self.equal_by_bytes(ty, lowered) {
            return Ok(Compared::Bytes(self.size_of(lowered)));
        }
        let kind = shared.types.get(shared.types.underlying(ty));
        match &*kind {
            CheckedType::Struct(fields) => {
                let named = fields.iter().enumerate();
                let named = named.filter(|(_, field)| field.name != shared.blank);
                Ok(Compared::Fields(
                    named.map(|(at, field)| (at as u32, field.ty)).collect(),
                ))
            }
            CheckedType::Array { len, elem } => {
                let of = self.memo.lower_type(shared, self.b.types(), *elem)?;
                Ok(Compared::Elements(*len, of, *elem))
            }
            _ => Err("a comparison of values of this type"),
        }
    }

    /// The parts of the type whose equality or hash is glue of their
    /// own, which the glue of the type calls: a field that is no scalar
    /// and no string, and the elements of an array.
    pub fn compared_parts(
        &mut self,
        ty: CheckedTypeId,
    ) -> Result<Vec<CheckedTypeId>, &'static str> {
        let mut parts = Vec::new();
        match self.compared(ty)? {
            Compared::Fields(fields) => {
                for (_, part) in fields {
                    if !matches!(self.compared(part)?, Compared::Scalar | Compared::Text) {
                        parts.push(part);
                    }
                }
            }
            Compared::Elements(_, _, part) => parts.push(part),
            Compared::Bytes(_) | Compared::Scalar | Compared::Text | Compared::Held => {}
        }
        Ok(parts)
    }

    // ---- equality

    /// Whether the values of the type `ty` in the two places are equal.
    fn equal_places(
        &mut self,
        ty: CheckedTypeId,
        a: Place,
        b: Place,
    ) -> Built<Result<Operand, &'static str>> {
        let (a_value, b_value) = (Operand::Copy(a.clone()), Operand::Copy(b.clone()));
        match self.compared(ty) {
            Err(what) => Ok(Err(what)),
            Ok(Compared::Scalar) => {
                let equal = Rvalue::Binary(BinOp::Eq, a_value, b_value);
                Ok(Ok(slot(self.b.assign_temp(equal)?)))
            }
            Ok(Compared::Text) => {
                let string = self.b.operand_ty(&a_value);
                let bool = TypeInterner::BOOL;
                let args = vec![a_value, b_value];
                let equal = self.runtime("_gopp_string_eq", &[string, string], bool, args)?;
                Ok(Ok(equal))
            }
            Ok(_) => match self.glue(Glue::Eq, ty)? {
                Err(what) => Ok(Err(what)),
                Ok(glue) => {
                    let callee = self.glue_value(Glue::Eq, glue);
                    let args = vec![self.record(a)?, self.record(b)?];
                    Ok(Ok(self.called(callee, args, TypeInterner::BOOL)?))
                }
            },
        }
    }

    /// A call of a function that is a value, which gives a `ret`.
    fn called(&mut self, callee: Operand, args: Vec<Operand>, ret: TypeId) -> Built<Operand> {
        let result = Place::local(self.b.temp(ret));
        let next = self.b.new_block();
        let unwind = crate::UnwindAction::Continue;
        self.b
            .call(callee, args, result.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        Ok(Operand::Copy(result))
    }

    /// `first == second` or `first != second` for two values of a struct
    /// or an array type, which `of` is an expression of.
    pub fn equal(
        &mut self,
        span: SourceSpan,
        op: BinaryOp,
        of: ExprId,
        first: Operand,
        second: Operand,
    ) -> Built<Rvalue> {
        let ty = self.b.operand_ty(&first);
        let a = self.slot_of(Rvalue::Use(first))?;
        let b = self.slot_of(Rvalue::Use(second))?;
        let equal = match self.checked_as(of, ty) {
            Some(checked) => self.equal_places(checked, a, b)?,
            None => Err("a comparison of values whose type is not known"),
        };
        Ok(match (equal, op) {
            (Ok(equal), BinaryOp::Ne) => Rvalue::Unary(UnOp::Not, equal),
            (Ok(equal), _) => Rvalue::Use(equal),
            (Err(what), _) => self.placeholder(span, TypeInterner::BOOL, what),
        })
    }

    /// The two values that a glue is given, as places of the type.
    fn given_places(&mut self, lowered: TypeId, count: u32) -> Built<Vec<Place>> {
        let to = self.b.types().ptr(lowered);
        let mut places = Vec::with_capacity(count as usize);
        for at in 0..count {
            let given = Operand::Copy(Place::local(self.b.param(at)));
            let typed = self
                .b
                .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, given, to))?;
            places.push(self.b.deref(&Place::local(typed))?);
        }
        Ok(places)
    }

    /// The body of the equality glue of the type.
    pub fn eq_body(&mut self, ty: CheckedTypeId) -> Built<Result<(), &'static str>> {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let lowered = match self.memo.lower_type(shared, self.b.types(), ty) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let how = match self.compared(ty) {
            Ok(how) => how,
            Err(what) => return Ok(Err(what)),
        };
        let places = self.given_places(lowered, 2)?;
        let [a, b] = &places[..] else {
            return Ok(Err("an equality of two values"));
        };
        let result = Place::local(self.b.result());
        let word = self.b.types().int(64, false);
        let bytes = self.bytes_ty();
        let bool = TypeInterner::BOOL;
        let given = |this: &Self, at| Operand::Copy(Place::local(this.b.param(at)));
        let equal = match how {
            Compared::Held => return Ok(Err("equality glue of an interface type")),
            Compared::Scalar | Compared::Text => {
                match self.equal_places(ty, a.clone(), b.clone())? {
                    Ok(equal) => equal,
                    Err(what) => return Ok(Err(what)),
                }
            }
            Compared::Bytes(size) => {
                let args = vec![
                    given(self, 0),
                    given(self, 1),
                    self.b.const_int(word, size)?,
                ];
                self.runtime("_gopp_mem_eq", &[bytes, bytes, word], bool, args)?
            }
            Compared::Fields(fields) => {
                let differ = self.b.new_block();
                for (at, part) in fields {
                    let (a, b) = (self.b.field(a, at)?, self.b.field(b, at)?);
                    let equal = match self.equal_places(part, a, b)? {
                        Ok(equal) => equal,
                        Err(what) => return Ok(Err(what)),
                    };
                    let next = self.b.new_block();
                    self.b.switch(equal, vec![(0, differ)], next)?;
                    self.b.switch_to(next);
                }
                self.b
                    .assign(result.clone(), Rvalue::Use(self.b.const_bool(true)))?;
                self.b.ret()?;
                self.b.switch_to(differ);
                self.b.const_bool(false)
            }
            Compared::Elements(0, ..) => self.b.const_bool(true),
            Compared::Elements(count, of, part) => {
                let glue = match self.glue(Glue::Eq, part)? {
                    Ok(glue) => glue,
                    Err(what) => return Ok(Err(what)),
                };
                let (a, b) = (self.b.const_index(a, 0)?, self.b.const_index(b, 0)?);
                let size = self.size_of(of);
                let args = vec![
                    self.record(a)?,
                    self.record(b)?,
                    self.b.const_int(word, count)?,
                    self.b.const_int(word, size)?,
                    self.glue_value(Glue::Eq, glue),
                ];
                let params = [bytes, bytes, word, word, self.glue_sig(Glue::Eq)];
                self.runtime("_gopp_each_eq", &params, bool, args)?
            }
        };
        self.b.assign(result, Rvalue::Use(equal))?;
        self.b.ret()?;
        Ok(Ok(()))
    }

    // ---- hashes

    /// The hash of the value of the type `ty` in the place, from the
    /// seed.
    fn hashed_place(
        &mut self,
        ty: CheckedTypeId,
        place: Place,
        seed: Operand,
    ) -> Built<Result<Operand, &'static str>> {
        let word = self.b.types().int(64, false);
        let lowered = self.b.place_ty(&place);
        match (self.compared(ty), self.kind(lowered)) {
            (Err(what), _) => Ok(Err(what)),
            (Ok(Compared::Scalar), Type::Float { bits }) => {
                let wide = self.b.types().float(64);
                let mut value = Operand::Copy(place);
                if bits != 64 {
                    let cast = Rvalue::Cast(CastKind::FloatToFloat, value, wide);
                    value = slot(self.b.assign_temp(cast)?);
                }
                let args = vec![value, seed];
                Ok(Ok(self.runtime(
                    "_gopp_hash_float",
                    &[wide, word],
                    word,
                    args,
                )?))
            }
            (Ok(Compared::Scalar), _) => {
                let size = self.size_of(lowered);
                let args = vec![self.record(place)?, self.b.const_int(word, size)?, seed];
                let params = [self.bytes_ty(), word, word];
                Ok(Ok(self.runtime("_gopp_hash_bytes", &params, word, args)?))
            }
            (Ok(Compared::Text), _) => {
                let args = vec![Operand::Copy(place), seed];
                Ok(Ok(self.runtime(
                    "_gopp_hash_string",
                    &[lowered, word],
                    word,
                    args,
                )?))
            }
            (Ok(_), _) => match self.glue(Glue::Hash, ty)? {
                Err(what) => Ok(Err(what)),
                Ok(glue) => {
                    let callee = self.glue_value(Glue::Hash, glue);
                    let args = vec![self.record(place)?, seed];
                    Ok(Ok(self.called(callee, args, word)?))
                }
            },
        }
    }

    /// The body of the hash glue of the type. Each part of a value has
    /// the hash of the parts before it as its seed.
    pub fn hash_body(&mut self, ty: CheckedTypeId) -> Built<Result<(), &'static str>> {
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        let shared = self.shared;
        let lowered = match self.memo.lower_type(shared, self.b.types(), ty) {
            Ok(lowered) => lowered,
            Err(what) => return Ok(Err(what)),
        };
        let how = match self.compared(ty) {
            Ok(how) => how,
            Err(what) => return Ok(Err(what)),
        };
        let places = self.given_places(lowered, 1)?;
        let [value] = &places[..] else {
            return Ok(Err("a hash of a value"));
        };
        let word = self.b.types().int(64, false);
        let bytes = self.bytes_ty();
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let seed = Operand::Copy(Place::local(self.b.param(1)));
        let hash = match how {
            Compared::Held => return Ok(Err("hash glue of an interface type")),
            Compared::Scalar | Compared::Text => {
                match self.hashed_place(ty, value.clone(), seed)? {
                    Ok(hash) => hash,
                    Err(what) => return Ok(Err(what)),
                }
            }
            Compared::Bytes(size) => {
                let args = vec![given, self.b.const_int(word, size)?, seed];
                self.runtime("_gopp_hash_bytes", &[bytes, word, word], word, args)?
            }
            Compared::Fields(fields) => {
                let mut hash = seed;
                for (at, part) in fields {
                    let field = self.b.field(value, at)?;
                    hash = match self.hashed_place(part, field, hash)? {
                        Ok(hash) => hash,
                        Err(what) => return Ok(Err(what)),
                    };
                }
                hash
            }
            Compared::Elements(0, ..) => seed,
            Compared::Elements(count, of, part) => {
                let glue = match self.glue(Glue::Hash, part)? {
                    Ok(glue) => glue,
                    Err(what) => return Ok(Err(what)),
                };
                let first = self.b.const_index(value, 0)?;
                let size = self.size_of(of);
                let args = vec![
                    self.record(first)?,
                    self.b.const_int(word, count)?,
                    self.b.const_int(word, size)?,
                    seed,
                    self.glue_value(Glue::Hash, glue),
                ];
                let params = [bytes, word, word, word, self.glue_sig(Glue::Hash)];
                self.runtime("_gopp_hash_each", &params, word, args)?
            }
        };
        let result = Place::local(self.b.result());
        self.b.assign(result, Rvalue::Use(hash))?;
        self.b.ret()?;
        Ok(Ok(()))
    }
}
