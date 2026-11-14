//! Expressions: values, places, calls, and the checks that Go's
//! semantics ask for (gir.md §5).

use gopp_mangle::{Entity, symbol_of};
use gopp_sema::constant::Value;
use gopp_sema::types::{PARENT, Type as CheckedType, TypeId as CheckedTypeId};
use gopp_sema::{LocalKind, ObjKind, Package, Predeclared, PredeclaredKind, Ref, SelectionKind};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{BinaryOp, ExprId, ExprKind, UnaryOp};

use super::func::{Built, FuncCx, SAID, deeper};
use super::slice::Part;
use super::{Callee, CalleeKey, Memo, Shared};
use crate::{
    AggKind, AssertKind, BinOp, CallConv, CastKind, Constant, ConstantKind, FuncType, Global,
    Linkage, LocalId, LocalKind as SlotKind, Mutability, Operand, Place, ProjectionKind, Rvalue,
    Type, TypeId, TypeInterner, UnOp,
};

/// A literal of an array with more elements is written one element
/// after another, over zeros.
const ARRAY_AT_ONCE: u64 = 64;

/// The receiver of a method that is called, and the way from it to the
/// value that has the method.
pub(super) type Receiver<'a> = (ExprId, &'a [u32]);

/// The code that a call runs.
pub(super) enum Code {
    /// A function that is known where the call is written.
    Known(Operand),
    /// A method of the interface value in the place, found in its table
    /// of methods when the arguments are worked out: which method of the
    /// interface, the type of its code, and whether the value may be
    /// nil.
    Slot(Place, u32, TypeId, bool),
}

/// What a call calls, and what it takes and gives.
pub(super) struct Called {
    pub code: Code,
    pub params: std::rc::Rc<[TypeId]>,
    /// How many of the last parameters are places for views.
    pub views: usize,
    /// Which of the parameters are lent their arguments, and which of
    /// those a value that nothing else reads.
    pub lent: std::rc::Rc<[bool]>,
    pub worked: std::rc::Rc<[bool]>,
    pub ret: TypeId,
    /// For a function with `...`: the type of its last parameter, a
    /// slice, as the checker has it.
    pub rest: Option<CheckedTypeId>,
    /// The function is known, and calls `recover` itself.
    pub recovers: bool,
}

/// The type of the last parameter of a function type with `...`, which
/// is a slice.
pub(super) fn rest_of(shared: &Shared<'_>, sig: CheckedTypeId) -> Option<CheckedTypeId> {
    match &*shared.types.get(shared.types.underlying(sig)) {
        CheckedType::Func {
            variadic: true,
            params,
            ..
        } => params.last().copied(),
        _ => None,
    }
}

/// Where the elements of the slice of a call of a function with `...`
/// come from.
pub(super) enum Rest<'e> {
    /// Each is an argument of the call.
    Args(&'e [ExprId]),
    /// One call gives all the arguments: they are its results from
    /// this one on.
    Results(usize),
}

/// What a call calls, with the arguments that come before those that
/// are written; or why the call is not supported yet.
pub(super) type Target = Result<(Called, Vec<Operand>), &'static str>;

/// A check that a place needs before it is used.
pub(super) enum Check {
    /// The index is below the length. The second slot has the index
    /// with the sign that the program gave it, for the message.
    Bounds(LocalId, LocalId, Length, SourceSpan),
    /// The pointer is not nil.
    Nil(Operand, SourceSpan),
}

/// How many elements there are: a number that is known, or the slot
/// that holds the length of a string.
#[derive(Clone, Copy)]
pub(super) enum Length {
    Known(u64),
    Of(LocalId),
}

/// How a place is worked out.
#[derive(Default)]
pub(super) struct PlaceMode {
    /// The checks of the place, if they are made later: where the
    /// place is assigned to, after the values are evaluated.
    pub later: Option<Vec<Check>>,
    /// Every pointer on the way is copied first: what is evaluated
    /// after the place may change the variable that holds it.
    pub held: bool,
}

impl<'a> FuncCx<'a, '_> {
    /// Whether what the expression names cannot change while a call
    /// runs: a variable of the function that nothing else reaches, or a
    /// part of one that is reached through no pointer. Only the
    /// function can get to such a value, and it is in the call.
    pub fn quiet(&self, expr: ExprId) -> bool {
        let types = self.shared.types;
        let mut at = Shared::unparen(self.tree, expr);
        loop {
            let base = match &self.tree.exprs[at].kind {
                ExprKind::Name(_) => {
                    return match self.denoted(at) {
                        Some(Ref::Local(local)) => self.quiet_var(*local),
                        _ => false,
                    };
                }
                ExprKind::Selector { x, .. } => {
                    let field = self.body.selections.get(&at).is_some_and(|selection| {
                        selection.kind == SelectionKind::Field && selection.path.len() == 1
                    });
                    if !field {
                        return false;
                    }
                    *x
                }
                ExprKind::Index { x, indices } if indices.len() == 1 => *x,
                _ => return false,
            };
            // A part behind a pointer is a part of what something else
            // may hold too.
            let held = self
                .body
                .types
                .get(&base)
                .map(|typed| types.underlying(typed.ty));
            let inside = held.is_some_and(|held| {
                matches!(
                    &*types.get(held),
                    CheckedType::Struct(_)
                        | CheckedType::Array { .. }
                        | CheckedType::Slice(_)
                        | CheckedType::Map { .. }
                )
            });
            if !inside {
                return false;
            }
            at = Shared::unparen(self.tree, base);
        }
    }

    /// What a name denotes. The key of a literal that is a name is
    /// kept apart by the checker: only the type of the literal says
    /// that it is no field.
    pub(super) fn denoted(&self, name: ExprId) -> Option<&'a Ref> {
        let body = self.body;
        body.uses.get(&name).or_else(|| body.keys.get(&name))
    }

    /// The object of an imported package that `pkg.Name` names.
    fn member(
        &self,
        import: gopp_sema::ObjId,
        member: gopp_sema::ObjId,
    ) -> Option<(&'a Package, &'a gopp_sema::Object)> {
        let ObjKind::Import { package, .. } = &self.home().object(import).kind else {
            return None;
        };
        let object = package.objects.get(member.0 as usize)?;
        Some((package, object))
    }
}

