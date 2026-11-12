//! The one way to make the body of a function. The builder works out the
//! type of every place and every rvalue, and refuses what has none
//! (gir.md §1); the verifier checks the same types on its own.

use la_arena::{Arena, Idx, RawIdx};
use smallvec::SmallVec;

use crate::ir::*;
use crate::types::{Type, TypeId, TypeInterner};

/// What a body cannot be made of.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum BuildError {
    #[error("{0} is no function type")]
    NotAFunction(String),
    #[error("no block is being written")]
    NoBlock,
    #[error("the block bb{0} has no end")]
    Unterminated(u32),
    #[error("{what}: a {found} where a {wanted} is needed")]
    Mismatch {
        what: &'static str,
        found: String,
        wanted: String,
    },
    #[error("{what} of a {on}")]
    Invalid { what: &'static str, on: String },
    #[error("{given} values for {wanted}")]
    Count { given: usize, wanted: usize },
}

/// A block while it is written: its end comes last.
struct Open {
    stmts: Vec<Statement>,
    term: Option<Terminator>,
    cleanup: bool,
    catch: bool,
    cold: bool,
}

pub struct FuncBuilder<'m> {
    module: &'m mut Module,
    func: Function,
    blocks: Vec<Open>,
    current: Option<usize>,
    /// The scopes that the statements are written in, the innermost
    /// last.
    scopes: Vec<ScopeId>,
    loc: Span,
    /// The blocks in the order in which they were chosen to be written.
    written: Vec<usize>,
}

fn block_id(at: usize) -> BlockId {
    Idx::from_raw(RawIdx::from_u32(at as u32))
}

impl<'m> FuncBuilder<'m> {
    /// A function of the name with the signature `sig`: its result is
    /// local 0, its parameters the locals after it. Without a block it
    /// is a declaration.
    pub fn new(
        module: &'m mut Module,
        name: &str,
        sig: TypeId,
        link: Linkage,
        loc: Span,
    ) -> Result<Self, BuildError> {
        let Type::Func(func) = module.types.get(sig).clone() else {
            return Err(BuildError::NotAFunction(module.types.display(sig)));
        };
        let mut locals = Arena::new();
        let mut local = |kind: LocalKind, ty: TypeId| {
            locals.alloc(Local {
                kind,
                ty,
                name: None,
                mutable: true,
                owned: false,
                loc,
                scope: ScopeId(0),
            });
        };
        local(LocalKind::Return, func.ret);
        for &param in func.params.iter() {
            local(LocalKind::Arg, param);
        }
        Ok(Self {
            module,
            func: Function {
                name: name.to_owned(),
                sig,
                link,
                cc: func.cc,
                no_unwind: false,
                no_return: false,
                inline: Inline::Default,
                loc,
                locals,
                blocks: Arena::new(),
                scopes: Vec::new(),
            },
            blocks: Vec::new(),
            current: None,
            scopes: Vec::new(),
            loc,
            written: Vec::new(),
        })
    }

    pub fn types(&mut self) -> &mut TypeInterner {
        &mut self.module.types
    }

    /// The module that the function is built in, for what the body
    /// needs of it: a function that it calls, a global that it reads.
    pub fn module(&mut self) -> &mut Module {
        self.module
    }

    /// Whether statements can be written: a block is chosen and has no
    /// end yet.
    pub fn is_open(&self) -> bool {
        self.current
            .is_some_and(|current| self.blocks[current].term.is_none())
    }

    pub fn set_no_unwind(&mut self) {
        self.func.no_unwind = true;
    }

    pub fn set_no_return(&mut self) {
        self.func.no_return = true;
    }

    pub fn set_inline(&mut self, inline: Inline) {
        self.func.inline = inline;
    }

    /// The place of the result.
    pub fn result(&self) -> LocalId {
        Idx::from_raw(RawIdx::from_u32(0))
    }

    /// The parameter `at`, the first being 0.
    pub fn param(&self, at: u32) -> LocalId {
        Idx::from_raw(RawIdx::from_u32(at + 1))
    }

    fn scope(&self) -> ScopeId {
        self.scopes.last().copied().unwrap_or_default()
    }

