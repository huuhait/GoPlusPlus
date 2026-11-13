//! The verifier (gir.md §9): what every pass may rely on. It trusts no
//! type that a place or a constant carries and works each out again;
//! every walk keeps a work list, and the work grows with the module.
//!
//! The data model holds two rules by itself: a block has its end
//! (`V020`), and a place has no type but that of its steps (`V031`).
//! Three rules are about parts that the model does not have yet and are
//! not checked: the types that cross to C (`V067`), prologues (`V077`)
//! and drop glue (`V081`). For the same reason `V011`, `V014`, `V073`
//! and `V074` say nothing of the source of a local, its glue, or where
//! an inlined local comes from.

mod initialized;
#[cfg(test)]
mod tests;

use std::fmt;

use rayon::prelude::*;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::ir::*;
use crate::types::{Type, TypeId, TypeInterner};

/// How many leaves the static data of one global may have.
pub(crate) const MAX_LEAVES: usize = 1 << 20;

/// A rule that a module breaks, and where.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VerifyError {
    /// The number of the rule: 27 for `V027`.
    pub rule: u16,
    /// The function, with the block and the statement in it; the end of
    /// a block counts as the statement after its last.
    pub func: Option<String>,
    pub at: Option<(u32, u32)>,
    pub message: String,
}

impl fmt::Display for VerifyError {
    /// `V027: fn main.f: bb3 #2: duplicate switch case 1`.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "V{:03}: ", self.rule)?;
        if let Some(func) = &self.func {
            write!(f, "fn {func}: ")?;
        }
        if let Some((block, stmt)) = self.at {
            write!(f, "bb{block} #{stmt}: ")?;
        }
        f.write_str(&self.message)
    }
}

/// Checks the module. `Err` has every rule that it breaks.
pub fn verify_module(module: &Module) -> Result<(), Vec<VerifyError>> {
    let (errors, _) = verify_counted(module);
    match errors.is_empty() {
        true => Ok(()),
        false => Err(errors),
    }
}

/// `verify_module`, with the number of steps that it took. The
/// functions are checked side by side; what is found is put together
/// in their order.
pub(crate) fn verify_counted(module: &Module) -> (Vec<VerifyError>, u64) {
    let mut whole = Verifier::new(module, None);
    whole.types();
    whole.globals();
    let mut names = FxHashSet::default();
    let funcs: Vec<(&Function, bool)> = module
        .funcs
        .values()
        .map(|func| (func, names.insert(func.name.as_str())))
        .collect();
    let each: Vec<Verifier> = funcs
        .par_iter()
        .map(|&(func, first)| {
            let mut verifier = Verifier::new(module, Some(func.name.clone()));
            if !first {
                verifier.error(1, format!("the symbol {} names two functions", func.name));
            }
            verifier.function(func);
            verifier
        })
        .collect();
    for one in each {
        whole.errors.extend(one.errors);
        whole.steps += one.steps;
    }
    (whole.errors, whole.steps)
}

struct Verifier<'m> {
    module: &'m Module,
    errors: Vec<VerifyError>,
    steps: u64,
    func: Option<String>,
    at: Option<(u32, u32)>,
}

fn raw(block: BlockId) -> u32 {
    block.into_raw().into_u32()
}

impl<'m> Verifier<'m> {
    fn new(module: &'m Module, func: Option<String>) -> Self {
        Self {
            module,
            errors: Vec::new(),
            steps: 0,
            func,
            at: None,
        }
    }

    fn error(&mut self, rule: u16, message: String) {
        self.errors.push(VerifyError {
            rule,
            func: self.func.clone(),
            at: self.at,
            message,
        });
    }

    fn types_of(&self) -> &TypeInterner {
        &self.module.types
    }

    fn shown(&self, ty: TypeId) -> String {
        self.module.types.display(ty)
    }

    fn is_int(&self, ty: TypeId) -> bool {
        matches!(self.types_of().get(ty), Type::Int { .. })
    }

    fn is_float(&self, ty: TypeId) -> bool {
        matches!(self.types_of().get(ty), Type::Float { .. })
    }

