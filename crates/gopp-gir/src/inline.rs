//! Calls of small functions are replaced by their bodies
//! (pipelines.md §3). A function is small if it is one block that
//! returns: it calls nothing, checks nothing, ends nothing and takes
//! no address, so its body means the same in the function that calls
//! it, and no trace of a panic ever names it. Which functions are
//! small is decided of the module as it is given, so the result does
//! not depend on the order in which the calls are replaced.

use la_arena::RawIdx;

use crate::ir::*;
use crate::types::TypeInterner;

/// How many statements a function may have for its calls to be
/// replaced, unless it is marked `@Inline`.
pub const MAX_STATEMENTS: usize = 12;

/// Whether a call of the function is replaced by its body.
fn small(func: &Function) -> bool {
    let mut blocks = func.blocks.values();
    let (Some(block), None) = (blocks.next(), blocks.next()) else {
        return false;
    };
    let returns = matches!(block.term.kind, TerminatorKind::Return) && !func.no_return;
    let assigned = |stmt: &Statement| matches!(stmt.kind, StatementKind::Assign(_));
    let short = match func.inline {
        Inline::Never => false,
        Inline::Always => true,
        Inline::Default => {
            block.stmts.iter().filter(|stmt| assigned(stmt)).count() <= MAX_STATEMENTS
        }
    };
    let takes_address = |stmt: &Statement| match &stmt.kind {
        StatementKind::Assign(assigned) => matches!(assigned.1, Rvalue::AddrOf(..)),
        _ => false,
    };
    let owns = |local: &Local| local.owned && local.kind == LocalKind::Temp;
    returns
        && short
        && !block.cleanup
        && !block.stmts.iter().any(takes_address)
        && !func.locals.values().any(owns)
}

/// The locals and the scopes of a function that is called, as those
/// that the function that calls it gets for them.
struct Moved {
    /// The local of the caller that stands for the first of the callee.
    locals: u32,
    /// The scope of the caller that stands for the body of the callee;
    /// the scopes of the callee follow it.
    scope: u32,
}

impl Moved {
    fn local(&self, local: LocalId) -> LocalId {
        LocalId::from_raw(RawIdx::from_u32(self.locals + local.into_raw().into_u32()))
    }

    fn scope(&self, scope: ScopeId) -> ScopeId {
        ScopeId(self.scope + scope.0)
    }

    fn place(&self, place: &Place) -> Place {
        let mut moved = place.clone();
        moved.local = self.local(place.local);
        for step in &mut moved.projections {
            if let ProjectionKind::Index(at) = step.kind {
                step.kind = ProjectionKind::Index(self.local(at));
            }
        }
        moved
    }

    fn operand(&self, operand: &Operand) -> Operand {
        match operand {
            Operand::Copy(place) => Operand::Copy(self.place(place)),
            Operand::Move(place) => Operand::Move(self.place(place)),
            Operand::Const(constant) => Operand::Const(*constant),
        }
    }

    fn rvalue(&self, value: &Rvalue) -> Rvalue {
        match value {
            Rvalue::Use(of) => Rvalue::Use(self.operand(of)),
            Rvalue::AddrOf(place, mutability) => Rvalue::AddrOf(self.place(place), *mutability),
            Rvalue::Binary(op, a, b) => Rvalue::Binary(*op, self.operand(a), self.operand(b)),
            Rvalue::Unary(op, of) => Rvalue::Unary(*op, self.operand(of)),
            Rvalue::Cast(kind, of, to) => Rvalue::Cast(*kind, self.operand(of), *to),
            Rvalue::Aggregate(kind, parts) => {
                Rvalue::Aggregate(*kind, parts.iter().map(|part| self.operand(part)).collect())
            }
            Rvalue::Len(place) => Rvalue::Len(self.place(place)),
        }
    }

    fn statement(&self, stmt: &Statement) -> Statement {
        let kind = match &stmt.kind {
            StatementKind::Assign(assigned) => {
                let (dest, value) = &**assigned;
                StatementKind::Assign(Box::new((self.place(dest), self.rvalue(value))))
            }
            StatementKind::StorageLive(local) => StatementKind::StorageLive(self.local(*local)),
            StatementKind::StorageDead(local) => StatementKind::StorageDead(self.local(*local)),
            StatementKind::Nop => StatementKind::Nop,
        };
        Statement {
            kind,
            loc: stmt.loc,
            scope: self.scope(stmt.scope),
            boundary: false,
        }
    }
}