    /// A variable of the program.
    pub fn var(&mut self, name: &str, ty: TypeId, mutable: bool) -> LocalId {
        let scope = self.scope();
        self.func.locals.alloc(Local {
            kind: LocalKind::Var,
            ty,
            name: Some(name.to_owned()),
            mutable,
            owned: false,
            loc: self.loc,
            scope,
        })
    }

    /// A slot for a value between two steps.
    pub fn temp(&mut self, ty: TypeId) -> LocalId {
        let scope = self.scope();
        self.func.locals.alloc(Local {
            kind: LocalKind::Temp,
            ty,
            name: None,
            mutable: true,
            owned: false,
            loc: self.loc,
            scope,
        })
    }

    pub fn local_kind(&self, local: LocalId) -> LocalKind {
        self.func.locals[local].kind
    }

    pub fn set_name(&mut self, local: LocalId, name: &str) {
        self.func.locals[local].name = Some(name.to_owned());
    }

    pub fn set_owned(&mut self, local: LocalId) {
        self.func.locals[local].owned = true;
    }

    /// Where in the source what is written next comes from.
    pub fn set_loc(&mut self, loc: Span) {
        self.loc = loc;
    }

    pub fn loc(&self) -> Span {
        self.loc
    }

    /// Opens a debug scope inside the one that is open.
    pub fn push_scope(&mut self, kind: ScopeKind, loc: Span) -> ScopeId {
        let parent = self.scope();
        self.func.scopes.push(DebugScope { kind, parent, loc });
        let scope = ScopeId(self.func.scopes.len() as u32);
        self.scopes.push(scope);
        scope
    }

    pub fn pop_scope(&mut self) {
        self.scopes.pop();
    }

    /// A new block, which is written after `switch_to`.
    pub fn new_block(&mut self) -> BlockId {
        self.blocks.push(Open {
            stmts: Vec::new(),
            term: None,
            cleanup: false,
            catch: false,
            cold: false,
        });
        block_id(self.blocks.len() - 1)
    }

    pub fn switch_to(&mut self, block: BlockId) {
        let at = block.into_raw().into_u32() as usize;
        self.current = Some(at);
        self.written.push(at);
    }

    pub fn set_cleanup(&mut self, block: BlockId) {
        self.blocks[block.into_raw().into_u32() as usize].cleanup = true;
    }

    /// A cleanup block that may stop the panic.
    pub fn set_catch(&mut self, block: BlockId) {
        let block = &mut self.blocks[block.into_raw().into_u32() as usize];
        (block.cleanup, block.catch) = (true, true);
    }

    pub fn set_cold(&mut self, block: BlockId) {
        self.blocks[block.into_raw().into_u32() as usize].cold = true;
    }

    // ---- types

    fn shown(&self, ty: TypeId) -> String {
        self.module.types.display(ty)
    }

    fn mismatch(&self, what: &'static str, found: TypeId, wanted: TypeId) -> BuildError {
        BuildError::Mismatch {
            what,
            found: self.shown(found),
            wanted: self.shown(wanted),
        }
    }

    fn invalid(&self, what: &'static str, on: TypeId) -> BuildError {
        BuildError::Invalid {
            what,
            on: self.shown(on),
        }
    }

    pub fn place_ty(&self, place: &Place) -> TypeId {
        match place.projections.last() {
            Some(last) => last.ty,
            None => self.func.locals[place.local].ty,
        }
    }

    pub fn operand_ty(&self, operand: &Operand) -> TypeId {
        match operand {
            Operand::Copy(place) | Operand::Move(place) => self.place_ty(place),
            Operand::Const(constant) => constant.ty,
        }
    }

    fn with(&self, place: &Place, kind: ProjectionKind, ty: TypeId) -> Place {
        let mut projections: SmallVec<[Projection; 2]> = place.projections.clone();
        projections.push(Projection { kind, ty });
        Place {
            local: place.local,
            projections,
        }
    }

