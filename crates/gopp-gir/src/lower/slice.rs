//! Slices: a value with a buffer of its own (ADR-0041, spec chapter 8).
//! A slice that is given to a new owner is copied, part of a slice is a
//! copy, the same elements in place, or a view of them, and the runtime
//! makes, grows and copies the buffers.

use gopp_sema::Transfer;
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{ExprId, ExprKind, UnaryOp};

use super::Shared;
use super::expr::PlaceMode;
use super::func::{Built, FuncCx, deeper};
use super::glue::Glue;
use crate::{
    AggKind, AssertKind, BinOp, CastKind, LocalId, LocalKind, Mutability, Operand, Place, Rvalue,
    Type, TypeId, TypeInterner,
};

/// What `x[lo:hi]` of a slice or an array is made as.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Part {
    /// A new slice with copies of the elements.
    Value,
    /// The same slice with other bounds: `s = s[lo:hi]`.
    InPlace,
    /// `&x[lo:hi]`: the same elements, in a record of its own.
    View,
}

fn slot(local: LocalId) -> Operand {
    Operand::Copy(Place::local(local))
}

impl FuncCx<'_, '_> {
    /// Whether a value of the type is copied by its bytes and owns
    /// nothing that is freed: no string, no slice and no map is in it,
    /// behind no pointer.
    pub fn flat(&mut self, ty: TypeId) -> bool {
        if let Some(&known) = self.memo.flat.get(&ty) {
            return known;
        }
        let known = deeper(|| self.owns_nothing(ty));
        self.memo.flat.insert(ty, known);
        known
    }

    /// Each type is asked once: a type that holds many others asks them,
    /// and does not walk down to their parts itself.
    fn owns_nothing(&mut self, ty: TypeId) -> bool {
        self.memo.steps += 1;
        let parts = match self.kind(ty) {
            Type::String | Type::Slice(_) => return false,
            Type::Ptr(_) => return !self.is_map(ty),
            Type::Array(of, _) | Type::Option(of) => vec![of],
            Type::Tuple(parts) => parts.to_vec(),
            Type::Struct(id) => {
                let fields = self.b.types().struct_fields(id).unwrap_or_default();
                fields.to_vec()
            }
            _ => return true,
        };
        parts.into_iter().all(|part| self.flat(part))
    }

    /// Whether Go finds a pointer in a value of the type: its
    /// allocator rounds the room of such values otherwise.
    fn has_pointers(&mut self, ty: TypeId) -> bool {
        let mut rest = vec![ty];
        while let Some(ty) = rest.pop() {
            match self.kind(ty) {
                Type::Ptr(_) | Type::String | Type::Slice(_) | Type::Func(_) => return true,
                Type::Array(of, _) | Type::Option(of) => rest.push(of),
                Type::Tuple(parts) => rest.extend(parts.iter()),
                Type::Struct(id) => {
                    let fields = self.b.types().struct_fields(id).unwrap_or_default();
                    rest.extend(fields.to_vec());
                }
                _ => {}
            }
        }
        false
    }

    /// The size of an element and whether it holds no pointer, as the
    /// runtime is told them.
    fn element_facts(&mut self, elem: TypeId) -> Built<[Operand; 2]> {
        let word = super::Memo::word(self.shared, self.b.types(), false);
        let layout = self.b.types().layout(elem, self.shared.layout);
        let size = layout.map_or(0, |layout| layout.size);
        let plain = !self.has_pointers(elem);
        Ok([self.b.const_int(word, size)?, self.b.const_bool(plain)])
    }

    /// The address of the place, as the runtime takes it.
    pub fn record(&mut self, place: Place) -> Built<Operand> {
        let address = self.b.assign_temp(Rvalue::AddrOf(place, Mutability::Mut))?;
        let byte = self.b.types().int(8, false);
        let bytes = self.b.types().ptr(byte);
        let cast = Rvalue::Cast(CastKind::PtrToPtr, slot(address), bytes);
        Ok(slot(self.b.assign_temp(cast)?))
    }

    pub fn bytes_ty(&mut self) -> TypeId {
        let byte = self.b.types().int(8, false);
        self.b.types().ptr(byte)
    }