    fn is_pointer(&self, ty: TypeId) -> bool {
        matches!(self.types_of().get(ty), Type::Ptr(_) | Type::Func(_))
    }

    /// Whether `ty` is `*u8`.
    fn is_bytes(&self, ty: TypeId) -> bool {
        let byte = Type::Int {
            bits: 8,
            signed: false,
        };
        matches!(self.types_of().get(ty), Type::Ptr(to) if *self.types_of().get(*to) == byte)
    }

    fn is_u64(&self, ty: TypeId) -> bool {
        let wanted = Type::Int {
            bits: 64,
            signed: false,
        };
        *self.types_of().get(ty) == wanted
    }

    /// The types of the module: their shapes, and that none holds itself
    /// by value.
    fn types(&mut self) {
        let types = &self.module.types;
        // The types that a value of each type holds by value.
        let held = |ty: TypeId| -> Vec<TypeId> {
            match types.get(ty) {
                Type::Array(of, _) | Type::Option(of) => vec![*of],
                Type::Tuple(fields) => fields.to_vec(),
                Type::Struct(id) => types.struct_fields(*id).unwrap_or_default().to_vec(),
                Type::Rec(id) => types.rec_body(*id).into_iter().collect(),
                _ => Vec::new(),
            }
        };
        for ty in types.ids() {
            self.steps += 1;
            match types.get(ty) {
                Type::Int { bits, .. } if ![8, 16, 32, 64].contains(bits) => {
                    self.error(2, format!("an integer of {bits} bits"));
                }
                Type::Float { bits } if ![32, 64].contains(bits) => {
                    self.error(2, format!("a float of {bits} bits"));
                }
                Type::Struct(id) if types.struct_fields(*id).is_none() => {
                    self.error(2, format!("the struct {} has no fields", self.shown(ty)));
                }
                Type::Rec(id) if types.rec_body(*id).is_none() => {
                    self.error(2, format!("the type {} has no body", self.shown(ty)));
                }
                Type::Func(func) if func.has_env => {
                    let env = func
                        .params
                        .first()
                        .is_some_and(|&first| self.is_bytes(first));
                    if func.cc != CallConv::Go || !env {
                        let message = format!(
                            "{}: an environment needs the Go convention and a first *u8",
                            self.shown(ty)
                        );
                        self.error(78, message);
                    }
                }
                _ => {}
            }
        }
        // A walk over what is held by value: a type that is met again
        // while it is open holds itself.
        #[derive(Clone, Copy, PartialEq)]
        enum Seen {
            Open,
            Done,
        }
        let mut seen: FxHashMap<TypeId, Seen> = FxHashMap::default();
        for start in types.ids() {
            let mut rest = vec![(start, false)];
            while let Some((ty, leaving)) = rest.pop() {
                self.steps += 1;
                if leaving {
                    seen.insert(ty, Seen::Done);
                    continue;
                }
                match seen.get(&ty) {
                    Some(Seen::Done) => continue,
                    Some(Seen::Open) => {
                        let message = format!("{} holds itself by value", self.shown(ty));
                        self.error(3, message);
                        continue;
                    }
                    None => {}
                }
                seen.insert(ty, Seen::Open);
                rest.push((ty, true));
                rest.extend(held(ty).into_iter().map(|part| (part, false)));
            }
        }
    }

    fn globals(&mut self) {
        let mut names = FxHashSet::default();
        for (_, global) in self.module.globals.iter() {
            self.steps += 1;
            if !names.insert(global.name.as_str()) {
                self.error(71, format!("the symbol {} names two globals", global.name));
            }
            if global.ty == TypeInterner::UNIT {
                self.error(71, format!("the global {} has no value", global.name));
            }
            if !global.parts.is_empty() {
                self.static_data(global);
            }
        }
    }

