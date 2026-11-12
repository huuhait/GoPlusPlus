//! Maps: a table with one owner, which the runtime keeps (spec chapter
//! 8). A map that is given to a new owner is copied, a write to a nil
//! map makes the map where it is held (OWN-P9), and a range loop tells
//! the map that it is walked until the loop is left (RNG-4).

use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{Block, ExprId, ExprKind};
use lasso::Spur;

use super::Shared;
use super::expr::PlaceMode;
use super::func::{Built, FuncCx, Target};
use crate::{
    AggKind, BinOp, CastKind, Mutability, Operand, Place, Rvalue, Type, TypeId, TypeInterner,
};

/// The keys and the values of a map type, and what the runtime is told
/// of them: the size of a key, the size of a value, where the value is
/// in a slot, the size of a slot, and how keys are hashed and compared.
#[derive(Clone, Copy)]
pub(crate) struct MapOf {
    pub key: TypeId,
    pub value: TypeId,
    shape: [u64; 5],
}

/// How the runtime hashes and compares keys: by their bytes, as
/// strings, or as floats of 32 or of 64 bits.
const KEY_BYTES: u64 = 0;
const KEY_STRING: u64 = 1;
const KEY_FLOAT32: u64 = 2;
const KEY_FLOAT64: u64 = 3;

fn slot(local: crate::LocalId) -> Operand {
    Operand::Copy(Place::local(local))
}

impl FuncCx<'_, '_> {
    pub fn is_map(&self, ty: TypeId) -> bool {
        self.memo.map == Some(ty)
    }

    /// Whether the expression names a map, or a pointer to one.
    pub fn holds_map(&mut self, x: ExprId) -> bool {
        if self.memo.map.is_none() {
            return false;
        }
        let ty = self.ty_of(x);
        let behind = self.pointee(ty);
        self.is_map(ty) || self.is_map(behind)
    }

    /// The map and the key of `m[k]`.
    pub fn map_index(&mut self, expr: ExprId) -> Option<(ExprId, ExprId)> {
        let inner = Shared::unparen(self.tree, expr);
        let ExprKind::Index { x, indices } = &self.tree.exprs[inner].kind else {
            return None;
        };
        let (x, &[index]) = (*x, &indices[..]) else {
            return None;
        };
        self.holds_map(x).then_some((x, index))
    }

    /// What the map that the expression names has for keys and values;
    /// none, with a report, for a map that is not supported yet.
    pub fn map_of(&mut self, map: ExprId) -> Option<MapOf> {
        let types = self.shared.types;
        let mut ty = types.underlying(self.body.types.get(&map)?.ty);
        if let CheckedType::Pointer(to) = &*types.get(ty) {
            ty = types.underlying(*to);
        }
        let known = match self.memo.maps.get(&ty) {
            Some(&known) => known,
            None => {
                let kind = types.get(ty);
                let CheckedType::Map { key, value } = &*kind else {
                    return None;
                };
                let known = self.map_shape(*key, *value);
                self.memo.maps.insert(ty, known);
                known
            }
        };
        match known {
            Ok(of) => Some(of),
            Err(what) => {
                self.unsupported(self.tree.exprs[map].span, what);
                None
            }
        }
    }

