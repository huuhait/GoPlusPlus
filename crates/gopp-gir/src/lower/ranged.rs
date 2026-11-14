//! A range over a function (RNG-6a).
//!
//! The body of the loop is a function of its own: the function that
//! walks is given it as a function value and calls it for each pass.
//! Its environment leads to the frame of the function with the loop,
//! which has the address of every variable that the body uses and does
//! not declare, as the environment of a function literal that is called
//! where it is written has them (CLO-1).
//!
//! The body returns whether the loop goes on. A `break`, a `return` or
//! a jump to a statement around the loop says in the frame how the loop
//! was left, and for a `return` what is returned; the function with the
//! loop does it when the function that walks has returned.
//!
//! The function that walks may keep the function value, and may not
//! call it when the loop is over, as in Go: the environment has a cell
//! with the state of the loop, which outlives the frame, and the body
//! asks the runtime first whether it may run.

use std::rc::Rc;

use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};
use gopp_sema::{LocalId as CheckedLocalId, LocalKind, Ref};
use gopp_span::Span as SourceSpan;
use gopp_syntax::ast::{BranchKind, ExprId, ExprKind, Ident, StmtId, StmtKind};
use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use super::func::{Built, FuncCx, Target};
use super::glue::{Ends, Glue};
use super::{Job, Shared};
use crate::{
    AggKind, BlockId, CastKind, Constant, ConstantKind, Mutability, Operand, Place, Rvalue, Type,
    TypeId,
};

/// How a loop was left, in the frame: by its end or a `break`, which
/// need nothing more, by a `return` with values or without, or by a
/// jump to the statement with a label, from `FIRST_LABEL` on two for
/// each: its `break` and its `continue`.
const BROKEN: u64 = 1;
const RETURNED: u64 = 2;
const RETURNED_BARE: u64 = 3;
const FIRST_LABEL: u64 = 4;

/// The states of a loop that the body writes; the runtime has the
/// others.
const READY: u64 = 0;
const DONE: u64 = 2;

/// What the function with a loop over a function says of the loop, for
/// the function that is its body.
pub(crate) struct Ranged {
    /// The type of the code of the body.
    pub sig: TypeId,
    /// The variables that the body uses and does not declare, in the
    /// order of the frame.
    pub captured: Vec<CheckedLocalId>,
    /// The function literal that the loop is written in.
    pub within: Option<ExprId>,
    label: Option<Spur>,
    /// The labels of the statements around the loop that a jump in the
    /// body names, each with whether a `break` and a `continue` do.
    labels: Vec<(Spur, bool, bool)>,
    /// The types of the results of the function with the loop, and
    /// whether it can fail.
    parts: Vec<TypeId>,
    fails: bool,
}

/// What the loops over functions of a job use of what is around them.
#[derive(Default)]
pub(crate) struct Named {
    loops: FxHashMap<StmtId, Around>,
    /// The receiver of the function, which `base` names without its
    /// name.
    receiver: Option<CheckedLocalId>,
}

/// What a loop over a function uses of what is around it: the
/// variables that its body names and does not declare, and the labels
/// of the statements around it that a jump in its body names.
#[derive(Default)]
struct Around {
    locals: FxHashSet<CheckedLocalId>,
    jumps: FxHashSet<(Spur, bool)>,
}

/// What the body of a loop over a function has of the function with
/// the loop: the types of its results, and the places for the state of
/// the loop, for how it was left and for what is returned.
pub(crate) struct Leaving {
    pub parts: Vec<TypeId>,
    state: Place,
    exit: Place,
    returned: Option<Place>,
    /// Where the statement is written: what is declared there is the
    /// body's own.
    within: (u32, u32),
}

impl Leaving {
    /// Whether a variable that is declared at `span` is one of the
    /// function with the loop.
    pub fn outside(&self, span: SourceSpan) -> bool {
        span.lo < self.within.0 || span.lo >= self.within.1
    }
}

/// What the function with the loop does when the loop was left by more
/// than its end.
enum Way {
    Returns,
    ReturnsBare,
    Jumps(BranchKind, Spur),
}

