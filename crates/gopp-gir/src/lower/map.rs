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
use super::func::{Built, FuncCx, Target, deeper};
use super::glue::Glue;
use crate::{
    AggKind, BinOp, CastKind, Constant, ConstantKind, Mutability, Operand, Place, Rvalue, Type,
    TypeId, TypeInterner,
};

/// The keys and the values of a map type, and what the runtime is told
/// of them: the size of a key, the size of a value, where the value is
/// in a slot, the size of a slot, and how keys are hashed and compared.
#[derive(Clone, Copy)]
pub(crate) struct MapOf {
    pub key: TypeId,
    pub value: TypeId,
    shape: [u64; 5],
    /// The clone glue of a value whose copy is more than its bytes.
    copy: Option<crate::FuncId>,
    /// The hash and the equality glue of keys that the runtime cannot
    /// compare itself.
    compare: Option<[crate::FuncId; 2]>,
    /// The drop glue of a value that holds what is freed.
    pub drop: Option<crate::FuncId>,
    /// The clone and the drop glue of a key that holds what is freed.
    keys: Option<[crate::FuncId; 2]>,
}

/// The maps on the way to an entry `m[a][b]`, the outermost first: what
/// each is, and its key by its address.
pub(super) type Way = Vec<(MapOf, Operand)>;

/// How the runtime hashes and compares keys: by their bytes, as
/// strings, as floats of 32 or of 64 bits, or by the glue of their
/// type.
const KEY_BYTES: u64 = 0;
const KEY_STRING: u64 = 1;
const KEY_FLOAT32: u64 = 2;
const KEY_FLOAT64: u64 = 3;
const KEY_GLUE: u64 = 4;

fn slot(local: crate::LocalId) -> Operand {
    Operand::Copy(Place::local(local))
}

impl FuncCx<'_, '_> {
    pub fn is_map(&self, ty: TypeId) -> bool {
        self.memo.map == Some(ty)
    }

    /// Whether the expression names a map, or a pointer to one.
    pub fn holds_map(&mut self, x: ExprId) -> bool {
        // The type of a map is known once a type with a map is lowered:
        // that of `x` may be the first.
        let ty = self.ty_of(x);
        if self.memo.map.is_none() {
            return false;
        }
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
        if !matches!(&*types.get(ty), CheckedType::Map { .. }) {
            return None;
        }
        match self.map_of_type(ty) {
            Ok(of) => Some(of),
            Err(what) => {
                self.unsupported(self.tree.exprs[map].span, what);
                None
            }
        }
    }