    /// The static data of a global says one constant for each leaf of
    /// its type, of a kind that the leaf takes.
    fn static_data(&mut self, global: &Global) {
        let types = &self.module.types;
        let leaves = types.leaves(global.ty, MAX_LEAVES).unwrap_or_default();
        if leaves.len() != global.parts.len() {
            let message = format!(
                "the global {} has {} leaves and {} parts",
                global.name,
                leaves.len(),
                global.parts.len()
            );
            self.error(72, message);
            return;
        }
        for (at, (&leaf, part)) in leaves.iter().zip(&global.parts).enumerate() {
            self.steps += 1;
            self.constant(part);
            let fits = match (part.kind, types.get(leaf)) {
                (ConstantKind::Zero, _) => true,
                (ConstantKind::Int(_), Type::Int { .. }) | (ConstantKind::Bool(_), Type::Bool) => {
                    part.ty == leaf
                }
                (ConstantKind::Str(_), Type::String) => true,
                // The address of a function or of a global, as the
                // pointer that the leaf is.
                (
                    ConstantKind::Null | ConstantKind::Func(_) | ConstantKind::Global(_),
                    Type::Ptr(_) | Type::Func(_),
                ) => true,
                _ => false,
            };
            if !fits {
                let message = format!(
                    "part {at} of the global {} is no {}",
                    global.name,
                    self.shown(leaf)
                );
                self.error(72, message);
            }
        }
    }

    fn span(&mut self, loc: Span) {
        if loc.file as usize > self.module.files.len() {
            self.error(70, format!("no file {}", loc.file - 1));
        }
    }

    fn function(&mut self, func: &Function) {
        let Type::Func(sig) = self.types_of().get(func.sig).clone() else {
            let message = format!("the signature {} is no function type", self.shown(func.sig));
            return self.error(10, message);
        };
        if func.cc != sig.cc {
            self.error(75, "the convention is not that of the signature".to_owned());
        }
        self.span(func.loc);
        if func.is_declaration() {
            if func.inline != Inline::Default {
                self.error(79, "a declaration has an inline hint".to_owned());
            }
            if !func.locals.is_empty() {
                self.error(12, "a declaration has locals".to_owned());
            }
            return;
        }
        self.locals(func, &sig.params, sig.ret);
        self.scopes(func);
        if !self.targets(func) {
            return;
        }
        self.edges(func);
        let taken = self.addresses(func);
        for (id, block) in func.blocks.iter() {
            for (at, stmt) in block.stmts.iter().enumerate() {
                self.steps += 1;
                self.at = Some((raw(id), at as u32));
                self.span(stmt.loc);
                self.scope(func, stmt.scope);
                self.statement(func, stmt);
            }
            self.steps += 1;
            self.at = Some((raw(id), block.stmts.len() as u32));
            self.span(block.term.loc);
            self.scope(func, block.term.scope);
            self.terminator(func, block, &taken);
        }
        self.at = None;
        self.initialized(func, &taken);
    }

    fn locals(&mut self, func: &Function, params: &[TypeId], ret: TypeId) {
        for (at, (_, local)) in func.locals.iter().enumerate() {
            self.steps += 1;
            let wanted = match at {
                0 => Some((LocalKind::Return, ret)),
                at => params.get(at - 1).map(|&param| (LocalKind::Arg, param)),
            };
            let fits = match wanted {
                Some((kind, ty)) => local.kind == kind && local.ty == ty,
                None => matches!(local.kind, LocalKind::Var | LocalKind::Temp),
            };
            if !fits {
                self.error(11, format!("local _{at} is not what the signature says"));
            }
            if at == 0 && local.owned {
                self.error(14, "the place of the result is owned".to_owned());
            }
            if local.scope.0 as usize > func.scopes.len() {
                self.error(74, format!("local _{at} is in no scope"));
            }
            self.span(local.loc);
        }
        if func.locals.len() <= params.len() {
            self.error(11, "locals of the signature are missing".to_owned());
        }
    }

    fn scopes(&mut self, func: &Function) {
        for (at, scope) in func.scopes.iter().enumerate() {
            self.steps += 1;
            if scope.parent.0 as usize > at {
                self.error(73, format!("scope {} comes before its parent", at + 1));
            }
            if let ScopeKind::Inlined(callee) = scope.kind {
                let defined = (callee.into_raw().into_u32() as usize) < self.module.funcs.len()
                    && !self.module.funcs[callee].is_declaration();
                if !defined {
                    self.error(73, format!("scope {} inlines no defined function", at + 1));
                }
            }
            self.span(scope.loc);
        }
    }