    fn map_shape(
        &mut self,
        key: CheckedTypeId,
        value: CheckedTypeId,
    ) -> Result<MapOf, &'static str> {
        let shared = self.shared;
        let key_ty = self.memo.lower_type(shared, self.b.types(), key)?;
        let value_ty = self.memo.lower_type(shared, self.b.types(), value)?;
        let kind = match self.kind(key_ty) {
            Type::String => KEY_STRING,
            Type::Float { bits: 32 } => KEY_FLOAT32,
            Type::Float { .. } => KEY_FLOAT64,
            _ if self.equal_by_bytes(key, key_ty) => KEY_BYTES,
            _ => return Err("a map whose keys are compared by a function"),
        };
        if !self.flat(value_ty) {
            return Err("a map whose values hold a slice or a map");
        }
        let mut layout = |ty: TypeId| {
            let layout = self.b.types().layout(ty, shared.layout);
            layout.map_or((0, 1), |layout| (layout.size, layout.align))
        };
        let ((key_size, key_align), (value_size, value_align)) = (layout(key_ty), layout(value_ty));
        let value_offset = key_size.next_multiple_of(value_align);
        let whole = key_align.max(value_align);
        // A slot takes room, so that the runtime finds a slot by its
        // address.
        let slot = (value_offset + value_size).next_multiple_of(whole).max(1);
        Ok(MapOf {
            key: key_ty,
            value: value_ty,
            shape: [key_size, value_size, value_offset, slot, kind],
        })
    }

    /// Whether two keys of the type are equal exactly when their bytes
    /// are: integers, booleans and pointers, with no room between them.
    fn equal_by_bytes(&mut self, key: CheckedTypeId, lowered: TypeId) -> bool {
        let shared = self.shared;
        let size = |this: &mut Self, ty: TypeId| {
            let layout = this.b.types().layout(ty, shared.layout);
            layout.map_or(0, |layout| layout.size)
        };
        let mut filled = 0u64;
        let mut rest = vec![(key, 1u64)];
        while let Some((ty, count)) = rest.pop() {
            self.memo.steps += 1;
            match &*shared.types.get(shared.types.underlying(ty)) {
                CheckedType::Array { len, elem } => rest.push((*elem, count.saturating_mul(*len))),
                CheckedType::Struct(fields) => {
                    rest.extend(fields.iter().map(|field| (field.ty, count)));
                }
                _ => {
                    let Ok(part) = self.memo.lower_type(shared, self.b.types(), ty) else {
                        return false;
                    };
                    let (Type::Unit | Type::Bool | Type::Int { .. } | Type::Ptr(_)) =
                        self.kind(part)
                    else {
                        return false;
                    };
                    filled = filled.saturating_add(count.saturating_mul(size(self, part)));
                }
            }
        }
        filled == size(self, lowered)
    }

    /// The record of the map type for the runtime, by its address.
    fn shape(&mut self, of: &MapOf) -> Built<Operand> {
        let wide = self.b.types().int(64, false);
        let mut parts = Vec::with_capacity(of.shape.len());
        for number in of.shape {
            parts.push(self.b.const_int(wide, number)?);
        }
        let record = self.slot_of(Rvalue::Aggregate(AggKind::Tuple, parts))?;
        self.record(record)
    }

    /// The place that holds the map which the expression names: what
    /// the pointer leads to, if it names a pointer to a map.
    pub fn map_place(&mut self, x: ExprId, mode: &mut PlaceMode) -> Built<Place> {
        let place = self.place(x, mode)?;
        match self.is_map(self.b.place_ty(&place)) {
            true => Ok(place),
            false => self.deref(place, self.tree.exprs[x].span, mode),
        }
    }

    /// The map that the expression names, as it is now.
    fn map_now(&mut self, x: ExprId) -> Built<Operand> {
        let place = self.map_place(x, &mut PlaceMode::default())?;
        Ok(slot(self.b.assign_temp(Rvalue::Use(Operand::Copy(place)))?))
    }

    /// The key in a slot of its own, by its address.
    pub fn key_at(&mut self, index: ExprId, of: &MapOf) -> Built<Operand> {
        let key = self.operand_as(index, of.key)?;
        let key = self.slot_of(Rvalue::Use(key))?;
        self.record(key)
    }

    /// What the key has in the map, or a zero, and whether the key is
    /// there.
    pub fn map_get(&mut self, map: Operand, of: &MapOf, key: Operand) -> Built<[Operand; 2]> {
        let value = Place::local(self.b.temp(of.value));
        let shape = self.shape(of)?;
        let bytes = self.bytes_ty();
        let params = [self.b.operand_ty(&map), bytes, bytes, bytes];
        let args = vec![map, shape, key, self.record(value.clone())?];
        let found = self.runtime("_gopp_map_read", &params, TypeInterner::BOOL, args)?;
        Ok([Operand::Copy(value), found])
    }

    /// `m[k]` as two values: `v, ok := m[k]`.
    pub fn map_read(&mut self, x: ExprId, index: ExprId) -> Built<Option<[Operand; 2]>> {
        let Some(of) = self.map_of(x) else {
            return Ok(None);
        };
        let map = self.map_now(x)?;
        let key = self.key_at(index, &of)?;
        self.map_get(map, &of, key).map(Some)
    }

    /// Where the value of the key is in the map that `map` holds. A key
    /// that is not there is put into the map, and a nil map is made
    /// first.
    pub fn map_entry(&mut self, map: Place, of: &MapOf, key: Operand) -> Built<Place> {
        let held = slot(self.b.assign_temp(Rvalue::AddrOf(map, Mutability::Mut))?);
        let shape = self.shape(of)?;
        let bytes = self.bytes_ty();
        let params = [self.b.operand_ty(&held), bytes, bytes];
        let at = self.runtime("_gopp_map_set", &params, bytes, vec![held, shape, key])?;
        let to = self.b.types().ptr(of.value);
        let at = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, at, to))?;
        self.b.deref(&Place::local(at))
    }

    /// `make(map[K]V)` and `make(map[K]V, hint)`.
    pub fn make_map(&mut self, expr: ExprId, ty: TypeId, sizes: &[ExprId]) -> Built<Rvalue> {
        let Some(of) = self.map_of(expr) else {
            return Ok(Rvalue::Use(self.zero(ty)));
        };
        let wide = self.b.types().int(64, true);
        let hint = match sizes.first() {
            Some(&hint) => self.as_int64(hint)?,
            None => self.zero(wide),
        };
        Ok(Rvalue::Use(self.made_map(ty, &of, hint)?))
    }

    /// An empty map with room for about `hint` entries.
    fn made_map(&mut self, ty: TypeId, of: &MapOf, hint: Operand) -> Built<Operand> {
        let shape = self.shape(of)?;
        let params = [self.bytes_ty(), self.b.operand_ty(&hint)];
        self.runtime("_gopp_map_make", &params, ty, vec![shape, hint])
    }

    /// `map[K]V{k: v, ...}`: the keys and the values are worked out from
    /// left to right, and then put into a new map.
    pub fn map_literal(&mut self, expr: ExprId, ty: TypeId, elems: &[ExprId]) -> Built<Rvalue> {
        let Some(of) = self.map_of(expr) else {
            return Ok(Rvalue::Use(self.zero(ty)));
        };
        let mut exprs = Vec::with_capacity(elems.len() * 2);
        let mut wanted = Vec::with_capacity(elems.len() * 2);
        for &elem in elems {
            if let ExprKind::KeyValue { key, value } = self.tree.exprs[elem].kind {
                exprs.extend([key, value]);
                wanted.extend([Some(of.key), Some(of.value)]);
            }
        }
        let values = self.values_each(&exprs, &wanted)?;
        let wide = self.b.types().int(64, true);
        let count = self.b.const_int(wide, elems.len() as u64)?;
        let made = self.made_map(ty, &of, count)?;
        let map = self.slot_of(Rvalue::Use(made))?;
        for pair in values.chunks_exact(2) {
            let key = self.slot_of(Rvalue::Use(pair[0].clone()))?;
            let key = self.record(key)?;
            let entry = self.map_entry(map.clone(), &of, key)?;
            self.store(entry, Rvalue::Use(pair[1].clone()))?;
        }
        Ok(Rvalue::Use(Operand::Copy(map)))
    }

    /// `delete(m, k)`.
    pub fn map_delete(&mut self, x: ExprId, index: ExprId) -> Built {
        let Some(of) = self.map_of(x) else {
            return Ok(());
        };
        let map = self.map_now(x)?;
        let key = self.key_at(index, &of)?;
        let shape = self.shape(&of)?;
        let bytes = self.bytes_ty();
        let params = [self.b.operand_ty(&map), bytes, bytes];
        self.runtime_call("_gopp_map_delete", &params, vec![map, shape, key])
    }

    /// `clear(m)`.
    pub fn clear_map(&mut self, x: ExprId) -> Built {
        let Some(of) = self.map_of(x) else {
            return Ok(());
        };
        let map = self.map_now(x)?;
        let shape = self.shape(&of)?;
        let params = [self.b.operand_ty(&map), self.bytes_ty()];
        self.runtime_call("_gopp_map_clear", &params, vec![map, shape])
    }

    /// `len(m)` as a number of the type.
    pub fn map_len(&mut self, ty: TypeId, x: ExprId) -> Built<Rvalue> {
        let map = self.map_now(x)?;
        let wide = self.b.types().int(64, false);
        let params = [self.b.operand_ty(&map)];
        let len = self.runtime("_gopp_map_len", &params, wide, vec![map])?;
        let integer = matches!(self.kind(ty), Type::Int { .. });
        Ok(match integer && ty != wide {
            true => Rvalue::Cast(CastKind::IntToInt, len, ty),
            false => Rvalue::Use(len),
        })
    }

    /// The map of its own that a new owner of the value gets.
    pub fn map_copy(&mut self, expr: ExprId, ty: TypeId, value: Rvalue) -> Built<Rvalue> {
        let Some(of) = self.map_of(expr) else {
            return Ok(value);
        };
        let map = self.operand_of(value)?;
        let shape = self.shape(&of)?;
        let params = [ty, self.bytes_ty()];
        let copy = self.runtime("_gopp_map_copy", &params, ty, vec![map, shape])?;
        Ok(Rvalue::Use(copy))
    }

    /// `for key, value := range m`. The runtime walks the map from a
    /// slot of its choice; each pass gets a copy of a key, and a copy of
    /// its value or, with `&`, the value where the map has it. The
    /// scope of the loop tells the runtime that the walk is over.
    pub fn range_map(
        &mut self,
        label: Option<Spur>,
        span: SourceSpan,
        lhs: &[ExprId],
        define: bool,
        x: ExprId,
        body: &Block,
    ) -> Built {
        self.open_scope();
        self.at(self.tree.exprs[x].span);
        let Some(of) = self.map_of(x) else {
            return self.close_scope(span);
        };
        let map = self.map_now(x)?;
        // What the runtime keeps of the walk: three words.
        let word = self.b.types().int(64, false);
        let walk = self.b.types().tuple(&[word; 3]);
        let walk = Place::local(self.b.temp(walk));
        let walk = self.record(walk)?;
        let bytes = self.bytes_ty();
        let params = [bytes, self.b.operand_ty(&map)];
        self.runtime_call("_gopp_map_iter_start", &params, vec![walk.clone(), map])?;
        self.walked(walk.clone());
        let key = Place::local(self.b.temp(of.key));
        let shape = self.shape(&of)?;
        let key_at = self.record(key.clone())?;
        let [head, inside, exit] = [(); 3].map(|()| self.b.new_block());
        self.b.goto(head)?;
        self.b.switch_to(head);
        let args = vec![walk, shape, key_at];
        let at = self.runtime("_gopp_map_iter_next", &[bytes; 3], bytes, args)?;
        let more = Rvalue::Binary(BinOp::Ne, at.clone(), self.zero(bytes));
        let more = slot(self.b.assign_temp(more)?);
        self.b.switch(more, vec![(0, exit)], inside)?;
        self.b.switch_to(inside);
        let to = self.b.types().ptr(of.value);
        let at = self
            .b
            .assign_temp(Rvalue::Cast(CastKind::PtrToPtr, at, to))?;
        let value = self.b.deref(&Place::local(at))?;
        let target = Target {
            label,
            breaks: exit,
            continues: Some(head),
            depth: self.depth(),
            next: None,
        };
        self.open_scope();
        if let Some(&left) = lhs.first() {
            self.set_range_var(left, define, Operand::Copy(key))?;
        }
        match lhs.get(1).map(|&left| (left, self.aliased(left))) {
            Some((_, Some(named))) => self.set_range_alias(named, value)?,
            Some((left, None)) => self.set_range_var(left, define, Operand::Copy(value))?,
            None => {}
        }
        self.targets.push(target);
        self.block(body)?;
        self.targets.pop();
        self.close_scope(span)?;
        self.b.goto(head)?;
        self.b.switch_to(exit);
        self.close_scope(span)
    }
}