impl<'a> FuncCx<'a, '_> {
    /// What the loops over functions of the job use of what is around
    /// them, found for all of them in one walk over the places where
    /// the body names a variable or jumps to a label, in the order of
    /// the source. What a loop uses, every loop around it uses too, up
    /// to the one that declares it: the walk goes outwards from the
    /// innermost loop and stops there, or where it was before.
    fn named(&mut self, job: &Job) -> Rc<Named> {
        let key = std::ptr::from_ref(job);
        if let Some(known) = self.memo.named.get(&key) {
            return known.clone();
        }
        enum Met {
            Loop(u32, StmtId),
            Local(CheckedLocalId),
            Jump(Spur, bool),
        }
        let types = self.shared.types;
        let over_func = |x: ExprId| {
            let ty = self.body.types.get(&x).map(|typed| typed.ty);
            ty.is_some_and(|ty| {
                matches!(&*types.get(types.underlying(ty)), CheckedType::Func { .. })
            })
        };
        let mut met: Vec<(u32, Met)> = Vec::new();
        let mut labels: FxHashMap<Spur, Vec<(u32, u32)>> = FxHashMap::default();
        for &stmt in self.shared.jumps(job) {
            let node = &self.tree.stmts[stmt];
            match &node.kind {
                StmtKind::Range { x, .. } if over_func(*x) => {
                    met.push((node.span.lo, Met::Loop(node.span.hi, stmt)));
                }
                StmtKind::Labeled { label, .. } => {
                    let spans = labels.entry(label.name).or_default();
                    spans.push((node.span.lo, node.span.hi));
                }
                StmtKind::Branch {
                    kind: kind @ (BranchKind::Break | BranchKind::Continue),
                    label: Some(label),
                } => {
                    let continues = *kind == BranchKind::Continue;
                    met.push((node.span.lo, Met::Jump(label.name, continues)));
                }
                _ => {}
            }
        }
        let mut named = Named::default();
        // A function without such a loop is not walked.
        if met.iter().any(|(_, met)| matches!(met, Met::Loop(..))) {
            for (&expr, used) in &self.body.uses {
                if let Ref::Local(local) = used {
                    met.push((self.tree.exprs[expr].span.lo, Met::Local(*local)));
                }
            }
            met.sort_unstable_by_key(|&(at, _)| at);
            let receives = |local: &gopp_sema::Local| local.kind == LocalKind::Receiver;
            let receiver = self.body.locals.iter().position(receives);
            named.receiver = receiver.map(|at| CheckedLocalId(at as u32));
            self.memo.steps += self.body.locals.len() as u64;
        } else {
            met.clear();
        }
        self.memo.steps += met.len() as u64;
        // The loops that are open at a place, the innermost last.
        let mut open: Vec<(u32, u32, StmtId)> = Vec::new();
        for (at, met) in met {
            while open.last().is_some_and(|&(_, end, _)| end <= at) {
                open.pop();
            }
            // Where what is named is declared.
            let declared = match &met {
                Met::Loop(end, stmt) => {
                    open.push((at, *end, *stmt));
                    named.loops.entry(*stmt).or_default();
                    continue;
                }
                Met::Local(local) => {
                    let declared = &self.body.locals[local.0 as usize];
                    let kept = matches!(
                        declared.kind,
                        LocalKind::Var | LocalKind::Param | LocalKind::Result | LocalKind::Receiver
                    );
                    match kept {
                        true => declared.span.lo,
                        false => continue,
                    }
                }
                // The innermost statement with the label around the
                // jump.
                Met::Jump(label, _) => {
                    let spans = labels.get(label).map(Vec::as_slice).unwrap_or_default();
                    let around = spans.iter().rev().find(|&&(lo, hi)| lo <= at && at < hi);
                    match around {
                        Some(&(lo, _)) => lo,
                        None => continue,
                    }
                }
            };
            for &(lo, hi, stmt) in open.iter().rev() {
                self.memo.steps += 1;
                if lo <= declared && declared < hi {
                    break;
                }
                let around = named.loops.entry(stmt).or_default();
                let new = match met {
                    Met::Local(local) => around.locals.insert(local),
                    Met::Jump(label, continues) => around.jumps.insert((label, continues)),
                    Met::Loop(..) => false,
                };
                if !new {
                    break;
                }
            }
        }
        let named = Rc::new(named);
        self.memo.named.insert(key, named.clone());
        named
    }

    /// The variables that the body of the loop uses and does not
    /// declare, of the function with the loop and of the functions
    /// around that; and whether a jump in the body names the label of a
    /// statement around the loop, for its `break` and its `continue`.
    fn loop_uses(
        &mut self,
        job: &Job,
        stmt: StmtId,
    ) -> (Vec<CheckedLocalId>, Vec<(Spur, bool, bool)>) {
        let named = self.named(job);
        let Some(around) = named.loops.get(&stmt) else {
            return (Vec::new(), Vec::new());
        };
        let mut captured: Vec<CheckedLocalId> = around.locals.iter().copied().collect();
        let receiver = named
            .receiver
            .filter(|&local| self.vars_of(local).is_some());
        captured.extend(receiver);
        captured.sort_unstable();
        captured.dedup();
        let jumps = |target: &Target| {
            let label = target.label?;
            let breaks = around.jumps.contains(&(label, false));
            let continues = target.continues.is_some() && around.jumps.contains(&(label, true));
            (breaks || continues).then_some((label, breaks, continues))
        };
        (captured, self.targets.iter().filter_map(jumps).collect())
    }

    /// The types of the values that the function `x` yields, as the
    /// checker has them.
    fn yielded(&self, x: ExprId) -> Vec<CheckedTypeId> {
        let types = self.shared.types;
        let params = |ty: CheckedTypeId| match &*types.get(types.underlying(ty)) {
            CheckedType::Func { params, .. } => Some(params.to_vec()),
            _ => None,
        };
        let walks = self.body.types.get(&x).and_then(|typed| params(typed.ty));
        let yields = walks.and_then(|walks| params(*walks.first()?));
        yields.unwrap_or_default()
    }

    /// The type of the cell of a loop: its count, then the state of the
    /// loop and the address of the frame. And the type of the
    /// environment of the body, which has that cell.
    fn loop_types(&mut self) -> (TypeId, TypeId) {
        let word = self.b.types().int(64, false);
        let bytes = self.bytes_ty();
        let inside = self.b.types().tuple(&[word, bytes]);
        let cell = self.cell_type(inside);
        let drop = self.glue_sig(Glue::Drop);
        let types = self.b.types();
        let to = types.ptr(cell);
        (cell, types.tuple(&[word, word, to, drop]))
    }

    /// The type of the place for what a `return` in the body returns.
    fn returned_type(&mut self, parts: &[TypeId]) -> Option<TypeId> {
        match parts {
            [] => None,
            [one] => Some(*one),
            many => Some(self.b.types().tuple(many)),
        }
    }

    /// `for key, value := range x` over a function `x`: the function
    /// is called with the body of the loop, and what the body left the
    /// loop for is done then.
    pub(super) fn range_func(
        &mut self,
        (stmt, label): (StmtId, Option<Spur>),
        span: SourceSpan,
        lhs: &[ExprId],
        x: ExprId,
    ) -> Built {
        let walked = self.ty_of(x);
        let code = self.code_type(walked);
        let yields = code.and_then(|code| match self.kind(code) {
            Type::Func(sig) => sig.params.get(1).copied(),
            _ => None,
        });
        let sig = yields.and_then(|yields| self.code_type(yields));
        let named = self.owner.and_then(|(job, within)| {
            let symbol = self.memo.range_body(self.shared, job, stmt)?;
            Some((job, within, symbol))
        });
        let (Some(code), Some(sig), Some((job, within, symbol))) = (code, sig, named) else {
            self.unsupported(span, "a range over a function of this type");
            return Ok(());
        };
        if lhs.iter().any(|&left| self.aliased(left).is_some()) {
            self.unsupported(
                span,
                "a range over a function whose variable is the value itself",
            );
            return Ok(());
        }
        if self.views.iter().any(Option::is_some) {
            let what = "a range over a function in a function that returns a view";
            self.unsupported(span, what);
            return Ok(());
        }
        self.open_scope();
        self.at(self.tree.exprs[x].span);
        // The function that walks lives as long as the loop.
        let mark = self.temps.len();
        let walks = self.operand(x)?;
        let walks = self.slot_of(Rvalue::Use(walks))?;
        self.keep_temps(mark)?;
        let word = self.b.types().int(64, false);
        let bytes = self.bytes_ty();
        // How the loop was left, and what a `return` in it returns.
        let parts = self.result_parts();
        let exit = Place::local(self.b.temp(word));
        let none = self.b.const_int(word, 0)?;
        self.b.assign(exit.clone(), Rvalue::Use(none))?;
        let returned = match self.returned_type(&parts) {
            Some(ty) => {
                let place = Place::local(self.b.temp(ty));
                let zero = self.zero(ty);
                self.b.assign(place.clone(), Rvalue::Use(zero))?;
                Some(place)
            }
            None => None,
        };
        // The frame: the variables that the body uses, then those two.
        let (captured, labels) = self.loop_uses(job, stmt);
        let mut frame = self.frame_parts(&captured)?;
        for place in [Some(exit.clone()), returned.clone()].into_iter().flatten() {
            let address = Rvalue::AddrOf(place, Mutability::Mut);
            frame.push(Operand::Copy(Place::local(self.b.assign_temp(address)?)));
        }
        let frame = self.slot_of(Rvalue::Aggregate(AggKind::Tuple, frame))?;
        let frame = self.record(frame)?;
        // The cell of the loop, which the scope of the loop lets go
        // where it is left by any way.
        let (cell_ty, env_ty) = self.loop_types();
        let layout = self.b.types().layout(cell_ty, self.shared.layout);
        let size = self
            .b
            .const_int(word, layout.map_or(0, |layout| layout.size))?;
        let made = self.runtime("_gopp_cell_new", &[word], bytes, vec![size])?;
        let to = self.b.types().ptr(cell_ty);
        let cell = Rvalue::Cast(CastKind::PtrToPtr, made.clone(), to);
        let cell = Place::local(self.b.assign_temp(cell)?);
        let ended = self.glue_sig(Glue::Drop);
        let ended = self.function_named("_gopp_range_end", ended)?;
        self.hold(cell.clone(), Ends::Glue(ended));
        let inside = self.b.deref(&cell)?;
        let inside = self.b.field(&inside, 1)?;
        let at = self.b.field(&inside, 1)?;
        self.b.assign(at, Rvalue::Use(frame))?;
        // The environment of the body has the cell too.
        let one = self.b.const_int(word, 1)?;
        let env = self.runtime("_gopp_env_new", &[word], bytes, vec![one])?;
        let to = self.b.types().ptr(env_ty);
        let typed = Rvalue::Cast(CastKind::PtrToPtr, env.clone(), to);
        let typed = Place::local(self.b.assign_temp(typed)?);
        let slot = self.b.deref(&typed)?;
        let slot = self.b.field(&slot, 2)?;
        self.b.assign(slot, Rvalue::Use(Operand::Copy(cell)))?;
        self.runtime_call("_gopp_cell_retain", &[bytes], vec![made.clone()])?;
        // The body is lowered after this function, with what is known
        // of the loop here.
        let ranged = Ranged {
            sig,
            captured,
            within,
            label,
            labels: labels.clone(),
            parts: parts.clone(),
            fails: self.fails,
        };
        let key = (std::ptr::from_ref(job), stmt);
        if self.memo.ranged.insert(key, Rc::new(ranged)).is_none() {
            self.memo.ranges.push((job, stmt));
        }
        let body = self.function_named(&symbol, sig)?;
        let body = Operand::Const(Constant {
            kind: ConstantKind::Func(body),
            ty: sig,
        });
        let given = self.slot_of(Rvalue::Aggregate(AggKind::Tuple, vec![body, env]))?;
        // The function that walks owns the function value that it is
        // given.
        let before = self.zero(bytes);
        self.call_value(&walks, code, vec![before, Operand::Copy(given)])?;
        self.runtime_call("_gopp_range_exit", &[bytes], vec![made])?;
        // What the body left the loop for.
        let mut ways = vec![(RETURNED_BARE, Way::ReturnsBare)];
        if returned.is_some() {
            ways.push((RETURNED, Way::Returns));
        }
        for (at, &(name, breaks, continues)) in labels.iter().enumerate() {
            let code = FIRST_LABEL + 2 * at as u64;
            if breaks {
                ways.push((code, Way::Jumps(BranchKind::Break, name)));
            }
            if continues {
                ways.push((code + 1, Way::Jumps(BranchKind::Continue, name)));
            }
        }
        let after = self.b.new_block();
        let blocks: Vec<BlockId> = ways.iter().map(|_| self.b.new_block()).collect();
        let codes = ways.iter().zip(&blocks);
        let table = codes.map(|(&(code, _), &block)| (code, block)).collect();
        self.b.switch(Operand::Copy(exit), table, after)?;
        for ((_, way), block) in ways.into_iter().zip(blocks) {
            self.b.switch_to(block);
            match (way, &returned) {
                (Way::Returns, Some(returned)) => {
                    let mut given = Vec::with_capacity(parts.len());
                    for at in 0..parts.len() {
                        let part = match parts.len() {
                            1 => returned.clone(),
                            _ => self.b.field(returned, at as u32)?,
                        };
                        given.push(Rvalue::Use(Operand::Copy(part)));
                    }
                    self.leave(span, Some(given))?;
                }
                (Way::Returns | Way::ReturnsBare, _) => self.leave(span, None)?,
                (Way::Jumps(kind, name), _) => {
                    self.branch(span, kind, Some(Ident { name, span }))?;
                }
            }
        }
        self.b.switch_to(after);
        self.close_scope(span)
    }

    /// The function of the body of a loop over a function: it is given
    /// the values of one pass, and returns whether the loop goes on.
    pub(super) fn ranged_body(&mut self, stmt: StmtId, ranged: &Ranged) -> Built {
        let StmtKind::Range {
            lhs,
            define,
            x,
            body,
        } = &self.tree.stmts[stmt].kind
        else {
            return Ok(());
        };
        let whole = self.tree.stmts[stmt].span;
        self.fails = ranged.fails;
        let entry = self.b.new_block();
        self.b.switch_to(entry);
        self.open_scope();
        self.at(whole);
        // What the body is given is its own: a value that no variable
        // of the clause takes ends with the pass.
        let yielded = self.yielded(*x);
        let taken = |this: &Self, at: usize| {
            lhs.get(at).is_some_and(|&left| {
                let kind = &this.tree.exprs[Shared::unparen(this.tree, left)].kind;
                !matches!(kind, ExprKind::Name(name) if *name == this.shared.blank)
            })
        };
        for (at, &checked) in yielded.iter().enumerate() {
            let given = Place::local(self.b.param(1 + at as u32));
            let of = self.b.place_ty(&given);
            if !taken(self, at)
                && !self.flat(of)
                && let Some(ends) = self.ends_of(Some(checked), whole)?
            {
                self.hold(given, ends);
            }
        }
        // The cell of the loop, which says whether the body may run.
        let word = self.b.types().int(64, false);
        let bytes = self.bytes_ty();
        let (_, env_ty) = self.loop_types();
        let to = self.b.types().ptr(env_ty);
        let given = Operand::Copy(Place::local(self.b.param(0)));
        let env = Rvalue::Cast(CastKind::PtrToPtr, given, to);
        let env = Place::local(self.b.assign_temp(env)?);
        let env = self.b.deref(&env)?;
        let cell = Operand::Copy(self.b.field(&env, 2)?);
        let cell = Place::local(self.b.assign_temp(Rvalue::Use(cell))?);
        let asked = Rvalue::Cast(CastKind::PtrToPtr, Operand::Copy(cell.clone()), bytes);
        let asked = Operand::Copy(Place::local(self.b.assign_temp(asked)?));
        self.runtime_call("_gopp_range_enter", &[bytes], vec![asked])?;
        let inside = self.b.deref(&cell)?;
        let inside = self.b.field(&inside, 1)?;
        let state = self.b.field(&inside, 0)?;
        // The frame of the function with the loop: its variables, and
        // the places for how the loop is left.
        let frame = Operand::Copy(self.b.field(&inside, 1)?);
        let returned = self.returned_type(&ranged.parts);
        let mut more = vec![self.b.types().ptr(word)];
        more.extend(returned.map(|ty| self.b.types().ptr(ty)));
        let frame = self.open_frame(frame, &ranged.captured, &more)?;
        let behind = |this: &mut Self, at: u32| -> Built<Place> {
            let address = Operand::Copy(this.b.field(&frame, at)?);
            let address = Place::local(this.b.assign_temp(Rvalue::Use(address))?);
            this.b.deref(&address)
        };
        let first = 1 + ranged.captured.len() as u32;
        let exit = behind(self, first)?;
        let returned = match returned {
            Some(_) => Some(behind(self, first + 1)?),
            None => None,
        };
        self.ranged = Some(Leaving {
            parts: ranged.parts.clone(),
            state: state.clone(),
            exit,
            returned,
            within: (whole.lo, whole.hi),
        });
        for (at, &left) in lhs.iter().enumerate().take(yielded.len()) {
            let given = Place::local(self.b.param(1 + at as u32));
            self.set_range_var(left, *define, Operand::Copy(given), true)?;
        }
        // A jump out of the body returns with what it is: the statements
        // around the loop first, the loop itself last.
        let mut leaves: Vec<(u64, BlockId)> = Vec::new();
        for (at, &(name, breaks, continues)) in ranged.labels.iter().enumerate() {
            let code = FIRST_LABEL + 2 * at as u64;
            // A `break` that no jump takes goes nowhere.
            let breaks = match breaks {
                true => {
                    let block = self.b.new_block();
                    leaves.push((code, block));
                    block
                }
                false => self.b.new_block(),
            };
            let continues = continues.then(|| self.b.new_block());
            leaves.extend(continues.map(|block| (code + 1, block)));
            self.targets.push(Target {
                label: Some(name),
                breaks,
                continues,
                depth: 0,
                next: None,
            });
        }
        let [breaks, continues] = [(); 2].map(|()| self.b.new_block());
        leaves.push((BROKEN, breaks));
        self.targets.push(Target {
            label: ranged.label,
            breaks,
            continues: Some(continues),
            depth: 0,
            next: None,
        });
        self.block(body)?;
        self.at_end(body.span);
        self.exit_scopes(0)?;
        self.b.goto(continues)?;
        // The next pass.
        self.b.switch_to(continues);
        let ready = self.b.const_int(word, READY)?;
        self.b.assign(state, Rvalue::Use(ready))?;
        let goes_on = self.b.const_bool(true);
        let result = Place::local(self.b.result());
        self.b.assign(result, Rvalue::Use(goes_on))?;
        self.b.ret()?;
        for (code, block) in leaves {
            self.b.switch_to(block);
            self.left(code)?;
        }
        Ok(())
    }

    /// The body says how the loop is left, and that it is over.
    fn left(&mut self, code: u64) -> Built {
        let Some(ranged) = &self.ranged else {
            return Ok(());
        };
        let (exit, state) = (ranged.exit.clone(), ranged.state.clone());
        let word = self.b.types().int(64, false);
        let code = self.b.const_int(word, code)?;
        self.b.assign(exit, Rvalue::Use(code))?;
        let done = self.b.const_int(word, DONE)?;
        self.b.assign(state, Rvalue::Use(done))?;
        let over = self.b.const_bool(false);
        let result = Place::local(self.b.result());
        self.b.assign(result, Rvalue::Use(over))?;
        self.b.ret()
    }

    /// A `return` in the body of a loop over a function: the function
    /// with the loop returns the values, or what its results have, when
    /// the function that walks has returned.
    pub(super) fn leave_loop(&mut self, span: SourceSpan, given: Option<Vec<Rvalue>>) -> Built {
        let Some(ranged) = &self.ranged else {
            return Ok(());
        };
        let (returned, count) = (ranged.returned.clone(), ranged.parts.len());
        let code = match given {
            Some(values) => {
                for (at, value) in values.into_iter().enumerate() {
                    let Some(returned) = &returned else {
                        break;
                    };
                    let part = match count {
                        1 => returned.clone(),
                        _ => self.b.field(returned, at as u32)?,
                    };
                    let value = self.operand_of(value)?;
                    self.b.assign(part, Rvalue::Use(value))?;
                }
                RETURNED
            }
            None => RETURNED_BARE,
        };
        self.at(span);
        self.drop_temps(0)?;
        self.exit_scopes(0)?;
        self.left(code)?;
        self.dead();
        Ok(())
    }
}