    fn scope(&mut self, func: &Function, scope: ScopeId) {
        if scope.0 as usize > func.scopes.len() {
            self.error(74, format!("no scope {}", scope.0));
        }
    }

    /// Whether unwinding runs the block.
    fn unwinds(block: &BasicBlock) -> bool {
        block.cleanup || block.catch
    }

    /// Whether every edge leads to a block of the function.
    fn targets(&mut self, func: &Function) -> bool {
        let count = func.blocks.len() as u32;
        let mut valid = true;
        for (id, block) in func.blocks.iter() {
            self.steps += 1;
            for (target, _) in block.term.kind.successors() {
                if raw(target) >= count {
                    self.at = Some((raw(id), block.stmts.len() as u32));
                    self.error(21, format!("no block bb{}", raw(target)));
                    valid = false;
                }
            }
        }
        self.at = None;
        valid
    }

    /// The rules of the edges: what is reached, and how cleanup blocks
    /// are entered and left.
    fn edges(&mut self, func: &Function) {
        let entry = func.blocks.iter().next();
        if entry.is_some_and(|(_, block)| Self::unwinds(block)) {
            self.error(13, "the first block is a cleanup block".to_owned());
        }
        let mut reached = vec![false; func.blocks.len()];
        let mut rest: Vec<BlockId> = entry.map(|(id, _)| id).into_iter().collect();
        while let Some(id) = rest.pop() {
            if std::mem::replace(&mut reached[raw(id) as usize], true) {
                continue;
            }
            let block = &func.blocks[id];
            self.at = Some((raw(id), block.stmts.len() as u32));
            for (target, unwinds) in block.term.kind.successors() {
                self.steps += 1;
                let to = &func.blocks[target];
                if raw(target) == 0 {
                    self.error(13, "an edge leads back to the first block".to_owned());
                }
                match unwinds {
                    true if !Self::unwinds(to) => {
                        let message =
                            format!("unwinding enters bb{}, no cleanup block", raw(target));
                        self.error(24, message);
                    }
                    false if to.catch && !block.catch => {
                        self.error(
                            23,
                            format!("bb{} is entered by unwinding only", raw(target)),
                        );
                    }
                    false if Self::unwinds(block) != Self::unwinds(to) && !block.catch => {
                        let message = format!("the edge to bb{} crosses the cleanup", raw(target));
                        self.error(23, message);
                    }
                    _ => {}
                }
                rest.push(target);
            }
        }
        for (id, _) in func.blocks.iter() {
            if !reached[raw(id) as usize] {
                self.at = Some((raw(id), 0));
                self.error(22, "the block is not reached".to_owned());
            }
        }
        self.at = None;
    }

    /// The locals whose address is taken: the address of what a pointer
    /// in a local leads to is not the address of the local.
    fn addresses(&mut self, func: &Function) -> FxHashSet<LocalId> {
        let mut taken = FxHashSet::default();
        for (_, block) in func.blocks.iter() {
            for stmt in &block.stmts {
                self.steps += 1;
                if let StatementKind::Assign(assigned) = &stmt.kind
                    && let Rvalue::AddrOf(place, _) = &assigned.1
                    && place
                        .projections
                        .iter()
                        .all(|step| step.kind != ProjectionKind::Deref)
                {
                    taken.insert(place.local);
                }
            }
        }
        taken
    }

    fn local(&mut self, func: &Function, local: LocalId, rule: u16) -> bool {
        let valid = (local.into_raw().into_u32() as usize) < func.locals.len();
        if !valid {
            self.error(rule, format!("no local _{}", local.into_raw().into_u32()));
        }
        valid
    }