    /// What the pointer `place` points to.
    pub fn deref(&self, place: &Place) -> Result<Place, BuildError> {
        let ty = self.place_ty(place);
        match self.module.types.get(ty) {
            Type::Ptr(to) => Ok(self.with(place, ProjectionKind::Deref, *to)),
            _ => Err(self.invalid("deref", ty)),
        }
    }

    /// The field `at` of a string, a slice, a tuple, an option or a
    /// struct.
    pub fn field(&mut self, place: &Place, at: u32) -> Result<Place, BuildError> {
        let ty = self.place_ty(place);
        let fields = self.module.types.fields(ty);
        match fields.as_ref().and_then(|fields| fields.get(at as usize)) {
            Some(&field) => Ok(self.with(place, ProjectionKind::Field(at), field)),
            None => Err(self.invalid("field", ty)),
        }
    }

    /// The type of the elements of an array, a slice or a string.
    fn element(&mut self, ty: TypeId) -> Option<TypeId> {
        match self.module.types.get(ty) {
            Type::Array(of, _) | Type::Slice(of) => Some(*of),
            Type::String => Some(self.module.types.int(8, false)),
            _ => None,
        }
    }

    /// The element at the index that the integer local `at` holds.
    pub fn index(&mut self, place: &Place, at: LocalId) -> Result<Place, BuildError> {
        let ty = self.place_ty(place);
        let index = self.func.locals[at].ty;
        if !matches!(self.module.types.get(index), Type::Int { .. }) {
            return Err(self.invalid("index by", index));
        }
        match self.element(ty) {
            Some(element) => Ok(self.with(place, ProjectionKind::Index(at), element)),
            None => Err(self.invalid("index", ty)),
        }
    }

    /// The element `at` of an array.
    pub fn const_index(&self, place: &Place, at: u64) -> Result<Place, BuildError> {
        let ty = self.place_ty(place);
        match self.module.types.get(ty) {
            Type::Array(of, count) if at < *count => {
                Ok(self.with(place, ProjectionKind::ConstIndex(at), *of))
            }
            _ => Err(self.invalid("constant index", ty)),
        }
    }

    /// The type of the value that the rvalue gives.
    pub fn rvalue_ty(&mut self, rvalue: &Rvalue) -> Result<TypeId, BuildError> {
        let is = |this: &Self, ty: TypeId, int: bool, float: bool, other: bool| match this
            .module
            .types
            .get(ty)
        {
            Type::Int { .. } => int,
            Type::Float { .. } => float,
            Type::Bool | Type::Ptr(_) | Type::Func(_) => other,
            _ => false,
        };
        match rvalue {
            Rvalue::Use(operand) => Ok(self.operand_ty(operand)),
            Rvalue::AddrOf(place, _) => {
                let ty = self.place_ty(place);
                Ok(self.module.types.ptr(ty))
            }
            Rvalue::Len(place) => {
                let ty = self.place_ty(place);
                match self.module.types.get(ty) {
                    Type::String | Type::Slice(_) | Type::Array(..) => {
                        Ok(self.module.types.int(64, false))
                    }
                    _ => Err(self.invalid("len", ty)),
                }
            }
            Rvalue::Unary(op, operand) => {
                let ty = self.operand_ty(operand);
                let fits = match op {
                    UnOp::Neg => is(self, ty, true, true, false),
                    UnOp::BitNot => is(self, ty, true, false, false),
                    UnOp::Not => ty == TypeInterner::BOOL,
                };
                match fits {
                    true => Ok(ty),
                    false => Err(self.invalid("unary operator", ty)),
                }
            }
            Rvalue::Binary(op, left, right) => {
                let (ty, other) = (self.operand_ty(left), self.operand_ty(right));
                use BinOp::*;
                // The count of a shift is any integer.
                if matches!(op, Shl | Shr) {
                    let fits =
                        is(self, ty, true, false, false) && is(self, other, true, false, false);
                    return match fits {
                        true => Ok(ty),
                        false => Err(self.invalid("shift", ty)),
                    };
                }
                if ty != other {
                    return Err(self.mismatch("binary operator", other, ty));
                }
                let (fits, result) = match op {
                    Add | Sub | Mul | Div => (is(self, ty, true, true, false), ty),
                    Rem | BitAnd | BitOr | BitXor | BitClear => {
                        (is(self, ty, true, false, false), ty)
                    }
                    Eq | Ne => (is(self, ty, true, true, true), TypeInterner::BOOL),
                    _ => (is(self, ty, true, true, false), TypeInterner::BOOL),
                };
                match fits {
                    true => Ok(result),
                    false => Err(self.invalid("binary operator", ty)),
                }
            }
            Rvalue::Cast(kind, operand, to) => {
                let from = self.operand_ty(operand);
                let types = &self.module.types;
                let (int, float, ptr) = (
                    |ty| matches!(types.get(ty), Type::Int { .. }),
                    |ty| matches!(types.get(ty), Type::Float { .. }),
                    |ty| matches!(types.get(ty), Type::Ptr(_) | Type::Func(_)),
                );
                let fits = match kind {
                    CastKind::IntToInt => int(from) && int(*to),
                    CastKind::IntToFloat => int(from) && float(*to),
                    CastKind::FloatToInt => float(from) && int(*to),
                    CastKind::FloatToFloat => float(from) && float(*to),
                    CastKind::PtrToPtr => ptr(from) && ptr(*to),
                    CastKind::PtrToInt => ptr(from) && int(*to),
                    CastKind::IntToPtr => int(from) && ptr(*to),
                };
                match fits {
                    true => Ok(*to),
                    false => Err(self.mismatch("cast", from, *to)),
                }
            }
            Rvalue::Aggregate(kind, operands) => {
                let given: Vec<TypeId> = operands.iter().map(|op| self.operand_ty(op)).collect();
                let (ty, wanted) = match kind {
                    AggKind::Tuple => return Ok(self.module.types.tuple(&given)),
                    AggKind::Array(of) => {
                        let ty = Type::Array(*of, given.len() as u64);
                        (self.module.types.intern(ty), vec![*of; given.len()])
                    }
                    AggKind::Of(ty) => match self.module.types.fields(*ty) {
                        Some(fields) => (*ty, fields),
                        None => return Err(self.invalid("aggregate", *ty)),
                    },
                };
                self.same_types("aggregate", &given, &wanted)?;
                Ok(ty)
            }
        }
    }