/// Puts the body of `callee`, the function of the number `id`, in the
/// place of the call that ends the block of `caller`.
fn replace(caller: &mut Function, block: BlockId, (id, callee): (FuncId, &Function)) -> u64 {
    let call = &caller.blocks[block].term;
    let TerminatorKind::Call {
        args,
        dest,
        target: Some(next),
        ..
    } = &call.kind
    else {
        return 0;
    };
    let (args, dest, next) = (args.clone(), dest.clone(), *next);
    let (loc, scope) = (call.loc, call.scope);
    // The body is a scope of its own under that of the call, with the
    // scopes of the callee under it.
    caller.scopes.push(DebugScope {
        kind: ScopeKind::Inlined(id),
        parent: scope,
        loc,
    });
    let moved = Moved {
        locals: caller.locals.len() as u32,
        scope: caller.scopes.len() as u32,
    };
    for inner in &callee.scopes {
        caller.scopes.push(DebugScope {
            parent: moved.scope(inner.parent),
            ..*inner
        });
    }
    // The result and the parameters are variables here: they have
    // their values from the statements that follow.
    for local in callee.locals.values() {
        caller.locals.alloc(Local {
            kind: match local.kind {
                LocalKind::Temp => LocalKind::Temp,
                _ => LocalKind::Var,
            },
            scope: moved.scope(local.scope),
            ..local.clone()
        });
    }
    let body = moved.scope(ScopeId(0));
    let crossing = |dest: Place, value: Operand| Statement {
        kind: StatementKind::Assign(Box::new((dest, Rvalue::Use(value)))),
        loc,
        scope: body,
        boundary: true,
    };
    let unit = |local: LocalId| callee.locals[local].ty == TypeInterner::UNIT;
    let mut stmts = Vec::new();
    let params = callee.locals.iter().skip(1).map(|(local, _)| local);
    for (param, arg) in params.zip(args) {
        if !unit(param) {
            stmts.push(crossing(Place::local(moved.local(param)), arg));
        }
    }
    let inner = callee.blocks.values().flat_map(|block| &block.stmts);
    stmts.extend(inner.map(|stmt| moved.statement(stmt)));
    if let Some((result, _)) = callee
        .locals
        .iter()
        .next()
        .filter(|&(local, _)| !unit(local))
    {
        let given = Operand::Copy(Place::local(moved.local(result)));
        stmts.push(crossing(dest, given));
    }
    let steps = stmts.len() as u64 + callee.locals.len() as u64 + callee.scopes.len() as u64;
    let block = &mut caller.blocks[block];
    block.stmts.extend(stmts);
    block.term.kind = TerminatorKind::Goto(next);
    steps
}

/// Takes the blocks away that nothing leads to any more: the cleanup
/// blocks that only calls led to that are replaced. A small function
/// does not unwind, so its body needs none of them.
fn prune(func: &mut Function) -> u64 {
    let count = func.blocks.len();
    let mut reached = vec![false; count];
    let mut rest = vec![0usize];
    while let Some(at) = rest.pop() {
        if at >= count || std::mem::replace(&mut reached[at], true) {
            continue;
        }
        let block = BlockId::from_raw(RawIdx::from_u32(at as u32));
        let targets = func.blocks[block].term.kind.successors();
        rest.extend(
            targets
                .iter()
                .map(|(to, _)| to.into_raw().into_u32() as usize),
        );
    }
    if reached.iter().all(|&reached| reached) {
        return count as u64;
    }
    // The number that each block that stays has after the others go.
    let mut kept = 0;
    let moved: Vec<BlockId> = (reached.iter())
        .map(|&stays| {
            let at = BlockId::from_raw(RawIdx::from_u32(kept));
            kept += u32::from(stays);
            at
        })
        .collect();
    let blocks = std::mem::take(&mut func.blocks);
    for ((_, mut block), stays) in blocks.into_iter().zip(&reached) {
        if *stays {
            (block.term.kind).each_target(|to| *to = moved[to.into_raw().into_u32() as usize]);
            func.blocks.alloc(block);
        }
    }
    2 * count as u64
}

/// Replaces each direct call of a small function of the module by the
/// body of the function. Gives the steps that it took.
pub fn inline_trivial(module: &mut Module) -> u64 {
    let mut steps = 0;
    let bodies: Vec<Option<Function>> = (module.funcs.values())
        .map(|func| {
            steps += 1 + func
                .blocks
                .values()
                .next()
                .map_or(0, |block| block.stmts.len() as u64);
            small(func).then(|| func.clone())
        })
        .collect();
    if bodies.iter().all(Option::is_none) {
        return steps;
    }
    for (_, caller) in module.funcs.iter_mut() {
        let blocks: Vec<BlockId> = caller.blocks.iter().map(|(id, _)| id).collect();
        let mut unwound = false;
        for block in blocks {
            steps += 1;
            let TerminatorKind::Call {
                callee:
                    Operand::Const(Constant {
                        kind: ConstantKind::Func(id),
                        ..
                    }),
                target: Some(_),
                unwind,
                ..
            } = &caller.blocks[block].term.kind
            else {
                continue;
            };
            let (id, cleaned) = (*id, matches!(unwind, UnwindAction::Cleanup(_)));
            if let Some(callee) = &bodies[id.into_raw().into_u32() as usize] {
                steps += replace(caller, block, (id, callee));
                unwound |= cleaned;
            }
        }
        if unwound {
            steps += prune(caller);
        }
    }
    steps
}

#[cfg(test)]
mod tests;