    /// The type of the place, worked out from its local step by step.
    /// `None` if a step is wrong, which is reported.
    fn place(&mut self, func: &Function, place: &Place) -> Option<TypeId> {
        if !self.local(func, place.local, 30) {
            return None;
        }
        let mut ty = func.locals[place.local].ty;
        for step in &place.projections {
            self.steps += 1;
            let types = self.types_of();
            // What the step gives is its type, or a type that is told
            // by what it is made of.
            let fits: Result<bool, (u16, &str)> = match (step.kind, types.get(ty)) {
                (ProjectionKind::Deref, Type::Ptr(to)) => Ok(step.ty == *to),
                (ProjectionKind::Deref, _) => Err((35, "deref of")),
                (ProjectionKind::Field(at), base) => match (base, at) {
                    (Type::String, 0 | 2) | (Type::Slice(_), 3) => Ok(self.is_bytes(step.ty)),
                    (Type::String, 1) | (Type::Slice(_), 1 | 2) => Ok(self.is_u64(step.ty)),
                    (Type::Slice(of), 0) => Ok(*types.get(step.ty) == Type::Ptr(*of)),
                    (Type::Option(of), 0) => Ok(step.ty == *of),
                    (Type::Option(_), 1) => Ok(step.ty == TypeInterner::BOOL),
                    (Type::Tuple(fields), at) => match fields.get(at as usize) {
                        Some(&field) => Ok(step.ty == field),
                        None => Err((32, "no such field of")),
                    },
                    (Type::Struct(id), at) => {
                        let fields = types.struct_fields(*id).unwrap_or_default();
                        match fields.get(at as usize) {
                            Some(&field) => Ok(step.ty == field),
                            None => Err((32, "no such field of")),
                        }
                    }
                    _ => Err((32, "no such field of")),
                },
                (ProjectionKind::Index(at), base) => {
                    let integer = (at.into_raw().into_u32() as usize) < func.locals.len()
                        && self.is_int(func.locals[at].ty);
                    let byte = Type::Int {
                        bits: 8,
                        signed: false,
                    };
                    match base {
                        _ if !integer => Err((34, "index by no integer into")),
                        Type::Array(of, _) | Type::Slice(of) => Ok(step.ty == *of),
                        Type::String => Ok(*types.get(step.ty) == byte),
                        _ => Err((32, "index of")),
                    }
                }
                (ProjectionKind::ConstIndex(at), Type::Array(of, count)) if at < *count => {
                    Ok(step.ty == *of)
                }
                (ProjectionKind::ConstIndex(_), _) => Err((32, "constant index of")),
                (ProjectionKind::Downcast(_), _) => Err((32, "variant of")),
            };
            match fits {
                Ok(true) => {}
                Ok(false) => {
                    let message =
                        format!("a step of {} says {}", self.shown(ty), self.shown(step.ty));
                    self.error(33, message);
                    return None;
                }
                Err((rule, what)) => {
                    self.error(rule, format!("{what} {}", self.shown(ty)));
                    return None;
                }
            }
            ty = step.ty;
        }
        Some(ty)
    }

    fn constant(&mut self, constant: &Constant) {
        let ty = constant.ty;
        let types = self.types_of();
        let fits = match constant.kind {
            ConstantKind::Int(value) => match types.get(ty) {
                Type::Int { bits, .. } => value.checked_shr((*bits).into()).unwrap_or(0) == 0,
                _ => false,
            },
            ConstantKind::Float(bits) => match types.get(ty) {
                Type::Float { bits: 32 } => bits >> 32 == 0,
                Type::Float { .. } => true,
                _ => false,
            },
            ConstantKind::Bool(_) => ty == TypeInterner::BOOL,
            ConstantKind::Str(at) => {
                (at as usize) < self.module.strings.len() && *types.get(ty) == Type::String
            }
            ConstantKind::Func(func) => {
                (func.into_raw().into_u32() as usize) < self.module.funcs.len()
                    && self.module.funcs[func].sig == ty
            }
            ConstantKind::Null => self.is_pointer(ty),
            ConstantKind::Zero => true,
            ConstantKind::Global(global) => {
                (global.into_raw().into_u32() as usize) < self.module.globals.len()
                    && *types.get(ty) == Type::Ptr(self.module.globals[global].ty)
            }
        };
        if !fits {
            let message = format!("the constant {:?} is no {}", constant.kind, self.shown(ty));
            self.error(40, message);
        }
    }