    fn same_types(
        &self,
        what: &'static str,
        given: &[TypeId],
        wanted: &[TypeId],
    ) -> Result<(), BuildError> {
        if given.len() != wanted.len() {
            return Err(BuildError::Count {
                given: given.len(),
                wanted: wanted.len(),
            });
        }
        match given
            .iter()
            .zip(wanted)
            .find(|(given, wanted)| given != wanted)
        {
            Some((&given, &wanted)) => Err(self.mismatch(what, given, wanted)),
            None => Ok(()),
        }
    }

    // ---- constants

    /// The integer `value` of the type `ty`, cut to its width.
    pub fn const_int(&self, ty: TypeId, value: u64) -> Result<Operand, BuildError> {
        let Type::Int { bits, .. } = *self.module.types.get(ty) else {
            return Err(self.invalid("integer constant", ty));
        };
        let value = match bits {
            64 => value,
            bits => value & ((1 << bits) - 1),
        };
        Ok(Operand::Const(Constant {
            kind: ConstantKind::Int(value),
            ty,
        }))
    }

    pub fn const_bool(&self, value: bool) -> Operand {
        Operand::Const(Constant {
            kind: ConstantKind::Bool(value),
            ty: TypeInterner::BOOL,
        })
    }

    /// A string of the bytes, which the module keeps.
    pub fn const_str(&mut self, bytes: &[u8]) -> Operand {
        self.module.strings.push(bytes.into());
        let ty = self.module.types.intern(Type::String);
        Operand::Const(Constant {
            kind: ConstantKind::Str(self.module.strings.len() as u32 - 1),
            ty,
        })
    }

    /// The function `func` as a value.
    pub fn const_func(&self, func: FuncId) -> Operand {
        Operand::Const(Constant {
            kind: ConstantKind::Func(func),
            ty: self.module.funcs[func].sig,
        })
    }

    // ---- statements