    /// The value in a slot of its own, whose address can be taken.
    pub fn slot_of(&mut self, value: Rvalue) -> Built<Place> {
        Ok(Place::local(self.b.assign_temp(value)?))
    }

    /// The header of the slice that the expression names, read where it
    /// is: the elements are those of the slice itself.
    fn header(&mut self, expr: ExprId, span: SourceSpan) -> Built<Place> {
        let mut mode = PlaceMode::default();
        let place = self.place(expr, &mut mode)?;
        let place = self.through(place, span, &mut mode)?;
        self.slot_of(Rvalue::Use(Operand::Copy(place)))
    }

    /// Whether the two expressions name one place: a variable, a field
    /// of one, or what a pointer in one points to.
    pub fn same_place(&self, a: ExprId, b: ExprId) -> bool {
        let (a, b) = (Shared::unparen(self.tree, a), Shared::unparen(self.tree, b));
        match (&self.tree.exprs[a].kind, &self.tree.exprs[b].kind) {
            (ExprKind::Name(first), ExprKind::Name(second)) => {
                first == second && self.body.uses.get(&a) == self.body.uses.get(&b)
            }
            (
                ExprKind::Selector { x, sel },
                ExprKind::Selector {
                    x: other,
                    sel: other_sel,
                },
            ) => sel.name == other_sel.name && self.same_place(*x, *other),
            (
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                },
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x: other,
                },
            ) => self.same_place(*x, *other),
            _ => false,
        }
    }

    /// The value of the expression for its new owner. A place whose
    /// value the checker says is copied gets the copy of its type:
    /// nothing for a string, a new buffer for a slice, a new table for a
    /// map, and what the glue of its type does for a value that holds
    /// such values.
    pub fn given(&mut self, expr: ExprId, ty: TypeId, value: Rvalue) -> Built<Rvalue> {
        let inner = Shared::unparen(self.tree, expr);
        if self.flat(ty) || self.reused == Some(inner) || self.lent.contains(&inner) {
            return Ok(value);
        }
        let span = self.tree.exprs[expr].span;
        let mark = self.body.var_modes.get(&inner);
        let handed = self.handed.contains(&inner);
        if mark.is_none() && !handed {
            return Ok(value);
        }
        // Nothing is moved out of a map: the value of an entry is read
        // into a slot, and the map keeps its own.
        let moved = (mark == Some(&Transfer::Move) || handed) && self.map_index(inner).is_none();
        let own = match &value {
            // The place gives its value up and is empty then.
            Rvalue::Use(Operand::Copy(from)) if moved => {
                self.moved.push((from.clone(), ty));
                Rvalue::Use(Operand::Copy(self.slot_of(value)?))
            }
            _ => match self.kind(ty) {
                // A copy of a string shares its bytes, which are
                // counted once more.
                Type::String => {
                    let copy = self.slot_of(value)?;
                    self.counted(copy.clone(), true)?;
                    Rvalue::Use(Operand::Copy(copy))
                }
                Type::Slice(of) if self.flat(of) => {
                    let copy = self.slot_of(value)?;
                    let [size, _] = self.element_facts(of)?;
                    let record = self.record(copy.clone())?;
                    let params = [self.bytes_ty(), self.b.operand_ty(&size)];
                    self.runtime_call("_gopp_slice_copy", &params, vec![record, size])?;
                    Rvalue::Use(Operand::Copy(copy))
                }
                Type::Ptr(_) if self.is_map(ty) => self.map_copy(expr, ty, value)?,
                _ => {
                    let checked = self.checked_as(expr, ty);
                    self.cloned_value(span, checked, value)?
                }
            },
        };
        self.fresh(expr, ty, own)
    }

    /// The value is new and its own. The temporary that holds it owns
    /// it until something takes it, and frees it when its statement
    /// ends if nothing does.
    pub fn fresh(&mut self, expr: ExprId, ty: TypeId, value: Rvalue) -> Built<Rvalue> {
        if self.flat(ty) {
            return Ok(value);
        }
        let place = match &value {
            Rvalue::Use(Operand::Const(_)) => return Ok(value),
            Rvalue::Use(Operand::Copy(place))
                if place.projections.is_empty()
                    && self.b.local_kind(place.local) == LocalKind::Temp =>
            {
                place.clone()
            }
            _ => self.slot_of(value)?,
        };
        if !self.owns_temp(&place) {
            let checked = self.checked_as(expr, ty);
            if let Some(ends) = self.ends_of(checked, self.tree.exprs[expr].span)? {
                self.own_temp(place.clone(), ends);
            }
        }
        Ok(Rvalue::Use(Operand::Copy(place)))
    }

    /// The value for a new owner. A temporary that owns it gives it up;
    /// anything else is read where it is, and the new owner gets a
    /// copy.
    pub fn taken(&mut self, expr: ExprId, ty: TypeId, value: Rvalue) -> Built<Rvalue> {
        if self.flat(ty) {
            return Ok(value);
        }
        match &value {
            Rvalue::Use(Operand::Const(_)) => return Ok(value),
            Rvalue::Use(Operand::Copy(place)) if self.take_temp(place) => return Ok(value),
            _ => {}
        }
        if self.reused == Some(Shared::unparen(self.tree, expr)) {
            return Ok(value);
        }
        let checked = self.checked_as(expr, ty);
        self.cloned_value(self.tree.exprs[expr].span, checked, value)
    }

    /// The value for a parameter that its function only reads: the
    /// caller stays the owner. A temporary keeps the value until its
    /// statement ends, and a place that cannot change during the call
    /// is read where it is (`lent`); of any other place a copy is made
    /// first, which the call cannot change (PAR-1).
    pub fn lent_value(&mut self, expr: ExprId, ty: TypeId, value: Rvalue) -> Built<Rvalue> {
        if self.flat(ty) || self.lent.contains(&Shared::unparen(self.tree, expr)) {
            return Ok(value);
        }
        match &value {
            Rvalue::Use(Operand::Const(_)) => return Ok(value),
            Rvalue::Use(Operand::Copy(place)) if self.owns_temp(place) => return Ok(value),
            _ => {}
        }
        let checked = self.checked_as(expr, ty);
        let copy = self.cloned_value(self.tree.exprs[expr].span, checked, value)?;
        self.fresh(expr, ty, copy)
    }

    /// The values of the expressions for new owners (see `values`).
    pub fn owned_values(
        &mut self,
        exprs: &[ExprId],
        wanted: &[Option<TypeId>],
    ) -> Built<Vec<Rvalue>> {
        let values = self.values(exprs, wanted)?;
        let mut owned = Vec::with_capacity(values.len());
        for (at, value) in values.into_iter().enumerate() {
            // One call gives all of them: each is a part of what the
            // call gave.
            let of = exprs.get(at).or(exprs.first());
            owned.push(match (wanted.get(at).copied().flatten(), of) {
                (Some(ty), Some(&expr)) => self.taken(expr, ty, value)?,
                _ => value,
            });
        }
        self.empty_moved()?;
        Ok(owned)
    }

    /// One value for each expression, each for a new owner.
    pub fn owned_each(
        &mut self,
        exprs: &[ExprId],
        wanted: &[Option<TypeId>],
    ) -> Built<Vec<Operand>> {
        let values = self.values_each(exprs, wanted)?;
        let mut owned = Vec::with_capacity(values.len());
        for ((value, &expr), wanted) in values.into_iter().zip(exprs).zip(wanted) {
            let value = match *wanted {
                Some(ty) => self.taken(expr, ty, Rvalue::Use(value))?,
                None => Rvalue::Use(value),
            };
            owned.push(self.operand_of(value)?);
        }
        self.empty_moved()?;
        Ok(owned)
    }

    /// An integer of any type as an `int64`, for the runtime.
    pub fn as_int64(&mut self, expr: ExprId) -> Built<Operand> {
        let wide = self.b.types().int(64, true);
        let given = self.operand(expr)?;
        let from = self.b.operand_ty(&given);
        match (from == wide, self.kind(from)) {
            (false, Type::Int { .. }) => {
                let cast = Rvalue::Cast(CastKind::IntToInt, given, wide);
                Ok(slot(self.b.assign_temp(cast)?))
            }
            (true, _) => Ok(given),
            _ => self.operand_as(expr, wide),
        }
    }

    /// `make([]T, len)` and `make([]T, len, cap)`.
    pub fn make_slice(&mut self, ty: TypeId, of: TypeId, sizes: &[ExprId]) -> Built<Rvalue> {
        let wide = self.b.types().int(64, true);
        let len = match sizes.first() {
            Some(&len) => self.as_int64(len)?,
            None => self.zero(wide),
        };
        let len = slot(self.b.assign_temp(Rvalue::Use(len))?);
        let cap = match sizes.get(1) {
            Some(&cap) => self.as_int64(cap)?,
            None => len.clone(),
        };
        self.made(ty, of, len, cap)
    }

    /// A slice of `len` zero elements with room for `cap`.
    fn made(&mut self, ty: TypeId, of: TypeId, len: Operand, cap: Operand) -> Built<Rvalue> {
        let out = self.slot_of(Rvalue::Use(self.zero(ty)))?;
        let [size, _] = self.element_facts(of)?;
        let record = self.record(out.clone())?;
        let wide = self.b.types().int(64, true);
        let params = [self.bytes_ty(), wide, wide, self.b.operand_ty(&size)];
        self.runtime_call("_gopp_slice_make", &params, vec![record, len, cap, size])?;
        Ok(Rvalue::Use(Operand::Copy(out)))
    }

    /// `[]T{...}`: a slice with the elements at their places; `set` has
    /// each index with its value.
    pub fn slice_literal(
        &mut self,
        ty: TypeId,
        of: TypeId,
        count: u64,
        set: Vec<(u64, Operand)>,
    ) -> Built<Rvalue> {
        let wide = self.b.types().int(64, true);
        let count = self.b.const_int(wide, count)?;
        let made = self.made(ty, of, count.clone(), count)?;
        let out = self.slot_of(made)?;
        let word = super::Memo::word(self.shared, self.b.types(), false);
        for (at, value) in set {
            let index = self.b.const_int(word, at)?;
            let index = self.b.assign_temp(Rvalue::Use(index))?;
            let element = self.b.index(&out, index)?;
            self.b.assign(element, Rvalue::Use(value))?;
        }
        Ok(Rvalue::Use(Operand::Copy(out)))
    }

    /// `append(s, values...)` and `append(s, other...)`. The slice is
    /// the function's own: a place that is read again was copied, and
    /// `s = append(s, ...)` grows `s` where it is.
    pub fn append(
        &mut self,
        span: SourceSpan,
        ty: TypeId,
        args: &[ExprId],
        spread: bool,
    ) -> Built<Rvalue> {
        let (Type::Slice(of), Some((&first, more))) = (self.kind(ty), args.split_first()) else {
            return Ok(Rvalue::Use(self.zero(ty)));
        };
        let own = self.rvalue_as(first, ty)?;
        let own = self.taken(first, ty, own)?;
        let own = self.slot_of(own)?;
        let [size, plain] = self.element_facts(of)?;
        let bytes = self.bytes_ty();
        let (word, bool) = (self.b.operand_ty(&size), TypeInterner::BOOL);
        if spread {
            // The elements of another slice, or the bytes of a string,
            // which may be those of the slice itself.
            let [from] = more else {
                return Ok(Rvalue::Use(Operand::Copy(own)));
            };
            let from = self.header(*from, span)?;
            // Where the elements that come start, and how many come: for
            // elements whose copy is more than their bytes.
            // What is appended may be of a type that is not known,
            // which was said: nothing of it is copied then.
            let known = matches!(self.kind(self.b.place_ty(&from)), Type::Slice(_));
            let lens = match self.flat(of) || !known {
                true => None,
                false => {
                    let before = self.b.assign_temp(Rvalue::Len(own.clone()))?;
                    Some((before, self.b.assign_temp(Rvalue::Len(from.clone()))?))
                }
            };
            let args = vec![self.record(own.clone())?, self.record(from)?, size, plain];
            let params = [bytes, bytes, word, bool];
            self.runtime_call("_gopp_slice_append", &params, args)?;
            if let Some((before, added)) = lens {
                let new = self.b.index(&own, before)?;
                let checked = self.checked_as(first, ty);
                let checked = checked.and_then(|slice| self.element_of(slice));
                self.clone_each(span, new, slot(added), of, checked)?;
            }
            return Ok(Rvalue::Use(Operand::Copy(own)));
        }
        let wanted = vec![Some(of); more.len()];
        let mut values = Vec::with_capacity(more.len());
        for value in self.owned_values(more, &wanted)? {
            values.push(self.operand_of(value)?);
        }
        let before = self.b.assign_temp(Rvalue::Len(own.clone()))?;
        let count = self.b.const_int(word, more.len() as u64)?;
        let args = vec![self.record(own.clone())?, count.clone(), size, plain];
        self.runtime_call("_gopp_slice_grow", &[bytes, word, word, bool], args)?;
        for (at, value) in values.into_iter().enumerate() {
            let offset = self.b.const_int(word, at as u64)?;
            let index = Rvalue::Binary(BinOp::Add, slot(before), offset);
            let index = self.b.assign_temp(index)?;
            let element = self.b.index(&own, index)?;
            self.b.assign(element, Rvalue::Use(value))?;
        }
        let len = self.b.field(&own, 1)?;
        self.b
            .assign(len, Rvalue::Binary(BinOp::Add, slot(before), count))?;
        Ok(Rvalue::Use(Operand::Copy(own)))
    }

    /// `copy(&to, from)`, and `copy(to, from)` of a Go file: as many
    /// elements as both have are written to those of `to` itself.
    pub fn copy_into(&mut self, span: SourceSpan, ty: TypeId, args: &[ExprId]) -> Built<Rvalue> {
        let [args_to, from] = args else {
            return Ok(Rvalue::Use(self.zero(ty)));
        };
        let args_to = *args_to;
        let to = self.header(args_to, span)?;
        let Type::Slice(of) = self.kind(self.b.place_ty(&to)) else {
            return Ok(Rvalue::Use(self.zero(ty)));
        };
        let from = self.header(*from, span)?;
        let [size, _] = self.element_facts(of)?;
        let bytes = self.bytes_ty();
        let word = self.b.operand_ty(&size);
        let copied = match self.flat(of) {
            true => {
                let args = vec![self.record(to)?, self.record(from)?, size];
                self.runtime("_gopp_slice_copy_into", &[bytes, bytes, word], word, args)?
            }
            // Elements that hold what is freed: a slice of its own with
            // copies of the elements is moved over those of `to`, which
            // end first.
            false => {
                let slice = self.b.place_ty(&to);
                let checked = self.checked_as(args_to, slice);
                let source = self.cloned_value(span, checked, Rvalue::Use(Operand::Copy(from)))?;
                let source = self.slot_of(source)?;
                let elem = checked.and_then(|slice| self.element_of(slice));
                match self.drop_glue(elem, span)? {
                    None => self.zero(word),
                    Some(drop) => {
                        let params = [bytes, bytes, word, self.glue_sig(Glue::Drop)];
                        let drop = self.glue_value(Glue::Drop, drop);
                        let args = vec![self.record(to)?, self.record(source)?, size, drop];
                        self.runtime("_gopp_slice_move_into", &params, word, args)?
                    }
                }
            }
        };
        Ok(match self.kind(ty) {
            Type::Int { .. } if ty != word => Rvalue::Cast(CastKind::IntToInt, copied, ty),
            _ => Rvalue::Use(copied),
        })
    }

    /// `clear(s)`: every element is zero again.
    pub fn clear_slice(&mut self, span: SourceSpan, x: ExprId) -> Built<bool> {
        let of = self.header(x, span)?;
        let Type::Slice(elem) = self.kind(self.b.place_ty(&of)) else {
            return Ok(false);
        };
        let [size, _] = self.element_facts(elem)?;
        // Elements that hold what is freed end before they are zero.
        if !self.flat(elem) {
            let slice = self.b.place_ty(&of);
            let checked = self.checked_as(x, slice);
            let checked = checked.and_then(|slice| self.element_of(slice));
            if let Some(drop) = self.drop_glue(checked, span)? {
                let word = self.b.operand_ty(&size);
                let none = self.b.assign_temp(Rvalue::Use(self.zero(word)))?;
                let first = self.b.index(&of, none)?;
                let count = self.b.assign_temp(Rvalue::Len(of.clone()))?;
                self.each(first, slot(count), elem, drop)?;
            }
        }
        let params = [self.bytes_ty(), self.b.operand_ty(&size)];
        let args = vec![self.record(of)?, size];
        self.runtime_call("_gopp_slice_clear", &params, args)?;
        Ok(true)
    }

    /// `cap(x)` as a number of the type.
    pub fn cap(&mut self, span: SourceSpan, ty: TypeId, x: ExprId) -> Built<Option<Rvalue>> {
        let mut mode = PlaceMode::default();
        let place = self.place(x, &mut mode)?;
        let place = self.through(place, span, &mut mode)?;
        let word = super::Memo::word(self.shared, self.b.types(), false);
        let cap = match self.kind(self.b.place_ty(&place)) {
            Type::Slice(_) => Operand::Copy(self.b.field(&place, 2)?),
            Type::Array(_, len) => self.b.const_int(word, len)?,
            _ => return Ok(None),
        };
        Ok(Some(match ty == word {
            true => Rvalue::Use(cap),
            false => Rvalue::Cast(CastKind::IntToInt, cap, ty),
        }))
    }

    /// Whether a slice is nil, or is not: `op` is `Eq` or `Ne`.
    pub fn nil_slice(&mut self, op: BinOp, first: Operand, second: Operand) -> Built<Rvalue> {
        let first = self.slot_of(Rvalue::Use(first))?;
        let second = self.slot_of(Rvalue::Use(second))?;
        let (first, second) = (self.b.field(&first, 0)?, self.b.field(&second, 0)?);
        Ok(Rvalue::Binary(
            op,
            Operand::Copy(first),
            Operand::Copy(second),
        ))
    }

    /// `[]byte(text)` and `[]rune(text)`: the function `name` of the
    /// runtime fills the slice.
    pub fn slice_of_text(&mut self, name: &str, ty: TypeId, text: Operand) -> Built<Rvalue> {
        let out = self.slot_of(Rvalue::Use(self.zero(ty)))?;
        let params = [self.bytes_ty(), self.b.operand_ty(&text)];
        let args = vec![self.record(out.clone())?, text];
        self.runtime_call(name, &params, args)?;
        Ok(Rvalue::Use(Operand::Copy(out)))
    }

    /// `string(bytes)` and `string(runes)`, likewise.
    pub fn text_of_slice(&mut self, name: &str, ty: TypeId, slice: Operand) -> Built<Rvalue> {
        let from = self.slot_of(Rvalue::Use(slice))?;
        let params = [self.bytes_ty()];
        let args = vec![self.record(from)?];
        let text = self.runtime(name, &params, ty, args)?;
        Ok(Rvalue::Use(text))
    }

    /// `x[lo:hi]` and `x[lo:hi:max]` of a slice or an array. The bounds
    /// are checked as Go checks them, and the high one also against the
    /// length where Go takes the capacity (SLC-13): past the length
    /// there are no elements.
    pub fn slice_part(
        &mut self,
        expr: ExprId,
        ty: TypeId,
        x: ExprId,
        bounds: [Option<ExprId>; 3],
        part: Part,
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let mut mode = PlaceMode::default();
        let base = self.place(x, &mut mode)?;
        let base = self.through(base, span, &mut mode)?;
        let word = super::Memo::word(self.shared, self.b.types(), false);
        // The elements, how many there are and how many fit.
        let (whole, of, len, cap) = match self.kind(self.b.place_ty(&base)) {
            Type::Slice(of) => {
                let whole = self.slot_of(Rvalue::Use(Operand::Copy(base)))?;
                let len = self.b.assign_temp(Rvalue::Len(whole.clone()))?;
                let cap = Operand::Copy(self.b.field(&whole, 2)?);
                let cap = self.b.assign_temp(Rvalue::Use(cap))?;
                (whole, of, len, cap)
            }
            Type::Array(of, count) => {
                let count = self.b.const_int(word, count)?;
                let len = self.b.assign_temp(Rvalue::Use(count))?;
                (base, of, len, len)
            }
            _ => return Ok(self.placeholder(span, ty, "a slice of this value")),
        };
        let slice = match self.kind(ty) {
            Type::Ptr(to) => to,
            _ => ty,
        };
        let [lo, hi, max] = bounds;
        let lo = match lo {
            Some(lo) => self.index_slots(lo, span)?,
            None => {
                let none = self.b.assign_temp(Rvalue::Use(self.zero(word)))?;
                (none, none)
            }
        };
        let hi = match hi {
            Some(hi) => self.index_slots(hi, span)?,
            None => (len, len),
        };
        let le = |a: LocalId, b: LocalId| Rvalue::Binary(BinOp::Le, slot(a), slot(b));
        let room = match max {
            Some(max) => {
                let max = self.index_slots(max, span)?;
                let said = vec![slot(max.1), slot(cap)];
                self.assert(span, le(max.0, cap), AssertKind::Slice3Acap, said)?;
                let said = vec![slot(hi.1), slot(max.0)];
                self.assert(span, le(hi.0, max.0), AssertKind::Slice3B, said)?;
                let said = vec![slot(hi.1), slot(len)];
                self.assert(span, le(hi.0, len), AssertKind::SliceAlen, said)?;
                let said = vec![slot(lo.1), slot(hi.0)];
                self.assert(span, le(lo.0, hi.0), AssertKind::Slice3C, said)?;
                max.0
            }
            None => {
                let said = vec![slot(hi.1), slot(len)];
                self.assert(span, le(hi.0, len), AssertKind::SliceAlen, said)?;
                let said = vec![slot(lo.1), slot(hi.0)];
                self.assert(span, le(lo.0, hi.0), AssertKind::SliceB, said)?;
                cap
            }
        };
        // A slice that stays itself loses the elements that it cuts
        // off: they end here (SLC-9).
        if part == Part::InPlace && !self.flat(of) {
            let checked = self.checked_as(expr, slice);
            let checked = checked.and_then(|slice| self.element_of(slice));
            if let Some(drop) = self.drop_glue(checked, span)? {
                let none = self.b.assign_temp(Rvalue::Use(self.zero(word)))?;
                let head = self.b.index(&whole, none)?;
                self.each(head, slot(lo.0), of, drop)?;
                let tail = self.b.index(&whole, hi.0)?;
                let left = Rvalue::Binary(BinOp::Sub, slot(len), slot(hi.0));
                let left = self.b.assign_temp(left)?;
                self.each(tail, slot(left), of, drop)?;
            }
        }
        let first = self.b.index(&whole, lo.0)?;
        let elements = self.b.assign_temp(Rvalue::AddrOf(first, Mutability::Mut))?;
        let count = self
            .b
            .assign_temp(Rvalue::Binary(BinOp::Sub, slot(hi.0), slot(lo.0)))?;
        // A slice that stays itself keeps its buffer and the room after
        // its elements; any other has neither.
        let bytes = self.bytes_ty();
        let null = self.zero(bytes);
        let (room, backing) = match part {
            Part::InPlace => {
                let room = Rvalue::Binary(BinOp::Sub, slot(room), slot(lo.0));
                let backing = Operand::Copy(self.b.field(&whole, 3)?);
                (slot(self.b.assign_temp(room)?), backing)
            }
            _ => (slot(count), null),
        };
        let parts = vec![slot(elements), slot(count), room, backing];
        let record = self.slot_of(Rvalue::Aggregate(AggKind::Of(slice), parts))?;
        match part {
            Part::InPlace => Ok(Rvalue::Use(Operand::Copy(record))),
            Part::View => Ok(Rvalue::AddrOf(record, Mutability::Mut)),
            Part::Value if !self.flat(of) => {
                let checked = self.checked_as(expr, ty);
                self.cloned_value(span, checked, Rvalue::Use(Operand::Copy(record)))
            }
            Part::Value => {
                let [size, _] = self.element_facts(of)?;
                let params = [self.bytes_ty(), self.b.operand_ty(&size)];
                let args = vec![self.record(record.clone())?, size];
                self.runtime_call("_gopp_slice_copy", &params, args)?;
                Ok(Rvalue::Use(Operand::Copy(record)))
            }
        }
    }
}