    fn operand(&mut self, func: &Function, operand: &Operand) -> Option<TypeId> {
        match operand {
            Operand::Copy(place) | Operand::Move(place) => self.place(func, place),
            Operand::Const(constant) => {
                self.constant(constant);
                Some(constant.ty)
            }
        }
    }

    fn statement(&mut self, func: &Function, stmt: &Statement) {
        match &stmt.kind {
            StatementKind::StorageLive(local) | StatementKind::StorageDead(local) => {
                self.local(func, *local, 36);
            }
            StatementKind::Nop => {}
            StatementKind::Assign(assigned) => {
                let (dest, rvalue) = &**assigned;
                let Some(wanted) = self.place(func, dest) else {
                    return;
                };
                self.rvalue(func, dest, rvalue, wanted);
            }
        }
    }

    /// The rvalue gives a value of the type `wanted` of the place `dest`.
    fn rvalue(&mut self, func: &Function, dest: &Place, rvalue: &Rvalue, wanted: TypeId) {
        let shown = self.shown(wanted);
        match rvalue {
            Rvalue::Use(operand) => {
                if let Some(given) = self.operand(func, operand)
                    && given != wanted
                {
                    self.error(
                        50,
                        format!("a {} is assigned to {shown}", self.shown(given)),
                    );
                }
            }
            Rvalue::AddrOf(place, _) => {
                if let Some(of) = self.place(func, place)
                    && *self.types_of().get(wanted) != Type::Ptr(of)
                {
                    self.error(
                        54,
                        format!("the address of {} is no {shown}", self.shown(of)),
                    );
                }
            }
            Rvalue::Len(place) => {
                let Some(of) = self.place(func, place) else {
                    return;
                };
                let counted = matches!(
                    self.types_of().get(of),
                    Type::String | Type::Slice(_) | Type::Array(..)
                );
                if !counted || !self.is_u64(wanted) {
                    self.error(56, format!("the length of {} as {shown}", self.shown(of)));
                }
            }
            Rvalue::Unary(op, operand) => {
                let Some(given) = self.operand(func, operand) else {
                    return;
                };
                let fits = match op {
                    UnOp::Neg => self.is_int(given) || self.is_float(given),
                    UnOp::BitNot => self.is_int(given),
                    UnOp::Not => given == TypeInterner::BOOL,
                };
                if !fits || given != wanted {
                    let message = format!("{op:?} of {} as {shown}", self.shown(given));
                    self.error(51, message);
                }
            }
            Rvalue::Binary(op, left, right) => {
                let (Some(left), Some(right)) =
                    (self.operand(func, left), self.operand(func, right))
                else {
                    return;
                };
                use BinOp::*;
                let number = self.is_int(left) || self.is_float(left);
                let (fits, result) = match op {
                    Shl | Shr => (self.is_int(left) && self.is_int(right), left),
                    _ if left != right => (false, left),
                    Add | Sub | Mul | Div => (number, left),
                    Rem | BitAnd | BitOr | BitXor | BitClear => (self.is_int(left), left),
                    Eq | Ne => {
                        let scalar = number || self.is_pointer(left) || left == TypeInterner::BOOL;
                        (scalar, TypeInterner::BOOL)
                    }
                    Lt | Le | Gt | Ge => (number, TypeInterner::BOOL),
                };
                if !fits || result != wanted {
                    let message = format!(
                        "{op:?} of {} and {} as {shown}",
                        self.shown(left),
                        self.shown(right)
                    );
                    self.error(52, message);
                }
            }
            Rvalue::Cast(kind, operand, to) => {
                let Some(from) = self.operand(func, operand) else {
                    return;
                };
                let (int, float, pointer) = (
                    |ty| self.is_int(ty),
                    |ty| self.is_float(ty),
                    |ty| self.is_pointer(ty),
                );
                let fits = match kind {
                    CastKind::IntToInt => int(from) && int(*to),
                    CastKind::IntToFloat => int(from) && float(*to),
                    CastKind::FloatToInt => float(from) && int(*to),
                    CastKind::FloatToFloat => float(from) && float(*to),
                    CastKind::PtrToPtr => pointer(from) && pointer(*to),
                    CastKind::PtrToInt => pointer(from) && int(*to),
                    CastKind::IntToPtr => int(from) && pointer(*to),
                };
                if !fits || *to != wanted {
                    let message = format!(
                        "{kind:?} of {} to {} as {shown}",
                        self.shown(from),
                        self.shown(*to)
                    );
                    self.error(53, message);
                }
            }
            Rvalue::Aggregate(kind, operands) => {
                let mut given = Vec::new();
                for operand in operands {
                    let reads = match operand {
                        Operand::Copy(place) | Operand::Move(place) => place.local == dest.local,
                        Operand::Const(_) => false,
                    };
                    if reads {
                        self.error(
                            57,
                            "an aggregate reads the local that it is put in".to_owned(),
                        );
                    }
                    let Some(ty) = self.operand(func, operand) else {
                        return;
                    };
                    given.push(ty);
                }
                if dest
                    .projections
                    .iter()
                    .any(|step| step.kind == ProjectionKind::Deref)
                {
                    self.error(57, "an aggregate is put behind a pointer".to_owned());
                }
                let types = self.types_of();
                let fits = match (kind, types.get(wanted)) {
                    (AggKind::Tuple, Type::Unit) => given.is_empty(),
                    (AggKind::Tuple, Type::Tuple(fields)) => fields[..] == given[..],
                    (AggKind::Array(of), Type::Array(elem, count)) => {
                        of == elem
                            && *count == given.len() as u64
                            && given.iter().all(|ty| ty == of)
                    }
                    (AggKind::Of(ty), made) if *ty == wanted => match made {
                        Type::Struct(id) => {
                            types.struct_fields(*id).unwrap_or_default() == &given[..]
                        }
                        Type::Option(of) => given[..] == [*of, TypeInterner::BOOL],
                        Type::String => {
                            given.len() == 3
                                && self.is_bytes(given[0])
                                && self.is_u64(given[1])
                                && self.is_bytes(given[2])
                        }
                        Type::Slice(of) => {
                            given.len() == 4
                                && *types.get(given[0]) == Type::Ptr(*of)
                                && self.is_u64(given[1])
                                && self.is_u64(given[2])
                                && self.is_bytes(given[3])
                        }
                        _ => false,
                    },
                    _ => false,
                };
                if !fits {
                    self.error(55, format!("the parts are not those of {shown}"));
                }
            }
        }
    }