    fn open(&mut self) -> Result<&mut Open, BuildError> {
        let current = self.current.ok_or(BuildError::NoBlock)?;
        match &mut self.blocks[current] {
            block if block.term.is_none() => Ok(block),
            _ => Err(BuildError::NoBlock),
        }
    }

    fn push(&mut self, kind: StatementKind) -> Result<(), BuildError> {
        let (loc, scope) = (self.loc, self.scope());
        self.open()?.stmts.push(Statement { kind, loc, scope });
        Ok(())
    }

    /// `dest = rvalue`: the place takes a value of its own type.
    pub fn assign(&mut self, dest: Place, rvalue: Rvalue) -> Result<(), BuildError> {
        let (given, wanted) = (self.rvalue_ty(&rvalue)?, self.place_ty(&dest));
        if given != wanted {
            return Err(self.mismatch("assignment", given, wanted));
        }
        self.push(StatementKind::Assign(Box::new((dest, rvalue))))
    }

    /// The value of the rvalue in a new slot of its type.
    pub fn assign_temp(&mut self, rvalue: Rvalue) -> Result<LocalId, BuildError> {
        let ty = self.rvalue_ty(&rvalue)?;
        let temp = self.temp(ty);
        self.push(StatementKind::Assign(Box::new((
            Place::local(temp),
            rvalue,
        ))))?;
        Ok(temp)
    }

    pub fn storage_live(&mut self, local: LocalId) -> Result<(), BuildError> {
        self.push(StatementKind::StorageLive(local))
    }

    pub fn storage_dead(&mut self, local: LocalId) -> Result<(), BuildError> {
        self.push(StatementKind::StorageDead(local))
    }

    // ---- terminators

    fn end(&mut self, kind: TerminatorKind) -> Result<(), BuildError> {
        let (loc, scope) = (self.loc, self.scope());
        self.open()?.term = Some(Terminator { kind, loc, scope });
        self.current = None;
        Ok(())
    }

    pub fn goto(&mut self, target: BlockId) -> Result<(), BuildError> {
        self.end(TerminatorKind::Goto(target))
    }

    /// Goes where the value of `on`, an integer or a boolean, leads.
    pub fn switch(
        &mut self,
        on: Operand,
        cases: Vec<(u64, BlockId)>,
        otherwise: BlockId,
    ) -> Result<(), BuildError> {
        let ty = self.operand_ty(&on);
        if !matches!(self.module.types.get(ty), Type::Int { .. } | Type::Bool) {
            return Err(self.invalid("switch", ty));
        }
        self.end(TerminatorKind::Switch {
            on,
            cases,
            otherwise,
        })
    }

    pub fn ret(&mut self) -> Result<(), BuildError> {
        self.end(TerminatorKind::Return)
    }

    pub fn unreachable(&mut self) -> Result<(), BuildError> {
        self.end(TerminatorKind::Unreachable)
    }

    pub fn resume(&mut self) -> Result<(), BuildError> {
        self.end(TerminatorKind::Resume)
    }

    /// Calls `callee` with the arguments and puts its result in `dest`.
    pub fn call(
        &mut self,
        callee: Operand,
        args: Vec<Operand>,
        dest: Place,
        target: Option<BlockId>,
        unwind: UnwindAction,
    ) -> Result<(), BuildError> {
        let ty = self.operand_ty(&callee);
        let Type::Func(func) = self.module.types.get(ty).clone() else {
            return Err(BuildError::NotAFunction(self.shown(ty)));
        };
        let given: Vec<TypeId> = args.iter().map(|arg| self.operand_ty(arg)).collect();
        self.same_types("argument", &given, &func.params)?;
        let result = self.place_ty(&dest);
        if result != func.ret {
            return Err(self.mismatch("result", func.ret, result));
        }
        self.end(TerminatorKind::Call {
            callee,
            args,
            dest,
            target,
            unwind,
        })
    }

    pub fn drop(
        &mut self,
        place: Place,
        target: BlockId,
        unwind: UnwindAction,
    ) -> Result<(), BuildError> {
        self.end(TerminatorKind::Drop {
            place,
            target,
            unwind,
        })
    }