impl<'a> FuncCx<'a, '_> {
    /// The package of the function that is lowered: what its body names
    /// is there. It is another than the package of the module for an
    /// instance of generic code of a package that is imported.
    pub fn home(&self) -> &'a Package {
        self.shared.home(self.file)
    }

    // ---- types and constants

    /// The type of the value of the expression; the empty type for
    /// what has none, and for `nil`, which has that of its place.
    pub fn ty_of(&mut self, expr: ExprId) -> TypeId {
        match self.body.types.get(&expr) {
            Some(typed) => self.lower(typed.ty, self.tree.exprs[expr].span),
            None => TypeInterner::UNIT,
        }
    }

    pub(super) fn constant(&mut self, value: &Value, ty: TypeId) -> Option<Operand> {
        match (value, self.kind(ty)) {
            (Value::Bool(known), Type::Bool) => Some(self.b.const_bool(*known)),
            (_, Type::Int { .. }) => {
                let bits = value.to_int().low_bits()?;
                self.b.const_int(ty, bits).ok()
            }
            (
                Value::Int64(_) | Value::Int(_) | Value::Rat(_) | Value::Float(_),
                Type::Float { bits },
            ) => {
                let bits = match bits {
                    32 => u64::from((value.to_f32() as f32).to_bits()),
                    _ => value.to_f64().to_bits(),
                };
                let kind = ConstantKind::Float(bits);
                Some(Operand::Const(Constant { kind, ty }))
            }
            (Value::Str(text), Type::String) => Some(self.b.const_str(text.bytes())),
            (Value::Composite(of), _) => {
                // Every use of a constant shares what the checker
                // found for it.
                let key = (std::sync::Arc::as_ptr(of) as usize, ty);
                let global = match self.memo.constants.get(&key) {
                    Some(&known) => known,
                    None => {
                        let made = self.composite_constant(value, ty);
                        self.memo.constants.insert(key, made);
                        made
                    }
                }?;
                let kind = ConstantKind::Value(global);
                Some(Operand::Const(Constant { kind, ty }))
            }
            _ => None,
        }
    }

    /// The parts of a constant of the type as static data has them: a
    /// constant for each leaf of the type, in their order. `said` gets
    /// what tells the constant from every other of its type.
    pub(super) fn constant_parts(
        &mut self,
        value: &Value,
        ty: TypeId,
        parts: &mut Vec<Constant>,
        said: &mut Vec<u8>,
    ) -> Option<()> {
        use gopp_sema::constant::Composite;
        self.memo.steps += 1;
        let zero = |this: &mut Self, parts: &mut Vec<Constant>| {
            let leaves = this.b.types().leaves(ty, crate::verify::MAX_LEAVES)?;
            let kind = ConstantKind::Zero;
            parts.extend(leaves.into_iter().map(|ty| Constant { kind, ty }));
            Some(())
        };
        match (value, self.kind(ty)) {
            (Value::Composite(of), Type::Struct(id)) => {
                let Composite::Struct(values) = &**of else {
                    return None;
                };
                let fields = self.b.types().struct_fields(id)?.to_vec();
                if fields.len() != values.len() || self.holds_tables(ty) {
                    return None;
                }
                for (value, field) in values.iter().zip(fields) {
                    self.constant_parts(value, field, parts, said)?;
                }
            }
            (Value::Composite(of), Type::Array(elem, count)) => {
                let Composite::Array(values) = &**of else {
                    return None;
                };
                if values.len() as u64 != count {
                    return None;
                }
                for value in values {
                    self.constant_parts(value, elem, parts, said)?;
                }
            }
            // An option that is nil is all zero; one that is set is its
            // value, and then that it is set.
            (Value::Composite(of), Type::Option(_)) if matches!(**of, Composite::Nil) => {
                said.push(b'n');
                zero(self, parts)?;
            }
            (value, Type::Option(of)) => {
                self.constant_parts(value, of, parts, said)?;
                said.push(b's');
                parts.push(Constant {
                    kind: ConstantKind::Bool(true),
                    ty: TypeInterner::BOOL,
                });
            }
            (Value::Composite(_), _) => return None,
            (value, _) => {
                let Operand::Const(part) = self.constant(value, ty)? else {
                    return None;
                };
                match (part.kind, value) {
                    (ConstantKind::Int(bits) | ConstantKind::Float(bits), _) => {
                        said.extend(bits.to_le_bytes());
                    }
                    (ConstantKind::Bool(known), _) => said.push(u8::from(known)),
                    (ConstantKind::Str(_), Value::Str(text)) => {
                        said.extend((text.bytes().len() as u64).to_le_bytes());
                        said.extend(text.bytes());
                    }
                    _ => return None,
                }
                parts.push(part);
            }
        }
        Some(())
    }

    /// The read-only global that holds the constant of several parts:
    /// one for all who use a constant of this type with these parts,
    /// in every package.
    fn composite_constant(&mut self, value: &Value, ty: TypeId) -> Option<crate::GlobalId> {
        use std::hash::{Hash, Hasher};
        let (mut parts, mut said) = (Vec::new(), Vec::new());
        self.constant_parts(value, ty, &mut parts, &mut said)?;
        // The symbol says the type as the module writes it, and the
        // parts by two sums over them.
        let shown = self.b.types().display(ty).to_string();
        let mut sums = (
            std::collections::hash_map::DefaultHasher::new(),
            rustc_hash::FxHasher::default(),
        );
        (&shown, &said).hash(&mut sums.0);
        (&said, &shown).hash(&mut sums.1);
        let mut symbol = "gopp.const.".to_owned();
        for letter in shown.bytes() {
            match letter.is_ascii_alphanumeric() || letter == b'_' || letter == b'.' {
                true => symbol.push(letter as char),
                false => symbol.push_str(&format!("${letter:02x}")),
            }
        }
        symbol.push_str(&format!(
            ".{:016x}{:016x}",
            sums.0.finish(),
            sums.1.finish()
        ));
        if let Some(&known) = self.memo.globals.get(&symbol) {
            return Some(known);
        }
        Some(self.said(symbol, ty, parts))
    }

    /// `nil` in a place of the type.
    fn nil(&mut self, ty: TypeId) -> Operand {
        let kind = match self.kind(ty) {
            Type::Ptr(_) | Type::Func(_) => ConstantKind::Null,
            _ => ConstantKind::Zero,
        };
        Operand::Const(Constant { kind, ty })
    }

    /// A zero in the place of what is not supported, which is reported.
    pub fn placeholder(&mut self, span: SourceSpan, ty: TypeId, what: &str) -> Rvalue {
        self.unsupported(span, what);
        Rvalue::Use(self.zero(ty))
    }

    // ---- values

    pub fn rvalue(&mut self, expr: ExprId) -> Built<Rvalue> {
        let ty = self.ty_of(expr);
        self.rvalue_as(expr, ty)
    }

    /// The value of the expression, for a place of the type `ty`.
    pub fn rvalue_as(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        if self.is_c_code(ty) {
            return self.callback(expr, ty);
        }
        if let Some(of) = self.held_by(expr, ty) {
            return deeper(|| self.set_option(expr, ty, of));
        }
        if let Some(&(from, to)) = self.body.iface_conversions.get(&expr) {
            return match self.unboxed.get(&expr).copied() {
                Some(worked) => deeper(|| self.lent_iface(expr, ty, (from, to), worked)),
                None => deeper(|| self.in_iface(expr, ty, from, to)),
            };
        }
        match self.body.iface_changes.get(&expr) {
            Some(&(_, to)) => deeper(|| self.iface_change(expr, ty, to)),
            None => self.rvalue_plain(expr, ty),
        }
    }

    /// The value of the expression as it is, without what puts it into
    /// an interface.
    pub fn rvalue_plain(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let value = deeper(|| self.rvalue_here(expr, ty))?;
        // A value of another type than its place: the checker did not
        // know the type of one of them. What has no type at all was
        // reported where it was made.
        let unit = TypeInterner::UNIT;
        let own = match &value {
            Rvalue::Aggregate(..) => return Ok(value),
            other => self.b.rvalue_ty(other).unwrap_or(unit),
        };
        if own == ty || (ty == unit && self.b.rvalue_ty(&value).is_ok()) {
            return self.given(expr, ty, value);
        }
        // A `Unique` or a `Shared` in a place where a pointer fits is
        // the address of its value, and stays what it is (OWN-P6).
        let owned = self.memo.held(own);
        let lent = match (owned, self.kind(ty)) {
            (Some(of), Type::Ptr(to)) => to == of,
            _ => false,
        };
        if lent {
            let handle = self.operand_of(value)?;
            return Ok(Rvalue::Cast(CastKind::PtrToPtr, handle, ty));
        }
        match self.handle_upcast(expr, ty, &value)? {
            Some(Ok(made)) => return Ok(made),
            Some(Err(what)) => {
                let span = self.tree.exprs[expr].span;
                return Ok(self.placeholder(span, ty, what));
            }
            None => {}
        }
        // A pointer to a value where one to a part of a parent of it
        // fits is the address of that part (EXP-13).
        if let (Type::Ptr(_), Type::Ptr(_)) = (self.kind(own), self.kind(ty)) {
            // A handle is lent as the address of its value first.
            let mut handle = self.operand_of(value)?;
            if let Some(held) = self.memo.held(own) {
                let to = self.b.types().ptr(held);
                let lent = Rvalue::Cast(CastKind::PtrToPtr, handle, to);
                handle = Operand::Copy(Place::local(self.b.assign_temp(lent)?));
            }
            match self.upcast(handle.clone(), ty)? {
                Some(Ok(part)) => return Ok(part),
                Some(Err(what)) => {
                    let span = self.tree.exprs[expr].span;
                    return Ok(self.placeholder(span, ty, what));
                }
                None => {
                    let span = self.tree.exprs[expr].span;
                    self.unsupported(span, "a value whose type is not known");
                    return Ok(Rvalue::Use(self.zero(ty)));
                }
            }
        }
        if own != unit && ty != unit {
            let span = self.tree.exprs[expr].span;
            self.unsupported(span, "a value whose type is not known");
        }
        Ok(Rvalue::Use(self.zero(ty)))
    }

    pub fn operand(&mut self, expr: ExprId) -> Built<Operand> {
        let ty = self.ty_of(expr);
        self.operand_as(expr, ty)
    }

    pub fn operand_as(&mut self, expr: ExprId, ty: TypeId) -> Built<Operand> {
        let value = self.rvalue_as(expr, ty)?;
        self.operand_of(value)
    }

    /// The value in a slot, unless it is one already.
    pub fn operand_of(&mut self, value: Rvalue) -> Built<Operand> {
        match value {
            Rvalue::Use(operand) => Ok(operand),
            other => Ok(Operand::Copy(Place::local(self.b.assign_temp(other)?))),
        }
    }

    /// The operand as nothing that is evaluated later can change it: a
    /// constant, or a slot of its own.
    pub fn stable(&mut self, operand: Operand) -> Built<Operand> {
        let own = |this: &Self, place: &Place| {
            this.b.local_kind(place.local) == SlotKind::Temp
                && place
                    .projections
                    .iter()
                    .all(|step| step.kind != ProjectionKind::Deref)
        };
        match &operand {
            Operand::Const(_) => Ok(operand),
            Operand::Copy(place) | Operand::Move(place) if own(self, place) => Ok(operand),
            _ => {
                let held = self.b.assign_temp(Rvalue::Use(operand))?;
                Ok(Operand::Copy(Place::local(held)))
            }
        }
    }

    /// The values of the expressions for places of the types `wanted`,
    /// evaluated from left to right. One call may give them all.
    pub fn values(&mut self, exprs: &[ExprId], wanted: &[Option<TypeId>]) -> Built<Vec<Rvalue>> {
        let ty = |this: &mut Self, at: usize| match wanted.get(at).copied().flatten() {
            Some(ty) => ty,
            None => this.ty_of(exprs[at]),
        };
        if let ([one], [_]) = (exprs, wanted) {
            let ty = ty(self, 0);
            return Ok(vec![self.rvalue_as(*one, ty)?]);
        }
        if exprs.len() == wanted.len() {
            let values = self.values_each(exprs, wanted)?;
            return Ok(values.into_iter().map(Rvalue::Use).collect());
        }
        let [call] = exprs else {
            return Ok(Vec::new());
        };
        // `v, ok := x.(T)`.
        if let (true, [value, _]) = (self.is_assertion(*call), wanted) {
            let found = self.asserted_ok(*call)?;
            let span = self.tree.exprs[*call].span;
            return Ok(match (found, *value) {
                (Some([found, ok]), None) => vec![Rvalue::Use(found), Rvalue::Use(ok)],
                (Some([found, ok]), Some(ty)) if self.b.operand_ty(&found) == ty => {
                    vec![Rvalue::Use(found), Rvalue::Use(ok)]
                }
                (Some([_, ok]), Some(ty)) => {
                    let value = self.placeholder(span, ty, "a value whose type is not known");
                    vec![value, Rvalue::Use(ok)]
                }
                (None, _) => {
                    let zero = |ty: &Option<TypeId>| {
                        Rvalue::Use(self.zero(ty.unwrap_or(TypeInterner::UNIT)))
                    };
                    wanted.iter().map(zero).collect()
                }
            });
        }
        // `v, ok := m[k]`.
        if let (Some((map, index)), [_, _]) = (self.map_index(*call), wanted) {
            let span = self.tree.exprs[*call].span;
            let read = self.map_read(map, index)?;
            let mut values = Vec::with_capacity(2);
            for (at, &wanted) in wanted.iter().enumerate() {
                let part = read.as_ref().map(|read| read[at].clone());
                values.push(match (part, wanted) {
                    (Some(part), None) => Rvalue::Use(part),
                    // The value is the new owner's: a copy, if the
                    // checker says so.
                    (Some(part), Some(ty)) if self.b.operand_ty(&part) == ty && at == 0 => {
                        self.given(*call, ty, Rvalue::Use(part))?
                    }
                    (Some(part), Some(ty)) if self.b.operand_ty(&part) == ty => Rvalue::Use(part),
                    (Some(_), Some(ty)) => {
                        self.placeholder(span, ty, "a value whose type is not known")
                    }
                    (None, ty) => Rvalue::Use(self.zero(ty.unwrap_or(TypeInterner::UNIT))),
                });
            }
            return Ok(values);
        }
        let whole = self.operand(*call)?;
        let whole = self.stable(whole)?;
        // Several values at once are the results of a call; a value of
        // two parts, as a func is, is one value.
        let inner = Shared::unparen(self.tree, *call);
        let several = matches!(self.tree.exprs[inner].kind, ExprKind::Call { .. });
        let parts = match &whole {
            Operand::Copy(place) if several => match self.kind(self.b.place_ty(place)) {
                Type::Tuple(parts) if parts.len() == wanted.len() => Some(place.clone()),
                _ => None,
            },
            _ => None,
        };
        let mut values = Vec::with_capacity(wanted.len());
        for (at, wanted) in wanted.iter().enumerate() {
            values.push(match &parts {
                Some(place) => {
                    let part = Rvalue::Use(Operand::Copy(self.b.field(place, at as u32)?));
                    // A result that its place takes as an interface
                    // value, or as one of another interface type.
                    let converts = self.body.part_conversions.get(&(*call, at as u32));
                    match (converts, *wanted) {
                        (Some(&(from, to)), Some(ty)) => {
                            let span = self.tree.exprs[*call].span;
                            match self.shared.types.is_interface(from) {
                                true => self.changed(span, ty, to, part)?,
                                false => self.boxed(span, ty, (from, to), part)?,
                            }
                        }
                        _ => part,
                    }
                }
                None => {
                    let span = self.tree.exprs[*call].span;
                    let ty = wanted.unwrap_or(TypeInterner::UNIT);
                    self.placeholder(span, ty, "an expression with a second value")
                }
            });
        }
        Ok(values)
    }

    fn rvalue_here(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        self.memo.steps += 1;
        let node = &self.tree.exprs[expr];
        let span = node.span;
        if self.body.operators.contains_key(&expr) {
            return self.operator(expr, ty);
        }
        if let Some(typed) = self.body.types.get(&expr)
            && let Some(known) = self.constant(&typed.value, ty)
        {
            return Ok(Rvalue::Use(known));
        }
        // A pointer where only what it points to fits is read as that.
        let own = self.ty_of(expr);
        if own != ty && self.pointee(own) == ty {
            let pointer = self.operand_as(expr, own)?;
            let pointer = match pointer {
                Operand::Copy(place) | Operand::Move(place) => place,
                constant => Place::local(self.b.assign_temp(Rvalue::Use(constant))?),
            };
            let place = self.deref(pointer, &mut PlaceMode::default())?;
            return Ok(Rvalue::Use(Operand::Copy(place)));
        }
        match &node.kind {
            ExprKind::Paren(inner) => self.rvalue_as(*inner, ty),
            ExprKind::Name(_) => self.name(expr, ty),
            ExprKind::Selector { .. } => self.selector(expr, ty),
            ExprKind::Index { .. } => {
                // `f[int]`: an instance of a generic function, or of a
                // method with type parameters of its own (`x.m[int]`).
                if let Some(key) = self.instance_named(expr) {
                    let named = self.shared.generic_named(self.file, self.body, expr);
                    if matches!(named, Some((_, Some(_)))) {
                        let made = self.method_value(expr, ty, false)?;
                        return self.fresh(expr, ty, made);
                    }
                    return self.func_value(expr, ty, key);
                }
                if let Some((map, index)) = self.map_index(expr) {
                    return Ok(Rvalue::Use(match self.map_read(map, index)? {
                        Some([value, _]) => value,
                        None => self.zero(ty),
                    }));
                }
                let place = self.place(expr, &mut PlaceMode::default())?;
                Ok(Rvalue::Use(Operand::Copy(place)))
            }
            ExprKind::CompositeLit { elems, .. } => self.composite(expr, ty, elems),
            ExprKind::Unary { op, x } => self.unary(expr, ty, *op, *x),
            ExprKind::Binary { op, x, y } => match op {
                BinaryOp::AndAnd | BinaryOp::OrOr => self.logical(expr),
                _ => self.binary(expr, *op, *x, *y),
            },
            ExprKind::Call { .. } => self.call(expr, ty),
            // A number where no type is known: what is around it was
            // reported.
            ExprKind::BasicLit { .. } if ty == TypeInterner::UNIT => Ok(Rvalue::Use(self.zero(ty))),
            ExprKind::BasicLit { .. } => Ok(self.placeholder(span, ty, "a complex number")),
            ExprKind::FuncLit { .. } => {
                let made = self.literal(expr, ty)?;
                self.fresh(expr, ty, made)
            }
            ExprKind::Slice { x, lo, hi, max, .. } => {
                let of = self.ty_of(*x);
                let of = self.pointee(of);
                let bounds = [*lo, *hi, *max];
                match self.kind(of) {
                    Type::Slice(_) | Type::Array(..) => {
                        let same = self.reused == Some(Shared::unparen(self.tree, *x));
                        let part = match same {
                            true => Part::InPlace,
                            false => Part::Value,
                        };
                        let made = self.slice_part(expr, ty, *x, bounds, part)?;
                        self.fresh(expr, ty, made)
                    }
                    // A part of a string shares its bytes, which are
                    // counted once more.
                    _ => {
                        let part = self.substring(expr, ty, *x, bounds)?;
                        let part = self.slot_of(part)?;
                        if let Type::String = self.kind(ty) {
                            self.counted(part.clone(), true)?;
                        }
                        self.fresh(expr, ty, Rvalue::Use(Operand::Copy(part)))
                    }
                }
            }
            ExprKind::Assert { .. } => self.asserted(expr, ty),
            ExprKind::MacroCall { .. } => Ok(self.placeholder(span, ty, "a macro call")),
            _ => Ok(self.placeholder(span, ty, "this expression")),
        }
    }

    fn name(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let what = match self.denoted(expr) {
            Some(Ref::Local(local)) => match self.body.locals[local.0 as usize].kind {
                LocalKind::Var | LocalKind::Param | LocalKind::Result | LocalKind::Receiver => {
                    let place = self.place(expr, &mut PlaceMode::default())?;
                    return Ok(Rvalue::Use(Operand::Copy(place)));
                }
                LocalKind::Const => "a constant of several parts",
                _ => "this name",
            },
            Some(Ref::Object(object)) => match self.home().object(*object).kind {
                ObjKind::Var => {
                    let place = self.place(expr, &mut PlaceMode::default())?;
                    return Ok(Rvalue::Use(Operand::Copy(place)));
                }
                ObjKind::Func => {
                    let key = self.instance_named(expr);
                    return self.func_value(expr, ty, key.unwrap_or(CalleeKey::Object(*object)));
                }
                ObjKind::Const => "a constant of several parts",
                _ => "this name",
            },
            Some(Ref::Universe(Predeclared::Nil)) => return Ok(Rvalue::Use(self.nil(ty))),
            _ => "this name",
        };
        Ok(self.placeholder(span, ty, what))
    }

    fn selector(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        if let Some(pointer) = self.whole_cloned(expr) {
            let made = self.whole_clone_value(span, ty, pointer)?;
            return self.fresh(expr, ty, made);
        }
        let what = match (self.body.selections.get(&expr), self.body.uses.get(&expr)) {
            (Some(selection), _) => match selection.kind {
                SelectionKind::Field | SelectionKind::BaseField => {
                    let place = self.place(expr, &mut PlaceMode::default())?;
                    return Ok(Rvalue::Use(Operand::Copy(place)));
                }
                SelectionKind::Method | SelectionKind::BaseMethod => {
                    let base = selection.kind == SelectionKind::BaseMethod;
                    let made = self.method_value(expr, ty, base)?;
                    return self.fresh(expr, ty, made);
                }
                SelectionKind::MethodExpr => return self.method_expr(expr, ty),
            },
            (None, Some(Ref::Member { import, member })) => {
                match self
                    .member(*import, *member)
                    .map(|(_, object)| &object.kind)
                {
                    Some(ObjKind::Var) => {
                        let place = self.place(expr, &mut PlaceMode::default())?;
                        return Ok(Rvalue::Use(Operand::Copy(place)));
                    }
                    Some(ObjKind::Func) => {
                        let key = self.instance_named(expr);
                        let key = key.unwrap_or(CalleeKey::Member(*import, *member));
                        return self.func_value(expr, ty, key);
                    }
                    _ => "a name of another package",
                }
            }
            _ => "this selector",
        };
        Ok(self.placeholder(span, ty, what))
    }

    // ---- places

    /// The place that the expression names; a slot of its own for a
    /// value that has none.
    pub fn place(&mut self, expr: ExprId, mode: &mut PlaceMode) -> Built<Place> {
        deeper(|| self.place_here(expr, mode))
    }

    fn place_here(&mut self, expr: ExprId, mode: &mut PlaceMode) -> Built<Place> {
        self.memo.steps += 1;
        let node = &self.tree.exprs[expr];
        let span = node.span;
        match &node.kind {
            ExprKind::Paren(inner) => return self.place(*inner, mode),
            // `base` is the receiver: the selector says which parent.
            ExprKind::Name(_) if self.based(expr).is_some() => {
                let receiver = self.based(expr).map(|local| self.var(local));
                if let Some(receiver) = receiver {
                    let receiver = receiver?;
                    let place = Place::local(receiver);
                    // In a function literal the receiver is in a cell.
                    return match self.aliases.contains(&receiver) {
                        true => self.b.deref(&place),
                        false => Ok(place),
                    };
                }
            }
            ExprKind::Index { x, .. } if self.based(*x).is_some() => {
                return self.place(*x, mode);
            }
            ExprKind::Name(_) => match self.denoted(expr) {
                // A constant has no place: it is a value.
                Some(Ref::Local(local))
                    if !matches!(self.body.locals[local.0 as usize].kind, LocalKind::Const) =>
                {
                    let var = self.var(*local)?;
                    let mut place = Place::local(var);
                    // Another name of an element is what its pointer
                    // leads to.
                    if self.aliases.contains(&var) {
                        place = self.b.deref(&place)?;
                    }
                    // An option that is known to be set is read as what
                    // it holds.
                    let holds = matches!(self.kind(self.b.place_ty(&place)), Type::Option(_));
                    return match self.body.narrowed.contains(&expr) && holds {
                        true => self.b.field(&place, 0),
                        false => Ok(place),
                    };
                }
                Some(Ref::Object(object)) => {
                    let package = self.home();
                    if let ObjKind::Var = package.object(*object).kind {
                        let key = CalleeKey::Object(*object);
                        return self.global(key, package, *object, span);
                    }
                }
                _ => {}
            },
            ExprKind::Selector { x, .. } => {
                if let Some(selection) = self.body.selections.get(&expr)
                    && let SelectionKind::Field | SelectionKind::BaseField = selection.kind
                {
                    let base = self.place(*x, mode)?;
                    return self.steps(base, &selection.path, span, mode);
                }
                if let Some(Ref::Member { import, member }) = self.body.uses.get(&expr)
                    && let Some((package, object)) = self.member(*import, *member)
                    && let ObjKind::Var = object.kind
                {
                    let key = CalleeKey::Member(*import, *member);
                    return self.global(key, package, *member, span);
                }
            }
            // An entry of a map is no place: it is read into a slot.
            ExprKind::Index { x, indices } => {
                if let ([index], false) = (&indices[..], self.indexed_by_method(expr))
                    && !self.holds_map(*x)
                {
                    return self.element(expr, *x, *index, mode);
                }
            }
            ExprKind::Unary {
                op: UnaryOp::Star,
                x,
            } if !self.body.operators.contains_key(&expr) => {
                let pointer = self.operand(*x)?;
                let pointer = match pointer {
                    Operand::Copy(place) | Operand::Move(place) => place,
                    constant => Place::local(self.b.assign_temp(Rvalue::Use(constant))?),
                };
                return self.deref(pointer, mode);
            }
            _ => {}
        }
        let value = self.rvalue(expr)?;
        // A temporary that owns the value gives it to the slot.
        let owner = match &value {
            Rvalue::Use(Operand::Copy(place)) if self.owns_temp(place) => Some(place.clone()),
            _ => None,
        };
        let ty = self.b.rvalue_ty(&value)?;
        let slot = self.b.temp(ty);
        self.b.assign(Place::local(slot), value)?;
        if let Some(owner) = owner {
            self.rehome_temp(&owner, &Place::local(slot));
        }
        Ok(Place::local(slot))
    }

    fn indexed_by_method(&self, expr: ExprId) -> bool {
        self.body.operators.contains_key(&expr) || self.body.index_stores.contains_key(&expr)
    }

    /// The variable of a package as a place: what the pointer to it
    /// leads to. `key` is how the source names it.
    pub(super) fn global(
        &mut self,
        key: CalleeKey,
        package: &Package,
        object: gopp_sema::ObjId,
        span: SourceSpan,
    ) -> Built<Place> {
        let ty = self.lower(package.object(object).ty, span);
        let home = self.shared.package_of(self.file);
        let known = match self.memo.global_keys.get(&(home, key)) {
            Some(&id) => Some(id),
            None => self.declare_global(key, package, object, ty, span),
        };
        let Some(id) = known else {
            // A variable without a value, or of a type that is not
            // supported, which was reported: no global stands for it.
            let zero = Rvalue::Use(self.zero(ty));
            let nothing = self.b.assign_temp(zero)?;
            return Ok(Place::local(nothing));
        };
        let kind = ConstantKind::Global(id);
        let ty = self.b.types().ptr(ty);
        let address = Operand::Const(Constant { kind, ty });
        let pointer = self.b.assign_temp(Rvalue::Use(address))?;
        self.b.deref(&Place::local(pointer))
    }

    /// The global of the variable in the module of the run, declared
    /// there if it is not yet; none for a variable without a value.
    pub(super) fn declare_global(
        &mut self,
        key: CalleeKey,
        package: &Package,
        object: gopp_sema::ObjId,
        ty: TypeId,
        span: SourceSpan,
    ) -> Option<crate::GlobalId> {
        if ty == TypeInterner::UNIT {
            return None;
        }
        let entity = Entity::Object(package, object);
        let Ok(symbol) = symbol_of(&mut self.memo.namer, entity) else {
            self.unsupported(span, "a variable without a symbol");
            return None;
        };
        // Two imports of one package name the same variables.
        let id = match self.memo.globals.get(&symbol) {
            Some(&id) => id,
            None => {
                let global = Global {
                    name: symbol.clone(),
                    ty,
                    link: Linkage::External,
                    mutable: true,
                    parts: Vec::new(),
                    defined: std::ptr::eq(package, self.shared.package),
                };
                let id = self.b.module().globals.alloc(global);
                self.memo.globals.insert(symbol, id);
                id
            }
        };
        let home = self.shared.package_of(self.file);
        self.memo.global_keys.insert((home, key), id);
        Some(id)
    }

    /// What the pointer in `place` leads to.
    pub fn deref(&mut self, place: Place, mode: &mut PlaceMode) -> Built<Place> {
        // What is no pointer has no type that is known, which was
        // reported: it stays what it is.
        let Type::Ptr(_) = self.kind(self.b.place_ty(&place)) else {
            return Ok(place);
        };
        let place = self.held(place, mode)?;
        // A `Unique` or a `Shared` leads to its box, which holds the
        // value first.
        let unique = self.memo.held(self.b.place_ty(&place)).is_some();
        let behind = self.b.deref(&place)?;
        match unique {
            true => self.b.field(&behind, 0),
            false => Ok(behind),
        }
    }

    /// Makes the check now, or keeps it for the assignment.
    fn check(&mut self, check: Check, mode: &mut PlaceMode) -> Built {
        match &mut mode.later {
            Some(later) => {
                later.push(check);
                Ok(())
            }
            None => self.checked(check),
        }
    }

    pub fn checked(&mut self, check: Check) -> Built {
        match check {
            Check::Bounds(index, written, len, span) => {
                let word = Memo::word(self.shared, self.b.types(), false);
                let len = match len {
                    Length::Known(len) => self.b.const_int(word, len)?,
                    Length::Of(len) => Operand::Copy(Place::local(len)),
                };
                let index = Operand::Copy(Place::local(index));
                let written = Operand::Copy(Place::local(written));
                let within = Rvalue::Binary(BinOp::Lt, index, len.clone());
                self.assert(span, within, AssertKind::Bounds, vec![written, len])
            }
            Check::Nil(pointer, span) => {
                let ty = self.b.operand_ty(&pointer);
                let null = self.nil(ty);
                let set = Rvalue::Binary(BinOp::Ne, pointer, null);
                self.assert(span, set, AssertKind::Nil, Vec::new())
            }
        }
    }

    /// Goes on only if the condition holds. The check is placed at
    /// `span`, which a panic names; what follows is placed as before.
    pub fn assert(
        &mut self,
        span: SourceSpan,
        holds: Rvalue,
        kind: AssertKind,
        said: Vec<Operand>,
    ) -> Built {
        let around = self.b.loc();
        self.at(span);
        let holds = Operand::Copy(Place::local(self.b.assign_temp(holds)?));
        let unwind = self.unwind()?;
        let next = self.b.new_block();
        self.b.assert(holds, true, (kind, said), next, unwind)?;
        self.b.switch_to(next);
        self.b.set_loc(around);
        Ok(())
    }

    /// The fields on the way of a selector, through the pointers that
    /// lie on it.
    pub(super) fn steps(
        &mut self,
        mut place: Place,
        path: &[u32],
        span: SourceSpan,
        mode: &mut PlaceMode,
    ) -> Built<Place> {
        let _ = span;
        for &step in path {
            place = self.through(place, mode)?;
            // A value of a type that is not supported has no parts: the
            // type was reported.
            if self.b.place_ty(&place) == TypeInterner::UNIT {
                return Ok(place);
            }
            place = self.part(&place, step)?;
        }
        Ok(place)
    }

    /// The part of the struct at the place that a step of the checker
    /// names: a field of the type, which comes after the parts of its
    /// parents, or the part of a parent (TYP-22).
    pub(super) fn part(&mut self, of: &Place, step: u32) -> Built<Place> {
        let ty = self.b.place_ty(of);
        let lead = self.memo.leads.get(&ty).copied();
        let table = u32::from(self.memo.tabled.contains(&ty));
        match step & PARENT != 0 {
            true => self.b.field(of, table + (step & !PARENT)),
            false => self.b.field(of, lead.unwrap_or(0) + step),
        }
    }

    /// The pointer in a slot of its own, if what is evaluated before
    /// it is followed may change the place that it is in.
    fn held(&mut self, place: Place, mode: &PlaceMode) -> Built<Place> {
        let own = place.projections.is_empty() && self.b.local_kind(place.local) == SlotKind::Temp;
        match mode.held && !own {
            true => Ok(Place::local(
                self.b.assign_temp(Rvalue::Use(Operand::Copy(place)))?,
            )),
            false => Ok(place),
        }
    }

    /// The place itself, or what it points to if it holds a pointer.
    pub fn through(&mut self, place: Place, mode: &mut PlaceMode) -> Built<Place> {
        match self.kind(self.b.place_ty(&place)) {
            Type::Ptr(_) => self.deref(place, mode),
            _ => Ok(place),
        }
    }

    /// `x[index]` of an array, or of a pointer to one.
    fn element(
        &mut self,
        expr: ExprId,
        x: ExprId,
        index: ExprId,
        mode: &mut PlaceMode,
    ) -> Built<Place> {
        let span = self.tree.exprs[expr].span;
        let base = self.place(x, mode)?;
        let mut base = self.through(base, mode)?;
        let len = match self.kind(self.b.place_ty(&base)) {
            Type::Array(_, len) => Length::Known(len),
            // A string is a value: its length is read where it is.
            Type::String => Length::Of(self.b.assign_temp(Rvalue::Len(base.clone()))?),
            Type::Slice(_) => {
                // An element that is stored to after more is worked out
                // is one of the slice as it is now.
                if mode.later.is_some() {
                    let now = self.b.assign_temp(Rvalue::Use(Operand::Copy(base)))?;
                    base = Place::local(now);
                }
                Length::Of(self.b.assign_temp(Rvalue::Len(base.clone()))?)
            }
            _ => {
                self.unsupported(span, "an index of a value of this type");
                let ty = self.ty_of(expr);
                let zero = Rvalue::Use(self.zero(ty));
                return Ok(Place::local(self.b.assign_temp(zero)?));
            }
        };
        let known = self
            .body
            .types
            .get(&index)
            .map(|typed| typed.value.to_int());
        if let (Length::Known(len), Some(at)) = (len, known.and_then(|value| value.low_bits()))
            && at < len
        {
            return self.b.const_index(&base, at);
        }
        let (at, written) = self.index_slots(index, span)?;
        self.check(Check::Bounds(at, written, len, span), mode)?;
        self.b.index(&base, at)
    }

    /// An index in two slots of its own: as a number without a sign,
    /// since a negative index is past every length, and as the program
    /// wrote it, for the message of a failure.
    pub fn index_slots(&mut self, index: ExprId, span: SourceSpan) -> Built<(LocalId, LocalId)> {
        let word = Memo::word(self.shared, self.b.types(), false);
        let given = self.operand(index)?;
        let from = self.b.operand_ty(&given);
        Ok(match (from == word, self.kind(from)) {
            (true, _) => {
                let at = self.b.assign_temp(Rvalue::Use(given))?;
                (at, at)
            }
            (false, Type::Int { .. }) => {
                let written = self.b.assign_temp(Rvalue::Use(given))?;
                let as_given = Operand::Copy(Place::local(written));
                let cast = Rvalue::Cast(CastKind::IntToInt, as_given, word);
                (self.b.assign_temp(cast)?, written)
            }
            _ => {
                self.unsupported(span, "an index that is no integer");
                let zero = Rvalue::Use(self.zero(word));
                let at = self.b.assign_temp(zero)?;
                (at, at)
            }
        })
    }

    /// `x[lo:hi]` of a string: the same bytes, of which nothing is
    /// copied. The string is worked out first, then its bounds, and
    /// then the bounds are checked, the high one first.
    fn substring(
        &mut self,
        expr: ExprId,
        ty: TypeId,
        x: ExprId,
        bounds: [Option<ExprId>; 3],
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let mut mode = PlaceMode::default();
        let base = self.place(x, &mut mode)?;
        let base = self.through(base, &mut mode)?;
        let [lo, hi, max] = bounds;
        if !matches!(self.kind(self.b.place_ty(&base)), Type::String) || max.is_some() {
            return Ok(self.placeholder(span, ty, "a slice of a slice or an array"));
        }
        let whole = self.b.assign_temp(Rvalue::Use(Operand::Copy(base)))?;
        let whole = Place::local(whole);
        let len = self.b.assign_temp(Rvalue::Len(whole.clone()))?;
        let word = Memo::word(self.shared, self.b.types(), false);
        let zero = self.zero(word);
        let lo = match lo {
            Some(lo) => self.index_slots(lo, span)?,
            None => {
                let none = self.b.assign_temp(Rvalue::Use(zero))?;
                (none, none)
            }
        };
        let hi = match hi {
            Some(hi) => self.index_slots(hi, span)?,
            None => (len, len),
        };
        let slot = |local: LocalId| Operand::Copy(Place::local(local));
        let within = Rvalue::Binary(BinOp::Le, slot(hi.0), slot(len));
        self.assert(
            span,
            within,
            AssertKind::SliceAlen,
            vec![slot(hi.1), slot(len)],
        )?;
        let ordered = Rvalue::Binary(BinOp::Le, slot(lo.0), slot(hi.0));
        self.assert(
            span,
            ordered,
            AssertKind::SliceB,
            vec![slot(lo.1), slot(hi.0)],
        )?;
        let first = self.b.index(&whole, lo.0)?;
        let bytes = self.b.assign_temp(Rvalue::AddrOf(first, Mutability::Not))?;
        let count = Rvalue::Binary(BinOp::Sub, slot(hi.0), slot(lo.0));
        let count = self.b.assign_temp(count)?;
        let backing = Operand::Copy(self.b.field(&whole, 2)?);
        let string = self.b.place_ty(&whole);
        let parts = vec![slot(bytes), slot(count), backing];
        Ok(Rvalue::Aggregate(AggKind::Of(string), parts))
    }

    // ---- operators

    fn unary(&mut self, expr: ExprId, ty: TypeId, op: UnaryOp, x: ExprId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let number =
            |this: &mut Self, ty| matches!(this.kind(ty), Type::Int { .. } | Type::Float { .. });
        match op {
            UnaryOp::Plus => self.rvalue_as(x, ty),
            UnaryOp::Neg if number(self, ty) => {
                Ok(Rvalue::Unary(UnOp::Neg, self.operand_as(x, ty)?))
            }
            UnaryOp::Not => Ok(Rvalue::Unary(UnOp::Not, self.operand_as(x, ty)?)),
            UnaryOp::Xor => Ok(Rvalue::Unary(UnOp::BitNot, self.operand_as(x, ty)?)),
            UnaryOp::Star => {
                let place = self.place(expr, &mut PlaceMode::default())?;
                Ok(Rvalue::Use(Operand::Copy(place)))
            }
            UnaryOp::Addr => {
                let inner = Shared::unparen(self.tree, x);
                if let ExprKind::CompositeLit { .. } = self.tree.exprs[inner].kind {
                    return Ok(self.placeholder(span, ty, "the address of a literal"));
                }
                // `&x[lo:hi]`: the same elements. Written directly in a
                // `return`, its record is built in the place of the
                // caller.
                if let ExprKind::Slice { x, lo, hi, max, .. } = self.tree.exprs[inner].kind {
                    let view = self.slice_part(inner, ty, x, [lo, hi, max], Part::View)?;
                    let outer = self.outer.remove(&expr);
                    return match (outer.as_deref(), view) {
                        (Some(&[outer]), Rvalue::AddrOf(record, _)) => {
                            let place = self.b.deref(&Place::local(outer))?;
                            self.b.assign(place, Rvalue::Use(Operand::Copy(record)))?;
                            Ok(Rvalue::Use(Operand::Copy(Place::local(outer))))
                        }
                        (_, view) => Ok(view),
                    };
                }
                let place = self.place(x, &mut PlaceMode::default())?;
                Ok(Rvalue::AddrOf(place, Mutability::Mut))
            }
            UnaryOp::Neg => Ok(self.placeholder(span, ty, "a complex number")),
            UnaryOp::Recv => Ok(self.placeholder(span, ty, "a receive")),
            UnaryOp::Tilde => Ok(self.placeholder(span, ty, "this expression")),
        }
    }

    /// `a && b` or `a || b` as a value.
    fn logical(&mut self, expr: ExprId) -> Built<Rvalue> {
        let value = self.b.temp(TypeInterner::BOOL);
        let [yes, no, done] = [(); 3].map(|()| self.b.new_block());
        self.cond(expr, yes, no)?;
        for (block, holds) in [(yes, true), (no, false)] {
            self.b.switch_to(block);
            let holds = self.b.const_bool(holds);
            self.b.assign(Place::local(value), Rvalue::Use(holds))?;
            self.b.goto(done)?;
        }
        self.b.switch_to(done);
        Ok(Rvalue::Use(Operand::Copy(Place::local(value))))
    }

    /// What a pointer of the source leads to; nothing for a handle and
    /// for what is no pointer.
    fn pointer_to(&mut self, ty: TypeId) -> Option<TypeId> {
        let handle = self.memo.held(ty).is_some() || self.memo.weaks.contains_key(&ty);
        match self.kind(ty) {
            Type::Ptr(to) if !handle => Some(to),
            _ => None,
        }
    }

    /// What a pointer of the type leads to; the type itself if it is
    /// no pointer.
    pub fn pointee(&mut self, ty: TypeId) -> TypeId {
        // A `Unique` leads to the value that it owns, a `Shared` to the
        // one that it shares.
        if let Some(value) = self.memo.held(ty) {
            return value;
        }
        match self.kind(ty) {
            Type::Ptr(to) => to,
            _ => ty,
        }
    }

    /// An operand of an operator, for a place of the type `ty`. One
    /// that is put into an interface to be compared is the function's
    /// own until the statement ends.
    pub(super) fn compared_operand(&mut self, expr: ExprId, ty: TypeId) -> Built<Operand> {
        if !self.body.iface_conversions.contains_key(&expr) {
            return self.operand_as(expr, ty);
        }
        let value = self.rvalue_as(expr, ty)?;
        let value = self.fresh(expr, ty, value)?;
        self.operand_of(value)
    }

    fn binary(&mut self, expr: ExprId, op: BinaryOp, x: ExprId, y: ExprId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let known = |this: &Self, expr: ExprId| {
            let typed = this.body.types.get(&expr);
            typed.is_some_and(|typed| typed.value != Value::Unknown)
        };
        // The operands of an operator have the type of its result; the
        // count of a shift has its own. Two values that are compared
        // have one type: two pointers are compared as pointers, and a
        // pointer and a value as two values.
        let result = self.ty_of(expr);
        let (left, right) = match op {
            BinaryOp::Shl | BinaryOp::Shr => {
                let count = match known(self, y) {
                    true => Memo::word(self.shared, self.b.types(), false),
                    false => {
                        let count = self.ty_of(y);
                        self.pointee(count)
                    }
                };
                (result, count)
            }
            BinaryOp::Eq
            | BinaryOp::Ne
            | BinaryOp::Lt
            | BinaryOp::Le
            | BinaryOp::Gt
            | BinaryOp::Ge => {
                let (left, right) = (self.ty_of(x), self.ty_of(y));
                let both = match (left, right) {
                    (TypeInterner::UNIT, other) | (other, TypeInterner::UNIT) => other,
                    _ if self.pointee(left) == right => right,
                    // A value that is compared with an interface value
                    // is put into an interface of that type.
                    _ if self.is_iface(right) => right,
                    _ => left,
                };
                (both, both)
            }
            _ => (result, result),
        };
        let mut first = self.compared_operand(x, left)?;
        if self.has_effects(y) {
            first = self.stable(first)?;
        }
        let second = self.compared_operand(y, right)?;
        // An interface value is nil when its first word is, and so is
        // a function value.
        let two_words = self.is_iface(left) || self.memo.func_types.contains(&left);
        if let (BinaryOp::Eq | BinaryOp::Ne, true) = (op, two_words) {
            let nil = |operand: &Operand| {
                let zero = ConstantKind::Zero;
                matches!(operand, Operand::Const(constant) if constant.kind == zero)
            };
            match (nil(&first), nil(&second)) {
                (false, true) => return self.nil_iface(op, first),
                (true, false) => return self.nil_iface(op, second),
                _ => {}
            }
        }
        // An option with a flag is nil when it is not set.
        if let (BinaryOp::Eq | BinaryOp::Ne, Type::Option(_)) = (op, self.kind(left)) {
            let nil = |operand: &Operand| {
                let zero = ConstantKind::Zero;
                matches!(operand, Operand::Const(constant) if constant.kind == zero)
            };
            match (nil(&first), nil(&second)) {
                (false, true) => return self.nil_option(op, first),
                (true, false) => return self.nil_option(op, second),
                _ => {}
            }
        }
        // Two structs or two arrays are compared part by part.
        if let (BinaryOp::Eq | BinaryOp::Ne, Type::Struct(_) | Type::Array(..)) =
            (op, self.kind(left))
        {
            let of = if self.checked_as(x, left).is_some() {
                x
            } else {
                y
            };
            return self.equal(span, op, of, first, second);
        }
        let nonzero = self
            .body
            .types
            .get(&y)
            .is_some_and(|typed| typed.value != Value::Unknown && typed.value.sign() != 0);
        // Two strings that are joined make a new one.
        let value = self.arith(span, op, first, second, nonzero)?;
        match op {
            BinaryOp::Add => self.fresh(expr, result, value),
            _ => Ok(value),
        }
    }

    /// `first op second`, with the checks of the operator. `nonzero`
    /// says that the second operand is known not to be zero.
    pub fn arith(
        &mut self,
        span: SourceSpan,
        op: BinaryOp,
        first: Operand,
        second: Operand,
        nonzero: bool,
    ) -> Built<Rvalue> {
        let (left, right) = (self.b.operand_ty(&first), self.b.operand_ty(&second));
        let (kind, count) = (self.kind(left), self.kind(right));
        let integer = matches!(kind, Type::Int { .. });
        let number = integer || matches!(kind, Type::Float { .. });
        let scalar = number || matches!(kind, Type::Bool | Type::Ptr(_) | Type::Func(_));
        let (lowered, fits) = match op {
            BinaryOp::Add => (BinOp::Add, number),
            BinaryOp::Sub => (BinOp::Sub, number),
            BinaryOp::Mul => (BinOp::Mul, number),
            BinaryOp::Div => (BinOp::Div, number),
            BinaryOp::Rem => (BinOp::Rem, integer),
            BinaryOp::And => (BinOp::BitAnd, integer),
            BinaryOp::Or => (BinOp::BitOr, integer),
            BinaryOp::Xor => (BinOp::BitXor, integer),
            BinaryOp::AndNot => (BinOp::BitClear, integer),
            BinaryOp::Shl => (BinOp::Shl, integer),
            BinaryOp::Shr => (BinOp::Shr, integer),
            BinaryOp::Eq => (BinOp::Eq, scalar),
            BinaryOp::Ne => (BinOp::Ne, scalar),
            BinaryOp::Lt => (BinOp::Lt, number),
            BinaryOp::Le => (BinOp::Le, number),
            BinaryOp::Gt => (BinOp::Gt, number),
            BinaryOp::Ge => (BinOp::Ge, number),
            BinaryOp::AndAnd | BinaryOp::OrOr => (BinOp::BitAnd, false),
        };
        // An operand without a type was reported where it was made.
        let unit = TypeInterner::UNIT;
        if left == unit || right == unit {
            return Ok(Rvalue::Use(self.zero(unit)));
        }
        if let (Type::String, Type::String) = (&kind, &count) {
            return self.string_op(span, lowered, first, second);
        }
        // A slice is compared with nil and with nothing else.
        if let (Type::Slice(_), BinOp::Eq | BinOp::Ne) = (&kind, lowered) {
            return self.nil_slice(lowered, first, second);
        }
        if !fits {
            let ty = match lowered {
                BinOp::Eq | BinOp::Ne | BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
                    TypeInterner::BOOL
                }
                _ => left,
            };
            return Ok(self.placeholder(span, ty, "this operator on these values"));
        }
        match lowered {
            BinOp::Div | BinOp::Rem if integer && !nonzero => {
                let zero = self.b.const_int(right, 0)?;
                let divides = Rvalue::Binary(BinOp::Ne, second.clone(), zero);
                self.assert(span, divides, AssertKind::DivByZero, Vec::new())?;
            }
            BinOp::Shl | BinOp::Shr => {
                let signed = matches!(count, Type::Int { signed: true, .. });
                if signed && !matches!(second, Operand::Const(_)) {
                    let zero = self.b.const_int(right, 0)?;
                    let counts = Rvalue::Binary(BinOp::Ge, second.clone(), zero);
                    self.assert(span, counts, AssertKind::NegativeShift, Vec::new())?;
                }
            }
            _ => {}
        }
        Ok(Rvalue::Binary(lowered, first, second))
    }

    /// Whether evaluating the expression can change what another
    /// expression of the same statement reads: it calls a function.
    pub fn has_effects(&mut self, expr: ExprId) -> bool {
        if let Some(known) = self.effects_known(expr) {
            return known;
        }
        self.memo.steps += 1;
        let constant = |this: &Self| {
            let typed = this.body.types.get(&expr);
            typed.is_some_and(|typed| !matches!(typed.value, Value::Unknown))
        };
        let effects = !constant(self)
            && (self.body.operators.contains_key(&expr)
                || deeper(|| match &self.tree.exprs[expr].kind {
                    ExprKind::Call { fun, args, .. } => {
                        let callee = Shared::unparen(self.tree, *fun);
                        let quiet = !self.body.types.contains_key(&callee)
                            && !matches!(self.builtin_of(callee), Some(called) if !matches!(called, Predeclared::Len | Predeclared::Cap));
                        !quiet || args.iter().any(|&arg| self.has_effects(arg))
                    }
                    ExprKind::Unary {
                        op: UnaryOp::Recv, ..
                    } => true,
                    ExprKind::Paren(x)
                    | ExprKind::Unary { x, .. }
                    | ExprKind::Selector { x, .. } => self.has_effects(*x),
                    ExprKind::Assert { x, .. } => self.has_effects(*x),
                    ExprKind::Binary { x, y, .. } => {
                        let first = self.has_effects(*x);
                        self.has_effects(*y) || first
                    }
                    ExprKind::KeyValue { value, .. } => self.has_effects(*value),
                    ExprKind::Index { x, indices } => {
                        let base = self.has_effects(*x);
                        indices.iter().any(|&index| self.has_effects(index)) || base
                    }
                    ExprKind::CompositeLit { elems, .. } => {
                        elems.iter().any(|&elem| self.has_effects(elem))
                    }
                    ExprKind::Slice { x, lo, hi, max, .. } => {
                        let parts = [Some(*x), *lo, *hi, *max];
                        parts
                            .into_iter()
                            .flatten()
                            .any(|part| self.has_effects(part))
                    }
                    _ => false,
                }));
        self.set_effects(expr, effects);
        effects
    }

    /// The built-in function that the expression names.
    fn builtin_of(&self, callee: ExprId) -> Option<Predeclared> {
        match self.body.uses.get(&callee) {
            Some(Ref::Universe(named)) if named.kind() == PredeclaredKind::Func => Some(*named),
            _ => None,
        }
    }

    // ---- calls

    fn call(&mut self, expr: ExprId, ty: TypeId) -> Built<Rvalue> {
        let node = &self.tree.exprs[expr];
        let span = node.span;
        let ExprKind::Call {
            fun, args, spread, ..
        } = &node.kind
        else {
            return Ok(self.placeholder(span, ty, "this expression"));
        };
        let callee = Shared::unparen(self.tree, *fun);
        // What is called has no type: a built-in function, or a type,
        // which converts; or the checker knows nothing of it.
        if !self.body.types.contains_key(&callee) {
            let converts = self.body.types.contains_key(&expr);
            return match (self.builtin_of(callee), &args[..]) {
                (Some(called), _) => self.builtin(expr, ty, called, args, *spread),
                (None, [value]) if converts => self.convert(expr, *value, ty),
                _ => Ok(self.placeholder(span, ty, "a call of what is not known")),
            };
        }
        if let Some(weak) = self.weak_locked(callee) {
            let made = self.weak_lock(ty, weak)?;
            return self.fresh(expr, ty, made);
        }
        if let Some(asked) = self.option_asked(callee) {
            return self.option_call(expr, asked);
        }
        if let Some(pointer) = self.whole_cloned(callee) {
            let made = self.whole_clone(span, expr, ty, pointer)?;
            return self.fresh(expr, ty, made);
        }
        if let Some(found) = self.table_called(span, callee, args, *spread)? {
            return match found {
                Ok((called, given)) => self.call_with(expr, &called, given, args),
                Err(what) => Ok(self.placeholder(span, ty, what)),
            };
        }
        if let Some(method) = self.iface_method(callee) {
            return match self.iface_called(span, method, args, *spread)? {
                Ok((called, given)) => self.call_with(expr, &called, given, args),
                Err(what) => Ok(self.placeholder(span, ty, what)),
            };
        }
        let (called, receiver) = match self.called_by(callee, *spread)? {
            Ok(found) => found,
            // A function value: its code is called with its
            // environment.
            Err(what) => {
                return match self.value_called(callee, *spread)? {
                    Some(Ok((called, given))) => self.call_with(expr, &called, given, args),
                    Some(Err(what)) => Ok(self.placeholder(span, ty, what)),
                    None => Ok(self.placeholder(span, ty, what)),
                };
            }
        };
        let holder = self.body.selections.get(&callee).map(|found| found.holder);
        self.method_call(expr, called, receiver, holder, args)
    }

    /// The call `expr` of the function `called` with the arguments
    /// `args`; if it is a method, with the receiver that is reached
    /// from an expression by a way, of the type `holder`.
    pub(super) fn method_call(
        &mut self,
        expr: ExprId,
        called: Callee,
        receiver: Option<(ExprId, &[u32])>,
        holder: Option<CheckedTypeId>,
        args: &[ExprId],
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let mut given = Vec::with_capacity(called.params.len());
        if let (Some((x, path)), Some(&wanted)) = (receiver, called.params.first()) {
            // A receiver by value is like an argument: lent to a method
            // that only reads it, the method's own otherwise.
            let lent = called.lent.first() == Some(&true);
            let direct = lent && path.is_empty() && self.quiet(x);
            let inner = Shared::unparen(self.tree, x);
            if direct {
                self.lent.insert(inner);
            }
            // The receiver of a method of an embedded type is a part
            // of the value, of the type that has the method.
            if let (Some(holder), false) = (holder, path.is_empty()) {
                self.parts.insert(x, holder);
            }
            let value = self.receiver(span, x, path, wanted)?;
            let value = self.given(x, wanted, Rvalue::Use(value))?;
            let value = match lent {
                true => self.lent_value(x, wanted, value)?,
                false => self.taken(x, wanted, value)?,
            };
            self.parts.remove(&x);
            self.lent.remove(&inner);
            let mut value = self.operand_of(value)?;
            if args.iter().any(|&arg| self.has_effects(arg)) {
                value = self.stable(value)?;
            }
            given.push(value);
        }
        let kind = ConstantKind::Func(called.func);
        let code = Operand::Const(Constant {
            kind,
            ty: called.sig,
        });
        let called = Called {
            code: Code::Known(code),
            params: called.params,
            views: called.views,
            lent: called.lent,
            worked: called.worked,
            ret: called.ret,
            rest: called.rest,
            recovers: called.recovers,
        };
        self.call_with(expr, &called, given, args)
    }

    /// The call `expr` of what `called` says, with the arguments
    /// `args` after the receiver in `given`: each argument is lent or
    /// given as its parameter asks, the results that hold what is freed
    /// are the caller's own.
    pub(super) fn call_with(
        &mut self,
        expr: ExprId,
        called: &Called,
        mut given: Vec<Operand>,
        args: &[ExprId],
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let mut declared = called.params.len() - called.views;
        // The arguments of a function with `...` that come after those
        // of its other parameters are one slice for its last one,
        // unless the call gives a slice with `...` itself.
        let spread = matches!(
            self.tree.exprs[expr].kind,
            ExprKind::Call { spread: true, .. }
        );
        let packed = called.rest.filter(|_| !spread);
        if packed.is_some() {
            declared -= 1;
        }
        let mut wanted: Vec<_> = called.params[given.len()..declared]
            .iter()
            .map(|&param| Some(param))
            .collect();
        let (args, rest) = match packed {
            Some(_) => self.rest_args(args, &mut wanted, called.params[declared]),
            None => (args, Rest::Args(&[])),
        };
        // An argument for a parameter that the function only reads is
        // lent; any other is a value of its own, which the function
        // owns.
        let lent: Vec<bool> = (given.len()..declared)
            .map(|at| called.lent.get(at) == Some(&true))
            .collect();
        let worked: Vec<bool> = (given.len()..declared)
            .map(|at| called.worked.get(at) == Some(&true))
            .collect();
        // A value that an interface parameter is lent needs no box: the
        // interface value points to where the value is (TYP-7).
        let mut unboxed = Vec::new();
        if args.len() == lent.len() {
            for (at, &arg) in args.iter().enumerate() {
                if lent[at] && self.body.iface_conversions.contains_key(&arg) {
                    self.unboxed.insert(arg, worked[at]);
                    unboxed.push(arg);
                }
            }
        }
        // An argument that a call inside another argument cannot change
        // either is read where it is.
        let mut direct = Vec::new();
        for (at, &arg) in args.iter().enumerate() {
            let read = lent.get(at) == Some(&true) && worked.get(at) != Some(&true);
            if read && self.quiet(arg) {
                direct.push(Shared::unparen(self.tree, arg));
            }
        }
        self.lent.extend(direct.iter().copied());
        let mut values = self.values(args, &wanted)?;
        let rest = match rest {
            Rest::Args(rest) => Ok(rest),
            Rest::Results(from) => Err(values.split_off(from.min(values.len()))),
        };
        for (at, value) in values.into_iter().enumerate() {
            // One call gives all of them: each is a part of what it gave.
            let of = args.get(at).or(args.first());
            let value = match (wanted.get(at).copied().flatten(), of) {
                (Some(_), Some(arg)) if unboxed.contains(arg) => value,
                (Some(ty), Some(&arg)) if worked.get(at) == Some(&true) => {
                    self.worked_value(arg, ty, value)?
                }
                (Some(ty), Some(&arg)) if lent.get(at) == Some(&true) => {
                    self.lent_value(arg, ty, value)?
                }
                (Some(ty), Some(&arg)) => self.taken(arg, ty, value)?,
                _ => value,
            };
            given.push(self.operand_of(value)?);
        }
        for arg in unboxed {
            self.unboxed.remove(&arg);
        }
        for arg in direct {
            self.lent.remove(&arg);
        }
        if let Some(checked) = packed {
            let ty = called.params[declared];
            let elems = match rest {
                Ok(rest) => {
                    // What is worked out for the slice may change what
                    // the arguments before it read.
                    if rest.iter().any(|&arg| self.has_effects(arg)) {
                        for value in &mut given {
                            *value = self.stable(value.clone())?;
                        }
                    }
                    self.rest_values(ty, rest)?
                }
                Err(results) => self.rest_results(ty, args, results)?,
            };
            let slice = self.packed(span, checked, ty, elems)?;
            // A function that only reads the slice is lent it: it ends
            // with this statement. Any other function owns it.
            if let Operand::Copy(place) = &slice
                && called.lent.get(declared) != Some(&true)
            {
                self.take_temp(place);
            }
            given.push(slice);
            declared += 1;
        }
        // A place for each view that a result may be: one of this
        // frame, or the place that the caller of this function gave.
        let outer = self.outer.remove(&expr);
        let outer = outer.filter(|places| places.len() == called.views);
        for (at, &param) in called.params[declared..].iter().enumerate() {
            let place = match &outer {
                Some(places) => places[at],
                None => {
                    let record = self.pointee(param);
                    let zero = Rvalue::Use(self.zero(record));
                    let record = self.slot_of(zero)?;
                    self.b
                        .assign_temp(Rvalue::AddrOf(record, Mutability::Mut))?
                }
            };
            given.push(Operand::Copy(Place::local(place)));
        }
        self.empty_moved()?;
        // The call is placed where it is written, which a trace names.
        let around = self.b.loc();
        self.at(span);
        // No panic calls the function here, so its `recover` stops
        // none (PAN-1).
        if called.recovers && !self.shared.aborts {
            self.runtime_call("_gopp_recover_not", &[], Vec::new())?;
        }
        let callee = match &called.code {
            Code::Known(code) => code.clone(),
            Code::Slot(value, slot, code, nilable) => {
                self.table_slot(value, *slot, *code, nilable.then_some(span))?
            }
        };
        let result = self.b.temp(called.ret);
        let unwind = self.unwind()?;
        let next = self.b.new_block();
        self.b
            .call(callee, given, Place::local(result), Some(next), unwind)?;
        self.b.switch_to(next);
        self.b.set_loc(around);
        // What the function gives is the caller's own: each result that
        // holds what is freed.
        let mut result = Place::local(result);
        if let ExprKind::Call { unwrap: true, .. } = self.tree.exprs[expr].kind {
            result = match self.unwrapped(expr, result, called.ret)? {
                Some(result) => result,
                None => return Ok(Rvalue::Use(self.zero(TypeInterner::UNIT))),
            };
        }
        let ret = self.b.place_ty(&result);
        let checked = self.body.types.get(&expr).map(|typed| typed.ty);
        let several = checked.map(|checked| self.shared.types.get(checked));
        match (several.as_deref(), self.kind(ret)) {
            (Some(CheckedType::Tuple(checked)), Type::Tuple(parts))
                if checked.len() == parts.len() =>
            {
                for (at, (&checked, &part)) in checked.iter().zip(parts.iter()).enumerate() {
                    if !self.flat(part)
                        && let Some(ends) = self.ends_of(Some(checked), span)?
                    {
                        let part = self.b.field(&result, at as u32)?;
                        self.own_temp(part, ends);
                    }
                }
                Ok(Rvalue::Use(Operand::Copy(result)))
            }
            _ => self.fresh(expr, ret, Rvalue::Use(Operand::Copy(result))),
        }
    }

    /// The arguments of a call of a function with `...`, whose last
    /// parameter has the type `ty`: those of the parameters before it,
    /// and those of the slice. `wanted` has the types of the parameters
    /// before it; where one call gives all the arguments (`f(g())`),
    /// the types of the elements are added to it.
    pub(super) fn rest_args<'e>(
        &mut self,
        args: &'e [ExprId],
        wanted: &mut Vec<Option<TypeId>>,
        ty: TypeId,
    ) -> (&'e [ExprId], Rest<'e>) {
        let fixed = wanted.len();
        if let ([one], Type::Slice(of)) = (args, self.kind(ty)) {
            let inner = Shared::unparen(self.tree, *one);
            // By the checker's type: a function value and an option are
            // held in several parts too.
            let whole = self.body.types.get(one).map(|typed| typed.ty);
            let whole = whole.map(|whole| self.shared.types.get(whole));
            let results = match whole.as_deref() {
                Some(CheckedType::Tuple(parts)) => parts.len(),
                _ => 0,
            };
            if results >= fixed.max(2)
                && matches!(self.tree.exprs[inner].kind, ExprKind::Call { .. })
            {
                wanted.resize(results, Some(of));
                return (args, Rest::Results(fixed));
            }
        }
        let (args, rest) = args.split_at(fixed.min(args.len()));
        (args, Rest::Args(rest))
    }

    /// The arguments of the last parameter of a function with `...`,
    /// whose type is `ty`: each is an element of its own.
    pub(super) fn rest_values(&mut self, ty: TypeId, rest: &[ExprId]) -> Built<Vec<Operand>> {
        let Type::Slice(of) = self.kind(ty) else {
            return Ok(Vec::new());
        };
        let wanted = vec![Some(of); rest.len()];
        let values = self.values_each(rest, &wanted)?;
        let mut elems = Vec::with_capacity(rest.len());
        for (value, &arg) in values.into_iter().zip(rest) {
            let value = self.taken(arg, of, Rvalue::Use(value))?;
            elems.push(self.operand_of(value)?);
        }
        Ok(elems)
    }

    /// The same, where they are the last results of the one call in
    /// `args`.
    pub(super) fn rest_results(
        &mut self,
        ty: TypeId,
        args: &[ExprId],
        results: Vec<Rvalue>,
    ) -> Built<Vec<Operand>> {
        let (Type::Slice(of), [call]) = (self.kind(ty), args) else {
            return Ok(Vec::new());
        };
        let mut elems = Vec::with_capacity(results.len());
        for value in results {
            let value = self.taken(*call, of, value)?;
            elems.push(self.operand_of(value)?);
        }
        Ok(elems)
    }

    /// The elements as one new slice of the type `ty`, which the
    /// checker has as `checked`, in a temporary that owns it. Without
    /// elements the slice is nil.
    pub(super) fn packed(
        &mut self,
        span: SourceSpan,
        checked: CheckedTypeId,
        ty: TypeId,
        elems: Vec<Operand>,
    ) -> Built<Operand> {
        let Type::Slice(of) = self.kind(ty) else {
            return Ok(self.zero(ty));
        };
        if elems.is_empty() {
            return Ok(self.zero(ty));
        }
        let count = elems.len() as u64;
        let set = (0..).zip(elems).collect();
        let made = self.slice_literal(ty, of, count, set)?;
        let slice = self.slot_of(made)?;
        if let Some(ends) = self.ends_of(Some(checked), span)? {
            self.own_temp(slice.clone(), ends);
        }
        Ok(Operand::Copy(slice))
    }

    /// `call!(args)` (ERR-5): `result` has what the call gave, whose
    /// last part is an error. If it is one, the function returns it, or
    /// the program panics, as the checker found for the function around
    /// (ERR-6), in a block that the function does not go through
    /// otherwise. What is left is the value of the expression: the
    /// other results in a place of their own, none if there are none.
    fn unwrapped(&mut self, expr: ExprId, result: Place, ret: TypeId) -> Built<Option<Place>> {
        let span = self.tree.exprs[expr].span;
        let parts = match self.kind(ret) {
            Type::Tuple(parts) => parts.to_vec(),
            _ => Vec::new(),
        };
        let error = match parts.len() {
            0 => result.clone(),
            count => self.b.field(&result, count as u32 - 1)?,
        };
        if !self.is_iface(self.b.place_ty(&error)) {
            self.unsupported(span, "a call with `!` of what gives no error");
            return Ok(None);
        }
        // The address of a temporary that a call wrote is not taken:
        // the error is in a slot of its own.
        let error = self.slot_of(Rvalue::Use(Operand::Copy(error)))?;
        let first = Operand::Copy(self.b.field(&error, 0)?);
        let bytes = self.bytes_ty();
        let failed = Rvalue::Binary(BinOp::Ne, first, self.zero(bytes));
        let failed = Operand::Copy(Place::local(self.b.assign_temp(failed)?));
        let [fails, goes_on] = [(); 2].map(|()| self.b.new_block());
        self.b.switch(failed, vec![(0, goes_on)], fails)?;
        self.b.switch_to(fails);
        self.b.set_cold(fails);
        match self.body.bangs.get(&expr) {
            Some(gopp_sema::Bang::Return) => self.failed(expr, error)?,
            _ => self.must(expr, error)?,
        }
        self.b.switch_to(goes_on);
        Ok(match parts.len() {
            0 => None,
            2 => Some(self.b.field(&result, 0)?),
            count => {
                let mut kept = Vec::with_capacity(count - 1);
                for at in 0..count as u32 - 1 {
                    kept.push(Operand::Copy(self.b.field(&result, at)?));
                }
                Some(self.slot_of(Rvalue::Aggregate(AggKind::Tuple, kept))?)
            }
        })
    }

    /// The function returns the error in the place, with a zero for
    /// each of its other results, as `return zero..., err` does
    /// (ERR-7). What the statement owns ends on this way; what follows
    /// the call is written as if this way were not there.
    fn failed(&mut self, expr: ExprId, error: Place) -> Built {
        let span = self.tree.exprs[expr].span;
        let (_, place) = self.must_texts(expr);
        self.traced(&error, &place)?;
        let parts = self.result_parts();
        let mut given = Vec::with_capacity(parts.len());
        for &part in parts.iter().take(parts.len().saturating_sub(1)) {
            given.push(Rvalue::Use(self.zero(part)));
        }
        given.push(Rvalue::Use(Operand::Copy(error)));
        self.aside(|this| this.leave(span, Some(given)))
    }

    /// The program panics with the error in the place (ERR-6, ERR-9):
    /// the runtime is given the call as the source has it, where it
    /// is, and what the error says.
    fn must(&mut self, expr: ExprId, error: Place) -> Built {
        let span = self.tree.exprs[expr].span;
        let (shown, place) = self.must_texts(expr);
        self.must_with(span, &shown, &place, error)
    }

    /// The call with `!` at the place failed with the error in the
    /// place: the runtime adds the place to the trace of the error
    /// (ERR-14). Only a call that fails comes here.
    pub(super) fn traced(&mut self, error: &Place, place: &str) -> Built {
        // The two words of the error are given as they are: its
        // address is not taken, so the way of a call that does not
        // fail has nothing of this.
        let string = self.b.types().intern(Type::String);
        let bytes = self.bytes_ty();
        let args = vec![
            Operand::Copy(self.b.field(error, 0)?),
            Operand::Copy(self.b.field(error, 1)?),
            self.b.const_str(place.as_bytes()),
        ];
        self.runtime_call("_gopp_err_trace", &[bytes, bytes, string], args)
    }

    /// The call without its mark, and its file, line and column.
    pub(super) fn must_texts(&self, expr: ExprId) -> (String, String) {
        let span = self.tree.exprs[expr].span;
        let text = self.source.text();
        let shown = match &self.tree.exprs[expr].kind {
            ExprKind::Call { fun, .. } => {
                let fun = self.tree.exprs[*fun].span;
                let rest = &text[fun.hi as usize..span.hi as usize];
                let rest = rest.trim_start().strip_prefix('!').unwrap_or(rest);
                format!("{}{rest}", &text[span.lo as usize..fun.hi as usize])
            }
            _ => String::new(),
        };
        let at = self.source.line_col(span.lo);
        let file = self.source.path().file_name();
        let file = file.map(|name| name.to_string_lossy().into_owned());
        let place = format!("{}:{}:{}", file.unwrap_or_default(), at.line, at.column);
        (shown, place)
    }

    /// The panic of a call with `!` that is written as `shown` at
    /// `place`, with the error in the place.
    pub(super) fn must_with(
        &mut self,
        span: SourceSpan,
        shown: &str,
        place: &str,
        error: Place,
    ) -> Built {
        let around = self.b.loc();
        self.at(span);
        self.traced(&error, place)?;
        let string = self.b.types().intern(Type::String);
        let bytes = self.bytes_ty();
        // `error` has one method, the first of its table.
        let code = self.b.types().intern(Type::Func(FuncType {
            params: [bytes].into(),
            ret: string,
            has_env: false,
            cc: CallConv::Go,
        }));
        let says = self.table_slot(&error, 0, code, None)?;
        let held = self.b.field(&error, 1)?;
        let held = self.record(held)?;
        let message = Place::local(self.b.temp(string));
        let unwind = self.unwind()?;
        let next = self.b.new_block();
        self.b
            .call(says, vec![held], message.clone(), Some(next), unwind)?;
        self.b.switch_to(next);
        if let Some(ends) = self.ends_of(Some(self.shared.types.error()), span)? {
            self.end(error, ends)?;
        }
        let args = vec![
            self.b.const_str(shown.as_bytes()),
            self.b.const_str(place.as_bytes()),
            Operand::Copy(message),
        ];
        self.runtime_call("_gopp_panic_must", &[string, string, string], args)?;
        self.b.unreachable()?;
        self.b.set_loc(around);
        self.dead();
        Ok(())
    }

    /// `NewUnique(v)` and `NewShared(v)` (OWN-P2): the value is worked
    /// out for a new owner, which is a box of its own on the heap; the
    /// handle of the type `ty` has the address of the box, which the
    /// function `boxed` of the runtime makes.
    fn on_heap(&mut self, ty: TypeId, value: ExprId, boxed: &str) -> Built<Rvalue> {
        let of = self.pointee(ty);
        let given = self.rvalue_as(value, of)?;
        let given = self.taken(value, of, given)?;
        let slot = self.slot_of(given)?;
        let word = self.b.types().int(64, false);
        let layout = self.b.types().layout(of, self.shared.layout);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let bytes = self.bytes_ty();
        let from = self.record(slot)?;
        let made = self.runtime(boxed, &[bytes, word], bytes, vec![from, size])?;
        Ok(Rvalue::Cast(CastKind::PtrToPtr, made, ty))
    }

    /// The handle at the place as the runtime takes it.
    fn handle_at(&mut self, place: Place) -> Built<Operand> {
        let bytes = self.bytes_ty();
        let handle = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(place), bytes);
        Ok(Operand::Copy(Place::local(self.b.assign_temp(handle)?)))
    }

    /// `NewWeak(s)` (OWN-P3): one more `Weak` handle of what `s`
    /// shares, which is read where it is.
    fn new_weak(&mut self, ty: TypeId, of: ExprId) -> Built<Rvalue> {
        let place = self.place(of, &mut PlaceMode::default())?;
        let handle = self.handle_at(place)?;
        let bytes = self.bytes_ty();
        // The counts of a handle of a type with a table are those of
        // the whole value that it may be a part of.
        let counted = match self.tabled_handle(of) {
            true => "_gopp_part_weak_retain",
            false => "_gopp_weak_retain",
        };
        self.runtime_call(counted, &[bytes], vec![handle.clone()])?;
        Ok(Rvalue::Cast(CastKind::PtrToPtr, handle, ty))
    }

    /// Whether the expression is a `Shared` or a `Weak` of a type with
    /// a type table, which may hold a child of the type.
    fn tabled_handle(&self, expr: ExprId) -> bool {
        let types = self.shared.types;
        let Some(typed) = self.body.types.get(&expr) else {
            return false;
        };
        match &*types.get(types.underlying(typed.ty)) {
            CheckedType::Shared(to) | CheckedType::Weak(to) => types.has_table(*to),
            _ => false,
        }
    }

    /// The `Weak` handle that the call `w.Lock()` asks, if `callee` is
    /// such a method.
    fn weak_locked(&self, callee: ExprId) -> Option<ExprId> {
        let ExprKind::Selector { x, sel } = self.tree.exprs[callee].kind else {
            return None;
        };
        let types = self.shared.types;
        let of = self.body.types.get(&x)?.ty;
        let weak = matches!(&*types.get(types.underlying(of)), CheckedType::Weak(_));
        (weak && self.shared.names.resolve(&sel.name) == "Lock").then_some(x)
    }

    /// `w.Lock()` (OWN-P8): a `Shared` handle of what `w` names if the
    /// value has not ended, nil if it has. `ty` is the option of the
    /// handle, which is the handle.
    fn weak_lock(&mut self, ty: TypeId, weak: ExprId) -> Built<Rvalue> {
        let place = self.place(weak, &mut PlaceMode::default())?;
        let handle = self.handle_at(place)?;
        let bytes = self.bytes_ty();
        let lock = match self.tabled_handle(weak) {
            true => "_gopp_part_lock",
            false => "_gopp_weak_lock",
        };
        let locked = self.runtime(lock, &[bytes], bytes, vec![handle])?;
        Ok(Rvalue::Cast(CastKind::PtrToPtr, locked, ty))
    }

    /// The pointer or the handle that the call `p.Clone()` copies the
    /// whole value of, if `callee` is such a method: one of a type with
    /// a type table (EXP-14).
    fn whole_cloned(&self, callee: ExprId) -> Option<ExprId> {
        let ExprKind::Selector { x, sel } = self.tree.exprs[callee].kind else {
            return None;
        };
        let types = self.shared.types;
        let of = self.body.types.get(&x)?.ty;
        let tabled = match &*types.get(types.underlying(of)) {
            CheckedType::Pointer(to) | CheckedType::Unique(to) | CheckedType::Shared(to) => {
                types.has_table(*to)
            }
            _ => false,
        };
        let named = self.shared.names.resolve(&sel.name) == "Clone";
        (tabled && named && !self.body.selections.contains_key(&callee)).then_some(x)
    }

    /// `p.Clone()` (EXP-14, OWN-R6): a `Unique` with a copy of the whole
    /// value that `p` points into, at the same part. A pointer and each
    /// handle is the address of that part, so the copy is that of a
    /// `Unique` of the type, which `ty` is.
    fn whole_clone(
        &mut self,
        span: SourceSpan,
        expr: ExprId,
        ty: TypeId,
        pointer: ExprId,
    ) -> Built<Rvalue> {
        let place = self.place(pointer, &mut PlaceMode::default())?;
        let held = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(place), ty);
        let checked = self.body.types.get(&expr).map(|typed| typed.ty);
        self.cloned_value(span, checked, held)
    }

    /// The instance of a generic function of the package that the
    /// expression names with type arguments, written or inferred.
    pub(super) fn instance_named(&self, expr: ExprId) -> Option<CalleeKey> {
        let instance = self.body.instances.get(&expr)?;
        let shared = self.shared;
        let (used, _) = shared.generic_named(self.file, self.body, expr)?;
        let job = shared.instance(used, &instance.args)?;
        Some(CalleeKey::Instance(job))
    }

    /// The function that a call of `callee` calls, and its receiver
    /// with the way to it if it is a method; or why the call is not
    /// supported.
    pub(super) fn called_by(
        &mut self,
        callee: ExprId,
        spread: bool,
    ) -> Built<Result<(Callee, Option<Receiver<'a>>), &'static str>> {
        if self.body.foreign_uses.contains_key(&callee) {
            return Ok(Err("a call of a function of a header"));
        }
        let body = self.body;
        let mut receiver = None;
        // A generic function is called as the instance that its type
        // arguments make.
        let instance = match body.instances.contains_key(&callee) {
            true => match self.instance_named(callee) {
                Some(key) => Some(key),
                None => return Ok(Err("a call of this generic function")),
            },
            false => None,
        };
        // A method with type arguments of its own is called on its
        // receiver.
        if instance.is_some()
            && let Some((_, Some(selector))) = self.shared.generic_named(self.file, body, callee)
            && let ExprKind::Selector { x, .. } = self.tree.exprs[selector].kind
            && let Some(selection) = body.selections.get(&selector)
        {
            receiver = Some((x, &selection.path[..]));
        }
        let key = match (&self.tree.exprs[callee].kind, body.uses.get(&callee)) {
            _ if instance.is_some() => instance,
            (ExprKind::Name(_), Some(Ref::Object(object))) => Some(CalleeKey::Object(*object)),
            (ExprKind::Selector { .. }, Some(Ref::Member { import, member })) => {
                Some(CalleeKey::Member(*import, *member))
            }
            (ExprKind::Selector { x, sel }, _) => match body.selections.get(&callee) {
                // `base.M()` calls the method of the parent itself.
                Some(selection)
                    if matches!(
                        selection.kind,
                        SelectionKind::Method | SelectionKind::BaseMethod
                    ) =>
                {
                    receiver = Some((*x, &selection.path[..]));
                    Some(CalleeKey::Method(selection.holder, sel.name))
                }
                _ => None,
            },
            _ => None,
        };
        let called = match key {
            Some(key) => self.callee(key)?,
            None => Err("a call of this function"),
        };
        // Only a function with `...` takes a slice for its last
        // parameter that way.
        let called = called.and_then(|called| match spread && called.rest.is_none() {
            true => Err("a call with `...`"),
            false => Ok(called),
        });
        Ok(called.map(|called| (called, receiver)))
    }

    /// `defer f(args)` (PAN-3): the receiver and the arguments are
    /// evaluated now, each for an owner of its own, which is the
    /// statement until the function is left and its call is made.
    pub fn defer_call(&mut self, stmt: gopp_syntax::ast::StmtId, expr: ExprId) -> Built {
        let node = &self.tree.exprs[Shared::unparen(self.tree, expr)];
        let span = node.span;
        let Some(at) = self
            .defers
            .iter()
            .position(|deferred| deferred.stmt == stmt)
        else {
            return Ok(());
        };
        let ExprKind::Call {
            fun,
            args,
            spread,
            unwrap,
        } = &node.kind
        else {
            self.unsupported(span, "a defer statement of this expression");
            return Ok(());
        };
        let callee = Shared::unparen(self.tree, *fun);
        // The error of the call would be that of the function with the
        // loop, whose results the body does not have.
        if *unwrap && self.ranged.is_some() {
            let what = "a deferred call with `!` in the body of a range over a function";
            self.unsupported(span, what);
            return Ok(());
        }
        let known = self.body.types.contains_key(&callee);
        let built_in = self.builtin_of(callee).filter(|_| !known);
        let prints = match built_in {
            Some(Predeclared::Print) => Some(false),
            Some(Predeclared::Println) => Some(true),
            _ => None,
        };
        let other = match (built_in, &args[..]) {
            (Some(Predeclared::Panic), [_]) | (Some(Predeclared::Recover), []) => true,
            (Some(Predeclared::Delete), [map, _]) => self.holds_map(*map),
            (Some(Predeclared::Clear), [_]) | (Some(Predeclared::CopyFunc), [_, _]) => true,
            _ => false,
        };
        let refused = match () {
            // Its deferred calls would have no function to make them;
            // those of the body of a loop over a function are made by
            // the function with the loop.
            () if !self.defers_calls() && self.ranged.is_none() => {
                Some("a defer statement in a function without a symbol")
            }
            () if !known && prints.is_none() && !other => {
                Some("a defer statement of this built-in function or of a conversion")
            }
            () => None,
        };
        if let Some(what) = refused {
            self.unsupported(span, what);
            return Ok(());
        }
        if let (true, Some(called)) = (other, built_in) {
            return self.defer_built_in(at, span, called, args);
        }
        // `print` and `println` write what their arguments were here.
        if let Some(line) = prints {
            let wanted = vec![None; args.len()];
            let mut slots = Vec::with_capacity(args.len());
            for (value, &arg) in self.values(args, &wanted)?.into_iter().zip(args) {
                let ty = self.b.rvalue_ty(&value)?;
                let value = self.taken(arg, ty, value)?;
                slots.push(self.kept(arg, ty, value)?);
            }
            self.empty_moved()?;
            let waits = super::func::Waits::Print(line);
            return self.waits(at, waits, slots, Vec::new(), (span, Vec::new(), None));
        }
        let (called, receiver) = match self.called_by(callee, *spread)? {
            Ok(found) => found,
            // A function value: it is kept with its arguments, and its
            // code is called with its environment.
            Err(what) => {
                let found = self.defer_value(callee, args, *spread)?;
                let (code_ty, packed) = match found.unwrap_or(Err(what)) {
                    Ok(found) => found,
                    Err(what) => {
                        self.unsupported(span, what);
                        return Ok(());
                    }
                };
                let Some(mut borrowed) = self.borrows(span, None, args) else {
                    return Ok(());
                };
                // A literal borrows the variables that it uses.
                let used = self.body.captures.get(&callee);
                borrowed.extend(used.into_iter().flatten().map(|capture| capture.local));
                let Type::Func(sig) = self.kind(code_ty) else {
                    return Ok(());
                };
                // The parameters after the environment, without the
                // slice that the statement makes.
                let fixed = match packed {
                    Some(_) => sig.params.len().saturating_sub(2).min(args.len()),
                    None => args.len(),
                };
                let (args, rest) = args.split_at(fixed);
                let kept: Vec<ExprId> = std::iter::once(callee)
                    .chain(args.iter().copied())
                    .collect();
                let mut slots = Vec::with_capacity(kept.len() + 1);
                for (at, &part) in kept.iter().enumerate() {
                    // The value itself, then each argument for its
                    // parameter after the environment.
                    let ty = match at {
                        0 => self.ty_of(part),
                        at => sig.params[at],
                    };
                    self.deferring = (at == 0).then_some(callee);
                    let value = self.rvalue_as(part, ty);
                    self.deferring = None;
                    let value = self.taken(part, ty, value?)?;
                    slots.push(self.kept(part, ty, value)?);
                }
                if let (Some(checked), Some(&ty)) = (packed, sig.params.last()) {
                    let elems = self.rest_values(ty, rest)?;
                    slots.push(self.kept_rest(span, checked, ty, elems)?);
                }
                self.empty_moved()?;
                let (result, marked) = match *unwrap {
                    true => (Vec::new(), self.marked(expr, sig.ret, &mut slots)?),
                    false => (self.deferred_result(span, expr, sig.ret)?, None),
                };
                if *unwrap && marked.is_none() {
                    return Ok(());
                }
                let literal = matches!(self.tree.exprs[callee].kind, ExprKind::FuncLit { .. });
                let recovers = !literal || self.recovering(callee);
                let waits = super::func::Waits::Value(code_ty, recovers);
                return self.waits(at, waits, slots, result, (span, borrowed, marked));
            }
        };
        if called.views > 0 {
            self.unsupported(span, "a deferred call whose result may be a view");
            return Ok(());
        }
        // A method that takes the address of its receiver borrows the
        // variable that the receiver is in.
        let held = match (receiver, called.params.first()) {
            (Some((x, _)), Some(&wanted)) => {
                let address = matches!(self.kind(wanted), Type::Ptr(_)) && self.ty_of(x) != wanted;
                address.then_some(x)
            }
            _ => None,
        };
        let Some(borrowed) = self.borrows(span, held, args) else {
            return Ok(());
        };
        let mut slots = Vec::with_capacity(called.params.len());
        if let (Some((x, path)), Some(&wanted)) = (receiver, called.params.first()) {
            let value = self.receiver(span, x, path, wanted)?;
            let value = self.given(x, wanted, Rvalue::Use(value))?;
            let value = self.taken(x, wanted, value)?;
            slots.push(self.kept(x, wanted, value)?);
        }
        // The arguments of the last parameter of a function with `...`
        // are one slice, which the statement keeps like the others.
        let packed = called.rest.filter(|_| !*spread);
        let declared = called.params.len() - usize::from(packed.is_some());
        let mut wanted: Vec<_> = called.params[slots.len()..declared]
            .iter()
            .map(|&param| Some(param))
            .collect();
        let (args, rest) = match packed {
            Some(_) => self.rest_args(args, &mut wanted, called.params[declared]),
            None => (&args[..], Rest::Args(&[])),
        };
        let mut values = self.values(args, &wanted)?;
        let rest = match rest {
            Rest::Args(rest) => Ok(rest),
            Rest::Results(from) => {
                wanted.truncate(from);
                Err(values.split_off(from.min(values.len())))
            }
        };
        for (at, value) in values.into_iter().enumerate() {
            let of = args.get(at).or(args.first());
            if let (Some(ty), Some(&arg)) = (wanted.get(at).copied().flatten(), of) {
                let value = self.taken(arg, ty, value)?;
                slots.push(self.kept(arg, ty, value)?);
            }
        }
        if let Some(checked) = packed {
            let ty = called.params[declared];
            let elems = match rest {
                Ok(rest) => self.rest_values(ty, rest)?,
                Err(results) => self.rest_results(ty, args, results)?,
            };
            slots.push(self.kept_rest(span, checked, ty, elems)?);
        }
        self.empty_moved()?;
        let (result, marked) = match *unwrap {
            true => (Vec::new(), self.marked(expr, called.ret, &mut slots)?),
            false => (self.deferred_result(span, expr, called.ret)?, None),
        };
        if *unwrap && marked.is_none() {
            return Ok(());
        }
        let waits = super::func::Waits::Call(called);
        self.waits(at, waits, slots, result, (span, borrowed, marked))
    }

    /// What a deferred call with `!` that gives a `ret` does with an
    /// error (ERR-11a); none, with a report, for a call that is not
    /// supported. In a function that can fail the statement keeps the
    /// address of the result with the error, after everything else.
    fn marked(
        &mut self,
        expr: ExprId,
        ret: TypeId,
        slots: &mut Vec<(LocalId, Option<super::glue::Ends>)>,
    ) -> Built<Option<super::func::Marked>> {
        let span = self.tree.exprs[expr].span;
        let others = match self.kind(ret) {
            Type::Tuple(parts) => parts[..parts.len() - 1].to_vec(),
            _ => Vec::new(),
        };
        if others.into_iter().any(|part| !self.flat(part)) {
            self.unsupported(
                span,
                "a deferred call with `!` whose other results are freed",
            );
            return Ok(None);
        }
        let sets = self.body.bangs.get(&expr) == Some(&gopp_sema::Bang::SetError);
        if sets {
            let Some(own) = self.results.last().copied().flatten() else {
                self.unsupported(span, "a deferred call with `!` in this function");
                return Ok(None);
            };
            let own = self.var_place(own)?;
            let at = self.b.assign_temp(Rvalue::AddrOf(own, Mutability::Mut))?;
            slots.push((at, None));
        }
        let (shown, place) = self.must_texts(expr);
        let ends = self.ends_of(Some(self.shared.types.error()), span)?;
        Ok(Some(super::func::Marked {
            sets,
            shown,
            place,
            ends,
        }))
    }

    /// `defer` of `panic`, `recover`, and `delete` or `clear` of a map.
    /// The value of the panic and the key are those of the statement;
    /// the map is the one that its place holds when the call is made,
    /// so the statement keeps the address of that place.
    fn defer_built_in(
        &mut self,
        at: usize,
        span: SourceSpan,
        called: Predeclared,
        args: &[ExprId],
    ) -> Built {
        use super::func::Waits;
        let mut slots = Vec::new();
        let mut borrowed = Vec::new();
        let waits = match (called, args) {
            (Predeclared::Panic, [value]) => {
                let ty = self.lower(self.shared.types.any(), span);
                let held = self.rvalue_as(*value, ty)?;
                let held = self.taken(*value, ty, held)?;
                slots.push(self.kept(*value, ty, held)?);
                Waits::Panic
            }
            (Predeclared::Clear, [of]) if !self.holds_map(*of) => {
                let Some(held) = self.borrows(span, Some(*of), &[]) else {
                    return Ok(());
                };
                borrowed = held;
                let mut mode = PlaceMode::default();
                let place = self.place(*of, &mut mode)?;
                let place = self.through(place, &mut mode)?;
                let slice = self.b.place_ty(&place);
                if !matches!(self.kind(slice), Type::Slice(_)) {
                    self.unsupported(span, "a defer statement of `clear` of this value");
                    return Ok(());
                }
                let checked = self.checked_as(*of, slice);
                let address = self.b.assign_temp(Rvalue::AddrOf(place, Mutability::Mut))?;
                slots.push((address, None));
                Waits::ClearSlice(checked)
            }
            (Predeclared::CopyFunc, [to, from]) => {
                let Some(held) = self.borrows(span, None, args) else {
                    return Ok(());
                };
                borrowed = held;
                let pointer = self.ty_of(*to);
                let slice = self.pointee(pointer);
                let checked = self.checked_as(*to, slice);
                for (arg, ty) in [(*to, pointer), (*from, self.ty_of(*from))] {
                    let value = self.rvalue_as(arg, ty)?;
                    let value = self.taken(arg, ty, value)?;
                    slots.push(self.kept(arg, ty, value)?);
                }
                Waits::Copy(checked)
            }
            (Predeclared::Delete | Predeclared::Clear, [map, key @ ..]) => {
                let Some(of) = self.map_of(*map) else {
                    return Ok(());
                };
                let Some(held) = self.borrows(span, Some(*map), &[]) else {
                    return Ok(());
                };
                borrowed = held;
                let place = self.map_place(*map, &mut PlaceMode::default())?;
                let address = self.b.assign_temp(Rvalue::AddrOf(place, Mutability::Mut))?;
                slots.push((address, None));
                for &key in key {
                    let value = self.rvalue_as(key, of.key)?;
                    let value = self.taken(key, of.key, value)?;
                    slots.push(self.kept(key, of.key, value)?);
                }
                match key.is_empty() {
                    true => Waits::Clear(of),
                    false => Waits::Delete(of),
                }
            }
            _ => Waits::Nothing,
        };
        self.empty_moved()?;
        self.waits(at, waits, slots, Vec::new(), (span, borrowed, None))
    }

    /// The variables that a deferred call has the address of: of its
    /// receiver, and of each `&x` among its arguments. A variable of a
    /// block that lives in a cell is borrowed, and lives as long as the
    /// function if the statement runs (PAN-3). One that has no cell is
    /// another name of an element, which a deferred call cannot keep:
    /// that is reported, and nothing is given back.
    fn borrows(
        &mut self,
        span: SourceSpan,
        receiver: Option<ExprId>,
        args: &[ExprId],
    ) -> Option<Vec<gopp_sema::LocalId>> {
        let mut borrowed = Vec::new();
        for place in receiver.into_iter().chain(args.iter().copied()) {
            let inner = Shared::unparen(self.tree, place);
            let by_address = Some(place) == receiver
                || matches!(
                    self.tree.exprs[inner].kind,
                    ExprKind::Unary {
                        op: UnaryOp::Addr,
                        ..
                    }
                );
            let Some(local) = self.root_local(inner).filter(|_| by_address) else {
                continue;
            };
            // In the body of a loop over a function only a variable of
            // the whole function with the loop lives until the call.
            // A variable of the body lives on in the list of the block
            // with the loop, if that block has one.
            let declared = self.body.locals[local.0 as usize].span;
            let passes = |ranged: &super::ranged::Leaving| match ranged.outside(declared) {
                true => self.extended(local),
                false => !self.extends(local),
            };
            if self.ranged.as_ref().is_some_and(passes) {
                let what = "a deferred call that holds a variable of a block, in the body of a \
                            range over a function";
                self.unsupported(span, what);
                return None;
            }
            // Another name of an element of a slice or an array is a
            // pointer to the element, which the call keeps as it is:
            // the checker holds the collection until the function
            // returns. An entry of a map may move when the map grows.
            match self.names_element(local) {
                Some(false) => continue,
                Some(true) => {
                    let what = "a deferred call that holds an entry of a map by another name";
                    self.unsupported(span, what);
                    return None;
                }
                None => {}
            }
            if self.of_block(local) && !self.in_cell(local) {
                self.unsupported(span, "a deferred call that holds a variable of a block");
                return None;
            }
            borrowed.push(local);
        }
        Some(borrowed)
    }

    /// How what a deferred call gives ends when the call is made:
    /// nothing takes it. Of several results, each that holds what is
    /// freed ends by itself.
    fn deferred_result(
        &mut self,
        span: SourceSpan,
        expr: ExprId,
        ret: TypeId,
    ) -> Built<Vec<(Option<u32>, super::glue::Ends)>> {
        if self.flat(ret) {
            return Ok(Vec::new());
        }
        let checked = self.body.types.get(&expr).map(|typed| typed.ty);
        let several = checked.map(|checked| self.shared.types.get(checked));
        let (Some(CheckedType::Tuple(checked)), Type::Tuple(parts)) =
            (several.as_deref(), self.kind(ret))
        else {
            let ends = self.ends_of(checked, span)?;
            return Ok(ends.map(|ends| (None, ends)).into_iter().collect());
        };
        let mut all = Vec::new();
        for (at, (&checked, &part)) in checked.iter().zip(parts.iter()).enumerate() {
            if !self.flat(part)
                && let Some(ends) = self.ends_of(Some(checked), span)?
            {
                all.push((Some(at as u32), ends));
            }
        }
        Ok(all)
    }

    /// What a deferred call gave ends: nothing took it.
    pub(super) fn end_results(
        &mut self,
        result: Place,
        ends: &[(Option<u32>, super::glue::Ends)],
    ) -> Built {
        if ends.is_empty() {
            return Ok(());
        }
        // The address of a temporary that a call wrote is not taken:
        // its value ends in a slot of its own.
        let slot = self.slot_of(Rvalue::Use(Operand::Copy(result)))?;
        for &(part, ends) in ends {
            let place = match part {
                Some(at) => self.b.field(&slot, at)?,
                None => slot.clone(),
            };
            self.end(place, ends)?;
        }
        Ok(())
    }

    /// The `defer` statement ran: its call waits with what it keeps.
    fn waits(
        &mut self,
        at: usize,
        what: super::func::Waits,
        slots: Vec<(LocalId, Option<super::glue::Ends>)>,
        result: Vec<(Option<u32>, super::glue::Ends)>,
        (span, borrowed, marked): (
            SourceSpan,
            Vec<gopp_sema::LocalId>,
            Option<super::func::Marked>,
        ),
    ) -> Built {
        // The record owns what the slots had.
        for &(slot, _) in &slots {
            self.take_temp(&Place::local(slot));
        }
        // A statement that may run again makes a record each time,
        // which is the first of its list from here on.
        let list = self.defers[at].list.and_then(|list| self.defer_list(list));
        let bytes = self.bytes_ty();
        let word = self.b.types().int(64, false);
        // The number of the statement in its function.
        let number = at + self.ranged.as_ref().map_or(0, |ranged| ranged.base);
        let (mut fields, mut kept) = match &list {
            Some(list) => (
                vec![bytes, word],
                vec![
                    Operand::Copy(list.clone()),
                    self.b.const_int(word, number as u64)?,
                ],
            ),
            None => (vec![TypeInterner::BOOL], vec![self.b.const_bool(true)]),
        };
        for &(slot, _) in &slots {
            let slot = Place::local(slot);
            fields.push(self.b.place_ty(&slot));
            kept.push(Operand::Copy(slot));
        }
        let ty = self.b.types().tuple(&fields);
        let whole = Rvalue::Aggregate(crate::AggKind::Tuple, kept);
        let record = match list {
            Some(list) => {
                let cell = self.cell_type(ty);
                let layout = self.b.types().layout(cell, self.shared.layout);
                let size = self
                    .b
                    .const_int(word, layout.map_or(0, |layout| layout.size))?;
                let made = self.runtime("_gopp_cell_new", &[word], bytes, vec![size])?;
                let to = self.b.types().ptr(cell);
                let cell = Rvalue::Cast(CastKind::PtrToPtr, made.clone(), to);
                let cell = Place::local(self.b.assign_temp(cell)?);
                let cell = self.b.deref(&cell)?;
                let inside = self.b.field(&cell, 1)?;
                // An aggregate is built in a slot before it is stored
                // behind a pointer.
                let whole = Operand::Copy(self.slot_of(whole)?);
                self.b.assign(inside, Rvalue::Use(whole))?;
                self.b.assign(list, Rvalue::Use(made))?;
                None
            }
            None => {
                let record = self.b.temp(ty);
                self.b.assign(Place::local(record), whole)?;
                Some(record)
            }
        };
        self.defers[at].call = Some(super::func::Pending {
            what,
            record,
            ty,
            slots: slots.into_iter().map(|(_, ends)| ends).collect(),
            result,
            marked,
            span,
        });
        for local in borrowed {
            self.extend(local)?;
        }
        Ok(())
    }

    /// The slice of the last arguments of a deferred call of a
    /// function with `...`, in a slot like the other arguments.
    fn kept_rest(
        &mut self,
        span: SourceSpan,
        checked: CheckedTypeId,
        ty: TypeId,
        elems: Vec<Operand>,
    ) -> Built<(LocalId, Option<super::glue::Ends>)> {
        let ends = match self.flat(ty) {
            true => None,
            false => self.ends_of(Some(checked), span)?,
        };
        // A slice without elements owns nothing and is no slot yet.
        let slice = match self.packed(span, checked, ty, elems)? {
            Operand::Copy(place) if place.projections.is_empty() => place.local,
            empty => self.b.assign_temp(Rvalue::Use(empty))?,
        };
        Ok((slice, ends))
    }

    /// The value in a slot that the function has until it is left,
    /// with how the value ends.
    fn kept(
        &mut self,
        expr: ExprId,
        ty: TypeId,
        value: Rvalue,
    ) -> Built<(LocalId, Option<super::glue::Ends>)> {
        let slot = self.b.temp(ty);
        self.b.assign(Place::local(slot), value)?;
        let ends = match self.flat(ty) {
            true => None,
            false => {
                let checked = self.checked_as(expr, ty);
                self.ends_of(checked, self.tree.exprs[expr].span)?
            }
        };
        // The slot owns the value while the rest of the statement is
        // worked out, where a panic may leave it.
        if let Some(ends) = ends {
            self.own_temp(Place::local(slot), ends);
        }
        Ok((slot, ends))
    }

    /// The variable of the function that the place of the expression
    /// is in, or is reached through.
    pub(super) fn root_local(&self, mut expr: ExprId) -> Option<gopp_sema::LocalId> {
        loop {
            expr = match &self.tree.exprs[expr].kind {
                ExprKind::Paren(x)
                | ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Unary { x, .. } => *x,
                ExprKind::Name(_) => {
                    return match self.denoted(expr) {
                        Some(Ref::Local(local)) => Some(*local),
                        _ => self.based(expr),
                    };
                }
                _ => return None,
            };
        }
    }

    /// The receiver, if the expression is the name `base` (TYP-20).
    pub(super) fn based(&self, expr: ExprId) -> Option<gopp_sema::LocalId> {
        let expr = Shared::unparen(self.tree, expr);
        self.shared.base_of(self.tree, self.body, expr)
    }

    /// What is known of the function that the key names: the checker
    /// is asked once in a run, and the function is declared in the
    /// module of the run then.
    pub(super) fn callee(&mut self, key: CalleeKey) -> Built<Result<Callee, &'static str>> {
        let of = self.shared.package_of(self.file);
        if let Some(known) = self.memo.callees.get(&(of, key)) {
            return Ok(known.clone());
        }
        let shared = self.shared;
        let extern_c = gopp_sema::types::Marks::EXTERN;
        let in_c = match key {
            CalleeKey::Object(object) => self.home().object(object).marks.has(extern_c),
            CalleeKey::Member(import, member) => self
                .member(import, member)
                .is_some_and(|(_, object)| object.marks.has(extern_c)),
            _ => false,
        };
        let home = self.home();
        let namer = &mut self.memo.namer;
        let mut function = |package: &Package, id: gopp_sema::ObjId| {
            let object = package.objects.get(id.0 as usize)?;
            if !matches!(object.kind, ObjKind::Func) {
                return None;
            }
            // A function of C is called what it is called there.
            let symbol = match object.marks.has(extern_c) {
                true => shared.names.resolve(&object.name).to_owned(),
                false => symbol_of(namer, Entity::Object(package, id)).ok()?,
            };
            Some((symbol, None, object.ty))
        };
        let found = match key {
            CalleeKey::Object(object) => function(home, object),
            CalleeKey::Member(import, member) => match &home.object(import).kind {
                ObjKind::Import { package, .. } => function(package, member),
                _ => None,
            },
            CalleeKey::Method(holder, name) => shared
                .method(holder, name)
                .map(|method| (method.symbol.clone(), Some(method.receiver), method.sig)),
            CalleeKey::Instance(job) => Some(shared.job_called(job)),
            CalleeKey::Operator(holder, which) => shared
                .operator(holder, which)
                .map(|method| (method.symbol.clone(), Some(method.receiver), method.sig)),
        };
        let declared = |receiver: Option<_>, checked| match &*shared.types.get(checked) {
            CheckedType::Func { params, .. } => params.len() + usize::from(receiver.is_some()),
            _ => 0,
        };
        let called = match found {
            None => Err("a call of this function"),
            Some((symbol, receiver, checked)) => {
                let sig = self
                    .memo
                    .signature(shared, self.b.types(), receiver, checked);
                // A function of C takes each value as C does; what is
                // wrong with its type is said where it is declared.
                let sig = match in_c {
                    true => self
                        .memo
                        .c_signature(shared, self.b.types(), checked, true)
                        .map_err(|_| SAID),
                    false => sig,
                };
                match sig.map(|sig| (sig, self.kind(sig))) {
                    Ok((sig, Type::Func(func))) => Ok(Callee {
                        func: self.function_named(&symbol, sig)?,
                        sig,
                        views: func.params.len() - declared(receiver, checked),
                        lent: shared.lent.get(&symbol).map_or_else(
                            || std::rc::Rc::from([]),
                            |lent| std::rc::Rc::from(&lent[..]),
                        ),
                        worked: shared.worked.get(&symbol).map_or_else(
                            || std::rc::Rc::from([]),
                            |worked| std::rc::Rc::from(&worked[..]),
                        ),
                        params: func.params.iter().copied().collect(),
                        ret: func.ret,
                        rest: rest_of(shared, checked),
                        recovers: shared.recovers.contains(&symbol),
                    }),
                    Ok(_) => Err("a call of this function"),
                    Err(what) => Err(what),
                }
            }
        };
        self.memo.callees.insert((of, key), called.clone());
        Ok(called)
    }

    /// The receiver that a method takes, of the type `wanted`: the
    /// value at the end of the way of the selector, its address, or
    /// what it points to.
    pub(super) fn receiver(
        &mut self,
        span: SourceSpan,
        x: ExprId,
        path: &[u32],
        wanted: TypeId,
    ) -> Built<Operand> {
        let mut mode = PlaceMode::default();
        let base = self.place(x, &mut mode)?;
        let place = self.steps(base, path, span, &mut mode)?;
        let have = self.b.place_ty(&place);
        if self.pointee(wanted) == have && wanted != have {
            let address = Rvalue::AddrOf(place, Mutability::Mut);
            return Ok(Operand::Copy(Place::local(self.b.assign_temp(address)?)));
        }
        // A `Unique` or a `Shared` is the address of its value.
        let held = self.memo.held(have);
        if held.is_some() && held == self.pointer_to(wanted) {
            let address = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(place), wanted);
            return Ok(Operand::Copy(Place::local(self.b.assign_temp(address)?)));
        }
        if self.pointee(have) == wanted && wanted != have {
            return Ok(Operand::Copy(self.deref(place, &mut mode)?));
        }
        Ok(Operand::Copy(place))
    }

    /// `min` or `max` (with `most`) of values that are not all known:
    /// the operands are worked out from left to right, and the best so
    /// far is kept. Two floats are compared by the runtime, as Go
    /// compares them.
    fn least(&mut self, expr: ExprId, ty: TypeId, most: bool, args: &[ExprId]) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let mut operands = Vec::with_capacity(args.len());
        for &arg in args {
            let operand = self.operand_as(arg, ty)?;
            operands.push(self.stable(operand)?);
        }
        let best = Place::local(self.b.temp(ty));
        for (at, next) in operands.into_iter().enumerate() {
            if at == 0 {
                self.b.assign(best.clone(), Rvalue::Use(next))?;
                continue;
            }
            let so_far = Operand::Copy(best.clone());
            if let Type::Float { bits } = self.kind(ty) {
                let wide = self.b.types().float(64);
                let mut both = vec![so_far, next];
                if bits != 64 {
                    for value in &mut both {
                        let widened = Rvalue::Cast(CastKind::FloatToFloat, value.clone(), wide);
                        *value = self.operand_of(widened)?;
                    }
                }
                let name = match most {
                    true => "_gopp_float_max",
                    false => "_gopp_float_min",
                };
                let chosen = self.runtime(name, &[wide, wide], wide, both)?;
                let chosen = match bits {
                    64 => Rvalue::Use(chosen),
                    _ => Rvalue::Cast(CastKind::FloatToFloat, chosen, ty),
                };
                self.b.assign(best.clone(), chosen)?;
                continue;
            }
            let op = match most {
                true => BinaryOp::Gt,
                false => BinaryOp::Lt,
            };
            let better = self.arith(span, op, next.clone(), so_far, false)?;
            let better = Operand::Copy(Place::local(self.b.assign_temp(better)?));
            let [takes, after] = [(); 2].map(|()| self.b.new_block());
            self.b.switch(better, vec![(0, after)], takes)?;
            self.b.switch_to(takes);
            self.b.assign(best.clone(), Rvalue::Use(next))?;
            self.b.goto(after)?;
            self.b.switch_to(after);
        }
        // The string that is given shares its bytes with its operand.
        if let Type::String = self.kind(ty) {
            self.counted(best.clone(), true)?;
            return self.fresh(expr, ty, Rvalue::Use(Operand::Copy(best)));
        }
        Ok(Rvalue::Use(Operand::Copy(best)))
    }

    fn builtin(
        &mut self,
        expr: ExprId,
        ty: TypeId,
        called: Predeclared,
        args: &[ExprId],
        spread: bool,
    ) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        match (called, args, self.kind(ty)) {
            (
                Predeclared::Min | Predeclared::Max,
                [_, ..],
                Type::Int { .. } | Type::Float { .. } | Type::String,
            ) => return self.least(expr, ty, called == Predeclared::Max, args),
            (Predeclared::Make, [_, sizes @ ..], Type::Slice(of)) => {
                let made = self.make_slice(ty, of, sizes)?;
                return self.fresh(expr, ty, made);
            }
            (Predeclared::Make, [_, sizes @ ..], _) if self.is_map(ty) => {
                let made = self.make_map(expr, ty, sizes)?;
                return self.fresh(expr, ty, made);
            }
            (Predeclared::Delete, [map, key], _) if self.holds_map(*map) => {
                self.map_delete(*map, *key)?;
                return Ok(Rvalue::Use(self.zero(ty)));
            }
            (Predeclared::Clear, [map], _) if self.holds_map(*map) => {
                self.clear_map(*map)?;
                return Ok(Rvalue::Use(self.zero(ty)));
            }
            (Predeclared::Len, [map], _) if self.holds_map(*map) => {
                return self.map_len(ty, *map);
            }
            (Predeclared::Append, _, Type::Slice(_)) => {
                let grown = self.append(span, ty, args, spread)?;
                return self.fresh(expr, ty, grown);
            }
            (Predeclared::CopyFunc, [_, _], _) => return self.copy_into(span, ty, args),
            (Predeclared::Panic, [value], _) => {
                self.panic(span, *value)?;
                return Ok(Rvalue::Use(self.zero(ty)));
            }
            (Predeclared::Recover, [], _) => return self.recover(expr, ty),
            // The variable gives its value up where it is named.
            (Predeclared::Move, [of], _) => return self.rvalue_as(*of, ty),
            // The handle is of the type of the value, whatever its place
            // takes: a handle of a parent is made of it there (EXP-13).
            (Predeclared::NewUnique, [value], _) if self.memo.uniques.contains_key(&ty) => {
                let ty = self.ty_of(expr);
                let made = self.on_heap(ty, *value, "_gopp_iface_box")?;
                return self.fresh(expr, ty, made);
            }
            (Predeclared::NewShared, [value], _) if self.memo.shareds.contains_key(&ty) => {
                let ty = self.ty_of(expr);
                let made = self.on_heap(ty, *value, "_gopp_shared_new")?;
                return self.fresh(expr, ty, made);
            }
            (Predeclared::NewWeak, [of], _) if self.memo.weaks.contains_key(&ty) => {
                let made = self.new_weak(ty, *of)?;
                return self.fresh(expr, ty, made);
            }
            (Predeclared::Clear, [of], _) if self.clear_slice(span, *of)? => {
                return Ok(Rvalue::Use(self.zero(ty)));
            }
            (Predeclared::Cap, [of], _) => {
                if let Some(cap) = self.cap(ty, *of)? {
                    return Ok(cap);
                }
            }
            _ => {}
        }
        if let (Predeclared::Len, [of]) = (called, args) {
            let mut mode = PlaceMode::default();
            let place = self.place(*of, &mut mode)?;
            let place = self.through(place, &mut mode)?;
            if let Type::String | Type::Slice(_) | Type::Array(..) =
                self.kind(self.b.place_ty(&place))
            {
                let len = self.b.assign_temp(Rvalue::Len(place))?;
                let len = Operand::Copy(Place::local(len));
                let integer = matches!(self.kind(ty), Type::Int { .. });
                return Ok(match integer && self.b.operand_ty(&len) != ty {
                    true => Rvalue::Cast(CastKind::IntToInt, len, ty),
                    false => Rvalue::Use(len),
                });
            }
        }
        if let Predeclared::Print | Predeclared::Println = called {
            self.print(called == Predeclared::Println, args)?;
            return Ok(Rvalue::Use(self.zero(ty)));
        }
        let what = format!("the built-in function {}", called.name());
        Ok(self.placeholder(span, ty, &what))
    }

    /// An operator on two strings: the runtime joins and compares
    /// them.
    fn string_op(
        &mut self,
        span: SourceSpan,
        op: BinOp,
        first: Operand,
        second: Operand,
    ) -> Built<Rvalue> {
        let string = self.b.operand_ty(&first);
        let both = [string, string];
        let args = vec![first, second];
        match op {
            BinOp::Add => {
                let joined = self.runtime("_gopp_string_concat", &both, string, args)?;
                Ok(Rvalue::Use(joined))
            }
            BinOp::Eq | BinOp::Ne => {
                let bool = TypeInterner::BOOL;
                let same = self.runtime("_gopp_string_eq", &both, bool, args)?;
                Ok(match op {
                    BinOp::Eq => Rvalue::Use(same),
                    _ => Rvalue::Unary(UnOp::Not, same),
                })
            }
            BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
                // The order of two strings against zero is the order of
                // the strings.
                let order = self.b.types().int(64, true);
                let ordered = self.runtime("_gopp_string_compare", &both, order, args)?;
                Ok(Rvalue::Binary(op, ordered, self.zero(order)))
            }
            _ => Ok(self.placeholder(span, string, "this operator on these values")),
        }
    }

    /// `print` and `println`: the values are worked out, and then each
    /// is given to the function of the runtime that writes its kind;
    /// `println` has a space between two and a newline at the end.
    fn print(&mut self, line: bool, args: &[ExprId]) -> Built {
        let wanted = vec![None; args.len()];
        let mut values = Vec::with_capacity(args.len());
        for (value, &arg) in self.values(args, &wanted)?.into_iter().zip(args) {
            let (value, span) = (self.operand_of(value)?, self.tree.exprs[arg].span);
            // One call may give several values: each is written.
            let checked = self.body.types.get(&arg).map(|typed| typed.ty);
            let checked = checked.map(|checked| self.shared.types.get(checked));
            let Some(CheckedType::Tuple(parts)) = checked.as_deref() else {
                values.push((value, span));
                continue;
            };
            let whole = self.slot_of(Rvalue::Use(value))?;
            for at in 0..parts.len() {
                values.push((Operand::Copy(self.b.field(&whole, at as u32)?), span));
            }
        }
        self.print_values(line, values)
    }

    /// Each value is given to the runtime, which writes it; `span` is
    /// where the value is written in the source.
    pub(super) fn print_values(&mut self, line: bool, values: Vec<(Operand, SourceSpan)>) -> Built {
        for (at, (value, span)) in values.into_iter().enumerate() {
            if line && at > 0 {
                self.runtime_call("_gopp_print_space", &[], Vec::new())?;
            }
            let from = self.b.operand_ty(&value);
            let types = self.b.types();
            let (name, param, cast) = match types.get(from).clone() {
                Type::Bool => ("_gopp_print_bool", from, None),
                Type::Int { signed: true, .. } => {
                    let wide = types.int(64, true);
                    ("_gopp_print_int", wide, Some(CastKind::IntToInt))
                }
                Type::Int { signed: false, .. } => {
                    let wide = types.int(64, false);
                    ("_gopp_print_uint", wide, Some(CastKind::IntToInt))
                }
                Type::Float { .. } => {
                    let wide = types.float(64);
                    ("_gopp_print_float", wide, Some(CastKind::FloatToFloat))
                }
                Type::String => ("_gopp_print_string", from, None),
                Type::Ptr(_) => {
                    let byte = types.int(8, false);
                    let bytes = types.ptr(byte);
                    ("_gopp_print_pointer", bytes, Some(CastKind::PtrToPtr))
                }
                _ => {
                    let what = "`print` of a value of this type";
                    self.unsupported(span, what);
                    continue;
                }
            };
            let given = match cast.filter(|_| from != param) {
                Some(kind) => {
                    let cast = Rvalue::Cast(kind, value, param);
                    Operand::Copy(Place::local(self.b.assign_temp(cast)?))
                }
                None => value,
            };
            self.runtime_call(name, &[param], vec![given])?;
        }
        if line {
            self.runtime_call("_gopp_print_newline", &[], Vec::new())?;
        }
        Ok(())
    }

    /// A call of the function `name` of the runtime, which gives
    /// nothing back.
    pub fn runtime_call(&mut self, name: &str, params: &[TypeId], args: Vec<Operand>) -> Built {
        self.runtime(name, params, TypeInterner::UNIT, args)
            .map(drop)
    }

    /// A call of the function `name` of the runtime: what it gives
    /// back.
    pub fn runtime(
        &mut self,
        name: &str,
        params: &[TypeId],
        ret: TypeId,
        args: Vec<Operand>,
    ) -> Built<Operand> {
        let sig = self.b.types().intern(Type::Func(FuncType {
            params: params.into(),
            ret,
            has_env: false,
            cc: CallConv::C,
        }));
        let func = self.function_named(name, sig)?;
        let kind = ConstantKind::Func(func);
        let callee = Operand::Const(Constant { kind, ty: sig });
        let result = self.b.temp(ret);
        let unwind = self.unwind()?;
        let next = self.b.new_block();
        self.b
            .call(callee, args, Place::local(result), Some(next), unwind)?;
        self.b.switch_to(next);
        Ok(Operand::Copy(Place::local(result)))
    }

    /// `T(value)` for a type of GIR `to`.
    fn convert(&mut self, expr: ExprId, value: ExprId, to: TypeId) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        // `any(x)` puts the value into an interface as an assignment
        // to a place of the type does.
        let types = self.shared.types;
        let written = |this: &Self, expr| this.body.types.get(&expr).map(|typed| typed.ty);
        if self.is_iface(to) && self.body.iface_conversions.contains_key(&value) {
            return self.rvalue_as(value, to);
        }
        if let (true, Some(have), Some(want)) =
            (self.is_iface(to), written(self, value), written(self, expr))
        {
            let have = types.default_type(have);
            if !types.is_interface(have) && !types.is_untyped(have) {
                return self.in_iface(value, to, have, want);
            }
            let methods = |ty| types.interface_methods(ty, self.shared.names);
            if types.is_interface(have) && methods(have) != methods(want) {
                return self.iface_change(value, to, want);
            }
        }
        let mut from = self.ty_of(value);
        if from == TypeInterner::UNIT {
            from = to;
        }
        // A pointer is converted as what it points to, to all but a
        // pointer.
        if !matches!(self.kind(to), Type::Ptr(_)) {
            from = self.pointee(from);
        }
        let given = self.operand_as(value, from)?;
        if from == to {
            return Ok(Rvalue::Use(given));
        }
        let byte = self.b.types().int(8, false);
        let rune = self.b.types().int(32, true);
        let cast = match (self.kind(from), self.kind(to)) {
            (Type::String, Type::Slice(of)) if of == byte => {
                let made = self.slice_of_text("_gopp_bytes_from_string", to, given)?;
                return self.fresh(expr, to, made);
            }
            (Type::String, Type::Slice(of)) if of == rune => {
                let made = self.slice_of_text("_gopp_runes_from_string", to, given)?;
                return self.fresh(expr, to, made);
            }
            (Type::Slice(of), Type::String) if of == byte => {
                let made = self.text_of_slice("_gopp_string_from_bytes", to, given)?;
                return self.fresh(expr, to, made);
            }
            (Type::Slice(of), Type::String) if of == rune => {
                let made = self.text_of_slice("_gopp_string_from_runes", to, given)?;
                return self.fresh(expr, to, made);
            }
            // `string(r)`: the text of one code point.
            (Type::Int { signed, .. }, Type::String) => {
                let wide = self.b.types().int(64, true);
                let rune = match from == wide {
                    true => given,
                    // A number without a sign that is too large for a
                    // code point stays too large.
                    false => {
                        let wider = self.b.types().int(64, signed);
                        let cast = Rvalue::Cast(CastKind::IntToInt, given, wider);
                        let widened = Operand::Copy(Place::local(self.b.assign_temp(cast)?));
                        match signed {
                            true => widened,
                            false => {
                                let cast = Rvalue::Cast(CastKind::IntToInt, widened, wide);
                                Operand::Copy(Place::local(self.b.assign_temp(cast)?))
                            }
                        }
                    }
                };
                let text = self.runtime("_gopp_string_from_rune", &[wide], to, vec![rune])?;
                return self.fresh(expr, to, Rvalue::Use(text));
            }
            (Type::Int { .. }, Type::Int { .. }) => CastKind::IntToInt,
            (Type::Int { .. }, Type::Float { .. }) => CastKind::IntToFloat,
            (Type::Float { .. }, Type::Int { .. }) => CastKind::FloatToInt,
            (Type::Float { .. }, Type::Float { .. }) => CastKind::FloatToFloat,
            (Type::Ptr(_), Type::Ptr(_)) => CastKind::PtrToPtr,
            _ => return Ok(self.placeholder(span, to, "this conversion")),
        };
        Ok(Rvalue::Cast(cast, given, to))
    }

    // ---- literals

    /// A literal: its parts are its own, and so is the value.
    /// The way to the field `name` of an ancestor of the struct `ty`,
    /// through the parts of its parents, and the type of the field.
    fn inherited(&mut self, ty: TypeId, name: &str) -> Option<(Vec<u32>, TypeId)> {
        let lead = *self.memo.leads.get(&ty)?;
        let Type::Struct(id) = self.kind(ty) else {
            return None;
        };
        let parents: Vec<TypeId> = self.b.types().struct_fields(id)?.to_vec();
        let table = usize::from(self.memo.tabled.contains(&ty));
        for (at, &parent) in parents.iter().enumerate().take(lead as usize).skip(table) {
            self.memo.steps += 1;
            let Type::Struct(of) = self.kind(parent) else {
                continue;
            };
            let names = self.b.types().field_names(of).to_vec();
            if let Some(found) = names.iter().position(|field| **field == *name) {
                let part = self.b.types().struct_fields(of)?.get(found).copied()?;
                return Some((vec![at as u32, found as u32], part));
            }
            if let Some((mut way, part)) = self.inherited(parent, name) {
                way.insert(0, at as u32);
                return Some((way, part));
            }
        }
        None
    }

    fn composite(&mut self, expr: ExprId, ty: TypeId, elems: &[ExprId]) -> Built<Rvalue> {
        let made = self.composite_of(expr, ty, elems)?;
        self.fresh(expr, ty, made)
    }

    fn composite_of(&mut self, expr: ExprId, ty: TypeId, elems: &[ExprId]) -> Built<Rvalue> {
        let span = self.tree.exprs[expr].span;
        let value_of = |this: &Self, elem: ExprId| match this.tree.exprs[elem].kind {
            ExprKind::KeyValue { key, value } => (Some(key), value),
            _ => (None, elem),
        };
        match self.kind(ty) {
            Type::Unit => Ok(Rvalue::Use(self.zero(ty))),
            Type::Struct(id) => {
                let fields = self
                    .b
                    .types()
                    .struct_fields(id)
                    .unwrap_or_default()
                    .to_vec();
                let names = self.b.types().field_names(id).to_vec();
                // A literal without names is of a type without parents:
                // its fields come after the address of the table, if
                // the type has one.
                let lead = self.memo.leads.get(&ty).copied().unwrap_or(0) as usize;
                // Which field each element sets, in the order of the
                // source.
                // A field of an ancestor is set in the part of the
                // parent that has it (TYP-17): it counts after the
                // parts of the struct itself, by the way to it.
                let mut set: Vec<(usize, ExprId)> = Vec::with_capacity(elems.len());
                let mut deep: Vec<(Vec<u32>, TypeId)> = Vec::new();
                for (at, &elem) in elems.iter().enumerate() {
                    let (key, value) = value_of(self, elem);
                    let field = match key.map(|key| &self.tree.exprs[key].kind) {
                        Some(ExprKind::Name(name)) => match self.memo.fields.get(&(ty, *name)) {
                            Some(&known) => known,
                            None => {
                                let text = self.memo.name(self.shared, *name);
                                let found = names.iter().position(|field| **field == *text);
                                self.memo.fields.insert((ty, *name), found);
                                found
                            }
                        },
                        Some(_) => None,
                        None => Some(lead + at),
                    };
                    let inherited = match (field, key.map(|key| &self.tree.exprs[key].kind)) {
                        (None, Some(ExprKind::Name(name))) => {
                            let text = self.memo.name(self.shared, *name).to_owned();
                            self.inherited(ty, &text)
                        }
                        _ => None,
                    };
                    match (field.filter(|&field| field < fields.len()), inherited) {
                        (Some(field), _) => set.push((field, value)),
                        (None, Some(found)) => {
                            set.push((fields.len() + deep.len(), value));
                            deep.push(found);
                        }
                        (None, None) => {
                            self.unsupported(self.tree.exprs[elem].span, "this field");
                        }
                    }
                }
                let exprs: Vec<ExprId> = set.iter().map(|&(_, value)| value).collect();
                let part_of = |field: usize| match field.checked_sub(fields.len()) {
                    None => fields[field],
                    Some(at) => deep[at].1,
                };
                let wanted: Vec<_> = set.iter().map(|&(field, _)| Some(part_of(field))).collect();
                let mut parts: Vec<Operand> =
                    fields.iter().map(|&field| self.zero(field)).collect();
                let values = self.owned_each(&exprs, &wanted)?;
                let mut later = Vec::new();
                for (&(field, _), value) in set.iter().zip(values) {
                    match field.checked_sub(fields.len()) {
                        None => parts[field] = value,
                        Some(at) => later.push((at, value)),
                    }
                }
                let made = Rvalue::Aggregate(AggKind::Of(ty), parts);
                let tables = self.memo.with_tables.get(&ty).copied();
                if later.is_empty() && tables.is_none() {
                    return Ok(made);
                }
                let whole = self.slot_of(made)?;
                for (at, value) in later {
                    let mut place = whole.clone();
                    for &step in &deep[at].0 {
                        place = self.b.field(&place, step)?;
                    }
                    self.b.assign(place, Rvalue::Use(value))?;
                }
                // The parts of the parents have the tables of their own
                // types: the value has those of its type from here.
                if let Some(checked) = tables
                    && let Err(what) = self.put_tables(&whole, checked)?
                {
                    self.unsupported(span, what);
                }
                Ok(Rvalue::Use(Operand::Copy(whole)))
            }
            Type::Array(of, len) => {
                let mut set: Vec<(u64, ExprId)> = Vec::with_capacity(elems.len());
                let mut next = 0;
                for &elem in elems {
                    let (key, value) = value_of(self, elem);
                    let known = key.and_then(|key| self.body.types.get(&key));
                    if let Some(at) = known.and_then(|typed| typed.value.to_int().low_bits()) {
                        next = at;
                    }
                    if next < len {
                        set.push((next, value));
                    }
                    next += 1;
                }
                let exprs: Vec<ExprId> = set.iter().map(|&(_, value)| value).collect();
                let wanted: Vec<_> = set.iter().map(|_| Some(of)).collect();
                let values = self.owned_each(&exprs, &wanted)?;
                if len <= ARRAY_AT_ONCE {
                    let mut parts: Vec<Operand> = (0..len).map(|_| self.zero(of)).collect();
                    for (&(at, _), value) in set.iter().zip(values) {
                        parts[at as usize] = value;
                    }
                    return Ok(Rvalue::Aggregate(AggKind::Array(of), parts));
                }
                let zero = Rvalue::Use(self.zero(ty));
                let whole = Place::local(self.b.assign_temp(zero)?);
                for (&(at, _), value) in set.iter().zip(values) {
                    let element = self.b.const_index(&whole, at)?;
                    self.b.assign(element, Rvalue::Use(value))?;
                }
                Ok(Rvalue::Use(Operand::Copy(whole)))
            }
            Type::Slice(of) => {
                let mut set: Vec<(u64, ExprId)> = Vec::with_capacity(elems.len());
                let (mut next, mut count) = (0, 0);
                for &elem in elems {
                    let (key, value) = value_of(self, elem);
                    let known = key.and_then(|key| self.body.types.get(&key));
                    if let Some(at) = known.and_then(|typed| typed.value.to_int().low_bits()) {
                        next = at;
                    }
                    set.push((next, value));
                    next += 1;
                    count = count.max(next);
                }
                let exprs: Vec<ExprId> = set.iter().map(|&(_, value)| value).collect();
                let wanted: Vec<_> = set.iter().map(|_| Some(of)).collect();
                let values = self.owned_each(&exprs, &wanted)?;
                let set = set.iter().map(|&(at, _)| at).zip(values).collect();
                self.slice_literal(ty, of, count, set)
            }
            Type::Ptr(_) if self.is_map(ty) => self.map_literal(expr, ty, elems),
            _ => Ok(self.placeholder(span, ty, "this literal")),
        }
    }

    /// `place = value`. A value that is made of parts is made in a slot
    /// of its own first if the place is behind a pointer, or if a part
    /// reads the variable that the place is in. A place whose type is
    /// not supported, which was reported, takes nothing.
    pub fn store(&mut self, place: Place, value: Rvalue) -> Built {
        if self.b.place_ty(&place) == TypeInterner::UNIT {
            let zero = Rvalue::Use(self.zero(TypeInterner::UNIT));
            return self.b.assign(place, zero);
        }
        let reads = |operand: &Operand| match operand {
            Operand::Copy(read) | Operand::Move(read) => read.local == place.local,
            Operand::Const(_) => false,
        };
        let behind = place
            .projections
            .iter()
            .any(|step| step.kind == ProjectionKind::Deref);
        let value = match &value {
            Rvalue::Aggregate(_, parts) if behind || parts.iter().any(reads) => {
                Rvalue::Use(self.operand_of(value)?)
            }
            _ => value,
        };
        self.b.assign(place, value)
    }

    /// One value for each expression, evaluated from left to right.
    pub fn values_each(
        &mut self,
        exprs: &[ExprId],
        wanted: &[Option<TypeId>],
    ) -> Built<Vec<Operand>> {
        let mut later = false;
        let mut effects_after = vec![false; exprs.len()];
        for at in (0..exprs.len()).rev() {
            effects_after[at] = later;
            later |= self.has_effects(exprs[at]);
        }
        let mut values = Vec::with_capacity(exprs.len());
        for (at, &expr) in exprs.iter().enumerate() {
            let ty = match wanted[at] {
                Some(ty) => ty,
                None => self.ty_of(expr),
            };
            let mut value = self.operand_as(expr, ty)?;
            if effects_after[at] {
                value = self.stable(value)?;
            }
            values.push(value);
        }
        Ok(values)
    }
}