    /// What a map type has for keys and values, or why it is not
    /// supported yet.
    pub fn map_of_type(&mut self, ty: CheckedTypeId) -> Result<MapOf, &'static str> {
        let types = self.shared.types;
        let ty = types.underlying(ty);
        if let Some(&known) = self.memo.maps.get(&ty) {
            return known;
        }
        let kind = types.get(ty);
        let CheckedType::Map { key, value } = &*kind else {
            return Err("a map of this type");
        };
        let known = self.map_shape(*key, *value);
        self.memo.maps.insert(ty, known);
        known
    }

    fn map_shape(
        &mut self,
        key: CheckedTypeId,
        value: CheckedTypeId,
    ) -> Result<MapOf, &'static str> {
        let shared = self.shared;
        let key_ty = self.memo.lower_type(shared, self.b.types(), key)?;
        let value_ty = self.memo.lower_type(shared, self.b.types(), value)?;
        let mut compare = None;
        let kind = match self.kind(key_ty) {
            Type::String => KEY_STRING,
            Type::Float { bits: 32 } => KEY_FLOAT32,
            Type::Float { .. } => KEY_FLOAT64,
            _ if self.equal_by_bytes(key, key_ty) => KEY_BYTES,
            _ => {
                let glue = |this: &mut Self, kind| match this.glue(kind, key) {
                    Ok(glue) => glue,
                    Err(_) => Err("a map whose keys cannot be compared"),
                };
                compare = Some([glue(self, Glue::Hash)?, glue(self, Glue::Eq)?]);
                KEY_GLUE
            }
        };
        // A value whose copy is more than its bytes is copied by its
        // glue when the map is.
        let glue = |this: &mut Self, kind, of, checked| match this.flat(of) {
            true => Ok(None),
            false => match this.glue(kind, checked) {
                Ok(Ok(glue)) => Ok(Some(glue)),
                Ok(Err(what)) => Err(what),
                Err(_) => Err("a map whose entries cannot be copied"),
            },
        };
        // The copy of a map copies its values.
        if shared.types.copy_rule(value).is_none() {
            return Err("a map whose values cannot be copied");
        }
        let copy = glue(self, Glue::Clone, value_ty, value)?;
        let drop = glue(self, Glue::Drop, value_ty, value)?;
        // A key that is put into the map is a copy of its own too.
        let keys = match glue(self, Glue::Clone, key_ty, key)? {
            Some(copy) => glue(self, Glue::Drop, key_ty, key)?.map(|drop| [copy, drop]),
            None => None,
        };
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
            copy,
            compare,
            drop,
            keys,
        })
    }

    /// Whether two values of the type are equal exactly when their
    /// bytes are: integers, booleans and pointers, with no room between
    /// them and no field without a name, which a comparison leaves out.
    pub fn equal_by_bytes(&mut self, key: CheckedTypeId, lowered: TypeId) -> bool {
        if let Some(&known) = self.memo.by_bytes.get(&key) {
            return known;
        }
        let known = deeper(|| self.bytes_say_all(key, lowered));
        self.memo.by_bytes.insert(key, known);
        known
    }

    fn bytes_say_all(&mut self, ty: CheckedTypeId, lowered: TypeId) -> bool {
        self.memo.steps += 1;
        let shared = self.shared;
        let size = |this: &mut Self, ty: TypeId| {
            let layout = this.b.types().layout(ty, shared.layout);
            layout.map_or(0, |layout| layout.size)
        };
        let lower = |this: &mut Self, part| this.memo.lower_type(shared, this.b.types(), part).ok();
        match &*shared.types.get(shared.types.underlying(ty)) {
            CheckedType::Array { elem, .. } => {
                lower(self, *elem).is_some_and(|of| self.equal_by_bytes(*elem, of))
            }
            CheckedType::Struct(fields) => {
                let mut filled = 0u64;
                // The parts of the parents are parts of the value too.
                let parents = shared.types.parents(ty);
                let parents = parents.iter().map(|&parent| (parent, false));
                let own = fields.iter();
                let own = own.map(|field| (field.ty, field.name == shared.blank));
                for (of, blank) in parents.chain(own) {
                    let part = lower(self, of).filter(|_| !blank);
                    let Some(part) = part.filter(|&part| self.equal_by_bytes(of, part)) else {
                        return false;
                    };
                    filled += size(self, part);
                }
                filled == size(self, lowered)
            }
            _ => matches!(
                self.kind(lowered),
                Type::Unit | Type::Bool | Type::Int { .. } | Type::Ptr(_)
            ),
        }
    }

    /// The record of the map type for the runtime, by its address.
    fn shape(&mut self, of: &MapOf) -> Built<Operand> {
        let wide = self.b.types().int(64, false);
        let mut parts = Vec::with_capacity(of.shape.len());
        for number in of.shape {
            parts.push(self.b.const_int(wide, number)?);
        }
        // The glue of the values and of the keys, each or null.
        let [hash, eq] = match of.compare {
            Some([hash, eq]) => [Some(hash), Some(eq)],
            None => [None, None],
        };
        let [copy_key, drop_key] = match of.keys {
            Some([copy, drop]) => [Some(copy), Some(drop)],
            None => [None, None],
        };
        let glues = [
            (Glue::Clone, of.copy),
            (Glue::Hash, hash),
            (Glue::Eq, eq),
            (Glue::Drop, of.drop),
            (Glue::Clone, copy_key),
            (Glue::Drop, drop_key),
        ];
        for (kind, glue) in glues {
            parts.push(match glue {
                Some(glue) => self.glue_value(kind, glue),
                None => {
                    let (kind, ty) = (ConstantKind::Null, self.glue_sig(kind));
                    Operand::Const(Constant { kind, ty })
                }
            });
        }
        // What a new value starts as, or null for all zero.
        let bytes = self.bytes_ty();
        parts.push(match self.blank_at(of.value)? {
            Some(blank) => blank,
            None => Operand::Const(Constant {
                kind: ConstantKind::Null,
                ty: bytes,
            }),
        });
        let record = self.slot_of(Rvalue::Aggregate(AggKind::Tuple, parts))?;
        self.record(record)
    }

    /// The place that holds the map which the expression names: what
    /// the pointer leads to, if it names a pointer to a map.
    pub fn map_place(&mut self, x: ExprId, mode: &mut PlaceMode) -> Built<Place> {
        let place = self.place(x, mode)?;
        match self.is_map(self.b.place_ty(&place)) {
            true => Ok(place),
            false => self.deref(place, mode),
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

    /// The way of `m[a][b]` to an entry: the place that holds the
    /// outermost map, and each map on the way with its key by its
    /// address, the outermost first. None for a map that is not
    /// supported, which was said.
    pub fn entries(&mut self, expr: ExprId, mode: &mut PlaceMode) -> Built<Option<(Place, Way)>> {
        let mut way = Vec::new();
        let mut at = expr;
        while let Some((map, index)) = self.map_index(at) {
            way.push((map, index));
            at = map;
        }
        let Some(&(outermost, _)) = way.last() else {
            return Ok(None);
        };
        let place = self.map_place(outermost, mode)?;
        let mut keyed = Vec::with_capacity(way.len());
        for &(map, index) in way.iter().rev() {
            let Some(of) = self.map_of(map) else {
                return Ok(None);
            };
            keyed.push((of, self.key_at(index, &of)?));
        }
        Ok(Some((place, keyed)))
    }

    /// Where the value at the end of the way is. Each key that is not
    /// there is put into its map, and a nil map on the way is made:
    /// `mm[a][b] = v` puts `a` into `mm` (OWN-P9).
    pub fn entry_at(&mut self, map: Place, way: &Way) -> Built<Place> {
        let mut place = map;
        for (of, key) in way {
            place = self.map_entry(place, of, key.clone())?;
        }
        Ok(place)
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
        // The map takes the values; of a key it makes a copy of its own.
        let mut values = self.values_each(&exprs, &wanted)?;
        for (at, value) in values.iter_mut().enumerate().skip(1).step_by(2) {
            let taken = self.taken(exprs[at], of.value, Rvalue::Use(value.clone()))?;
            *value = self.operand_of(taken)?;
        }
        self.empty_moved()?;
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
        self.map_forget(map, &of, Some(key))
    }

    /// The map has the key at the address no more; without a key, it
    /// has no key any more.
    pub fn map_forget(&mut self, map: Operand, of: &MapOf, key: Option<Operand>) -> Built {
        let shape = self.shape(of)?;
        let bytes = self.bytes_ty();
        match key {
            Some(key) => {
                let params = [self.b.operand_ty(&map), bytes, bytes];
                self.runtime_call("_gopp_map_delete", &params, vec![map, shape, key])
            }
            None => {
                let params = [self.b.operand_ty(&map), bytes];
                self.runtime_call("_gopp_map_clear", &params, vec![map, shape])
            }
        }
    }

    /// `clear(m)`.
    pub fn clear_map(&mut self, x: ExprId) -> Built {
        let Some(of) = self.map_of(x) else {
            return Ok(());
        };
        let map = self.map_now(x)?;
        self.map_forget(map, &of, None)
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
        Ok(Rvalue::Use(self.map_copied(ty, &of, map)?))
    }

    /// The map ends: its values, and its table.
    pub fn map_dropped(&mut self, ty: TypeId, of: &MapOf, map: Operand) -> Built {
        let shape = self.shape(of)?;
        let params = [ty, self.bytes_ty()];
        self.runtime_call("_gopp_map_drop", &params, vec![map, shape])
    }

    /// A map of its own with the entries of `map`, which is of the
    /// type `ty`.
    pub fn map_copied(&mut self, ty: TypeId, of: &MapOf, map: Operand) -> Built<Operand> {
        let shape = self.shape(of)?;
        let params = [ty, self.bytes_ty()];
        self.runtime("_gopp_map_copy", &params, ty, vec![map, shape])
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
        // A map that is made for the loop lives as long as the loop.
        let mark = self.temps.len();
        let map = self.map_now(x)?;
        self.keep_temps(mark)?;
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
            self.set_range_var(left, define, Operand::Copy(key), false)?;
        }
        match lhs.get(1).map(|&left| (left, self.aliased(left))) {
            Some((_, Some(named))) => self.set_range_alias(named, value)?,
            Some((left, None)) => self.set_range_var(left, define, Operand::Copy(value), false)?,
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