    fn unwind(&mut self, func: &Function, block: &BasicBlock, action: UnwindAction) {
        let plain = block.cleanup && !block.catch;
        let quiet = matches!(action, UnwindAction::Terminate | UnwindAction::Unreachable);
        if plain && !quiet {
            self.error(25, "a cleanup block lets a panic go on".to_owned());
        }
        if func.no_unwind && action == UnwindAction::Continue {
            self.error(
                26,
                "a function that does not unwind lets a panic go on".to_owned(),
            );
        }
    }

    fn terminator(&mut self, func: &Function, block: &BasicBlock, taken: &FxHashSet<LocalId>) {
        match &block.term.kind {
            TerminatorKind::Goto(_) | TerminatorKind::Unreachable => {}
            TerminatorKind::Return => {
                if block.cleanup && !block.catch {
                    self.error(25, "a cleanup block returns".to_owned());
                }
            }
            TerminatorKind::Resume => {
                if !Self::unwinds(block) {
                    self.error(25, "resume outside a cleanup block".to_owned());
                }
                if func.no_unwind {
                    self.error(26, "a function that does not unwind resumes".to_owned());
                }
            }
            TerminatorKind::Switch { on, cases, .. } => {
                let Some(ty) = self.operand(func, on) else {
                    return;
                };
                let bits = match self.types_of().get(ty) {
                    Type::Int { bits, .. } => u32::from(*bits),
                    Type::Bool => 1,
                    _ => {
                        return self.error(60, format!("a switch on {}", self.shown(ty)));
                    }
                };
                let mut seen = FxHashSet::default();
                for &(value, _) in cases {
                    self.steps += 1;
                    if bits < 64 && value >> bits != 0 {
                        self.error(27, format!("switch case {value} is too wide"));
                    }
                    if !seen.insert(value) {
                        self.error(27, format!("duplicate switch case {value}"));
                    }
                }
            }
            TerminatorKind::Drop { place, unwind, .. } => {
                self.unwind(func, block, *unwind);
                if self.place(func, place) == Some(TypeInterner::UNIT) {
                    self.error(64, "a drop of what has no value".to_owned());
                }
            }
            TerminatorKind::Assert {
                cond,
                kind,
                args,
                unwind,
                ..
            } => {
                self.unwind(func, block, *unwind);
                if let Some(ty) = self.operand(func, cond)
                    && ty != TypeInterner::BOOL
                {
                    self.error(60, format!("an assert on {}", self.shown(ty)));
                }
                let wanted = match kind {
                    AssertKind::Nil | AssertKind::DivByZero | AssertKind::NegativeShift => 0,
                    _ => 2,
                };
                let mut integers = true;
                for arg in args {
                    let ty = self.operand(func, arg);
                    integers &= ty.is_some_and(|ty| self.is_int(ty));
                }
                if args.len() != wanted || !integers {
                    self.error(65, format!("{kind:?} names {wanted} integers"));
                }
            }
            TerminatorKind::Call {
                callee,
                args,
                dest,
                target,
                unwind,
            } => {
                self.unwind(func, block, *unwind);
                // A function that is named, not one that a value holds.
                let direct = match callee {
                    Operand::Const(Constant {
                        kind: ConstantKind::Func(id),
                        ..
                    }) if (id.into_raw().into_u32() as usize) < self.module.funcs.len() => {
                        Some(&self.module.funcs[*id])
                    }
                    _ => None,
                };
                let quiet = direct.is_some_and(|callee| callee.no_unwind);
                match unwind {
                    UnwindAction::Cleanup(_) if quiet => {
                        self.error(26, "a cleanup for a call that does not unwind".to_owned());
                    }
                    UnwindAction::Unreachable if !quiet => {
                        self.error(26, "the call may unwind".to_owned());
                    }
                    _ => {}
                }
                if target.is_none() && !direct.is_some_and(|callee| callee.no_return) {
                    self.error(28, "the call returns nowhere".to_owned());
                }
                let Some(ty) = self.operand(func, callee) else {
                    return;
                };
                let Type::Func(sig) = self.types_of().get(ty).clone() else {
                    return self.error(61, format!("a call of {}", self.shown(ty)));
                };
                let mut given = Vec::new();
                for arg in args {
                    given.extend(self.operand(func, arg));
                }
                if given.len() == args.len() && given[..] != sig.params[..] {
                    self.error(62, "the arguments are not those of the function".to_owned());
                }
                let scalar = |this: &Self, ty: TypeId| {
                    this.is_int(ty)
                        || this.is_float(ty)
                        || this.is_pointer(ty)
                        || matches!(this.types_of().get(ty), Type::Bool | Type::Unit)
                };
                if sig.cc == CallConv::C && direct.is_none() {
                    let all = given.iter().chain([&sig.ret]).all(|&ty| scalar(self, ty));
                    if !all {
                        self.error(76, "a C function pointer takes scalars only".to_owned());
                    }
                }
                let Some(result) = self.place(func, dest) else {
                    return;
                };
                if result != sig.ret {
                    let message = format!("the result is put in {}", self.shown(result));
                    self.error(63, message);
                }
                // A value of several parts comes back in a slot of its
                // own, which nothing points into.
                if !scalar(self, sig.ret) {
                    let slot = &func.locals[dest.local];
                    let own = dest.projections.is_empty()
                        && slot.kind == LocalKind::Temp
                        && !taken.contains(&dest.local);
                    if !own {
                        self.error(66, "the result needs a temporary of its own".to_owned());
                    }
                }
            }
        }
    }
}