    /// Goes on to `target` if `cond` is `expected`, and panics otherwise.
    pub fn assert(
        &mut self,
        cond: Operand,
        expected: bool,
        (kind, args): (AssertKind, Vec<Operand>),
        target: BlockId,
        unwind: UnwindAction,
    ) -> Result<(), BuildError> {
        let ty = self.operand_ty(&cond);
        if ty != TypeInterner::BOOL {
            return Err(self.mismatch("assert", ty, TypeInterner::BOOL));
        }
        self.end(TerminatorKind::Assert {
            cond,
            expected,
            kind,
            args,
            target,
            unwind,
        })
    }

    /// Takes away the blocks that nothing leads to from the first; such
    /// a block needs no end. A block that only goes on to another is
    /// passed by. The others come in the order in which they were
    /// first written.
    pub fn drop_unreached(&mut self) {
        let count = self.blocks.len();
        // Where each block leads at once if it is empty; a ring of
        // empty blocks is left as it is.
        let next = |blocks: &[Open], at: usize| match &blocks[at] {
            Open {
                stmts,
                term: Some(term),
                cleanup: false,
                catch: false,
                ..
            } if stmts.is_empty() => match term.kind {
                TerminatorKind::Goto(to) => Some(to.into_raw().into_u32() as usize),
                _ => None,
            },
            _ => None,
        };
        let mut ends: Vec<Option<usize>> = vec![None; count];
        for start in 0..count {
            let mut path = Vec::new();
            let mut at = start;
            let end = loop {
                if let Some(known) = ends[at] {
                    break known;
                }
                match next(&self.blocks, at) {
                    Some(to) if to < count && !path.contains(&at) && path.len() < count => {
                        path.push(at);
                        at = to;
                    }
                    _ => break at,
                }
            };
            ends[at] = Some(end);
            for passed in path {
                ends[passed] = Some(end);
            }
        }
        for block in &mut self.blocks {
            if let Some(term) = &mut block.term {
                term.kind.each_target(|to| {
                    let at = to.into_raw().into_u32() as usize;
                    if let Some(Some(end)) = ends.get(at) {
                        *to = block_id(*end);
                    }
                });
            }
        }
        let mut reached = vec![false; count];
        let mut rest = vec![0];
        while let Some(at) = rest.pop() {
            if at >= count || std::mem::replace(&mut reached[at], true) {
                continue;
            }
            if let Some(term) = &self.blocks[at].term {
                let targets = term.kind.successors();
                rest.extend(
                    targets
                        .iter()
                        .map(|(to, _)| to.into_raw().into_u32() as usize),
                );
            }
        }
        let written = std::mem::take(&mut self.written);
        let mut placed = vec![false; count];
        let mut order = Vec::with_capacity(count);
        for at in std::iter::once(0).chain(written).chain(0..count) {
            if at < count && reached[at] && !std::mem::replace(&mut placed[at], true) {
                order.push(at);
            }
        }
        let mut new = vec![block_id(0); count];
        for (to, &from) in order.iter().enumerate() {
            new[from] = block_id(to);
        }
        let mut old: Vec<Option<Open>> = std::mem::take(&mut self.blocks)
            .into_iter()
            .map(Some)
            .collect();
        for from in order {
            let Some(mut block) = old[from].take() else {
                continue;
            };
            if let Some(term) = &mut block.term {
                term.kind
                    .each_target(|to| *to = new[to.into_raw().into_u32() as usize]);
            }
            self.blocks.push(block);
        }
        self.current = None;
    }

    /// The function in its module: every block has its end.
    pub fn finish(mut self) -> Result<FuncId, BuildError> {
        // A declaration has no locals: its signature says all.
        if self.blocks.is_empty() {
            self.func.locals = Arena::new();
        }
        for (at, block) in self.blocks.into_iter().enumerate() {
            let Some(term) = block.term else {
                return Err(BuildError::Unterminated(at as u32));
            };
            self.func.blocks.alloc(BasicBlock {
                stmts: block.stmts,
                term,
                cleanup: block.cleanup,
                catch: block.catch,
                cold: block.cold,
            });
        }
        Ok(self.module.funcs.alloc(self.func))
    }
}
