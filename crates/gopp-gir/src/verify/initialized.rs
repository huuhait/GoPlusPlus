//! Rule `V085`: a temporary has a value wherever it is read.

use rustc_hash::{FxHashMap, FxHashSet};

use super::{Verifier, raw};
use crate::ir::*;

/// How many edges the search for the value of a temporary walks back
/// from one read. A stretch of blocks that one way leads through is no
/// edge.
const SEARCH_AT_MOST: usize = 32;

const NONE: u32 = u32::MAX;

/// The blocks that a walk from the first block reaches.
pub(super) struct Graph {
    /// The blocks in the order in which the walk meets them.
    pub(super) order: Vec<u32>,
    /// Where each block is in `order`; `NONE` if the walk does not meet
    /// it.
    pub(super) number: Vec<u32>,
    /// For each place in `order`, the place of the block from which the
    /// walk met it.
    parent: Vec<u32>,
    /// The blocks that lead to each block, each once.
    pub(super) preds: Vec<Vec<u32>>,
}

impl Graph {
    /// `succs` has the blocks that each block leads to.
    pub(super) fn walk(succs: &[Vec<u32>], steps: &mut u64) -> Self {
        let count = succs.len();
        let mut graph = Self {
            order: Vec::with_capacity(count),
            number: vec![NONE; count],
            parent: Vec::with_capacity(count),
            preds: vec![Vec::new(); count],
        };
        let mut rest = vec![(0, NONE)];
        while let Some((at, from)) = rest.pop() {
            *steps += 1;
            if graph.number[at as usize] != NONE {
                continue;
            }
            let number = graph.order.len() as u32;
            graph.number[at as usize] = number;
            graph.order.push(at);
            graph.parent.push(from);
            for &to in &succs[at as usize] {
                *steps += 1;
                let preds = &mut graph.preds[to as usize];
                // The edges of one block come one after another.
                if preds.last() != Some(&at) {
                    preds.push(at);
                }
                rest.push((to, number));
            }
        }
        graph
    }

    /// For each place in `order`, the place of the block that every way
    /// to it goes through last (Lengauer and Tarjan).
    pub(super) fn dominators(&self, steps: &mut u64) -> Vec<u32> {
        let count = self.order.len();
        let mut semi: Vec<u32> = (0..count as u32).collect();
        let mut forest = Forest {
            ancestor: vec![NONE; count],
            label: semi.clone(),
            path: Vec::new(),
        };
        let mut idom = vec![0; count];
        let mut bucket: Vec<Vec<u32>> = vec![Vec::new(); count];
        for at in (1..count).rev() {
            for &pred in &self.preds[self.order[at] as usize] {
                *steps += 1;
                let least = forest.eval(self.number[pred as usize], &semi, steps);
                semi[at] = semi[at].min(semi[least as usize]);
            }
            bucket[semi[at] as usize].push(at as u32);
            let parent = self.parent[at];
            forest.ancestor[at] = parent;
            for below in std::mem::take(&mut bucket[parent as usize]) {
                *steps += 1;
                let least = forest.eval(below, &semi, steps);
                idom[below as usize] = match semi[least as usize] < semi[below as usize] {
                    true => least,
                    false => parent,
                };
            }
        }
        for at in 1..count {
            if idom[at] != semi[at] {
                idom[at] = idom[idom[at] as usize];
            }
        }
        idom
    }

    /// When a walk down the tree of `idom` enters and leaves each place
    /// in `order`: one block is above another if its two numbers are
    /// around the other's.
    fn intervals(&self, idom: &[u32]) -> (Vec<u32>, Vec<u32>) {
        let count = self.order.len();
        let mut children: Vec<Vec<u32>> = vec![Vec::new(); count];
        for at in 1..count {
            children[idom[at] as usize].push(at as u32);
        }
        let (mut enter, mut leave) = (vec![0; count], vec![0; count]);
        let mut clock = 0;
        let mut rest = vec![(0, false)];
        while let Some((at, leaving)) = rest.pop() {
            clock += 1;
            if leaving {
                leave[at as usize] = clock;
                continue;
            }
            enter[at as usize] = clock;
            rest.push((at, true));
            rest.extend(children[at as usize].iter().map(|&child| (child, false)));
        }
        (enter, leave)
    }

    /// The first block on the way back from `from` that more than one
    /// block leads to, or the first block of all: no block between
    /// them can be come to but through it.
    fn joined(&self, tops: &mut [u32], from: u32, steps: &mut u64) -> u32 {
        let mut at = from;
        let mut path = Vec::new();
        while at != 0 && tops[at as usize] == NONE && self.preds[at as usize].len() == 1 {
            path.push(at);
            at = self.preds[at as usize][0];
        }
        if tops[at as usize] == NONE {
            tops[at as usize] = at;
        }
        let top = tops[at as usize];
        for on in path {
            *steps += 1;
            tops[on as usize] = top;
        }
        top
    }
}

/// The blocks that are linked so far, each under the one that the walk
/// met it from.
struct Forest {
    ancestor: Vec<u32>,
    /// The block with the least `semi` on the way up from each block,
    /// as far as the way is folded.
    label: Vec<u32>,
    path: Vec<u32>,
}

impl Forest {
    /// The block with the least `semi` on the way up from `at`, which
    /// is folded so that the next ask is short.
    fn eval(&mut self, at: u32, semi: &[u32], steps: &mut u64) -> u32 {
        if self.ancestor[at as usize] == NONE {
            return at;
        }
        let mut up = at;
        while self.ancestor[self.ancestor[up as usize] as usize] != NONE {
            self.path.push(up);
            up = self.ancestor[up as usize];
        }
        while let Some(on) = self.path.pop() {
            *steps += 1;
            let above = self.ancestor[on as usize] as usize;
            if semi[self.label[above] as usize] < semi[self.label[on as usize] as usize] {
                self.label[on as usize] = self.label[above];
            }
            self.ancestor[on as usize] = self.ancestor[above];
        }
        self.label[at as usize]
    }
}

/// The local that a call gives a value, and the block where it has it:
/// a call that unwinds gives none.
fn returned(block: &BasicBlock) -> Option<(LocalId, u32)> {
    match &block.term.kind {
        TerminatorKind::Call {
            dest,
            target: Some(target),
            unwind,
            ..
        } if dest.projections.is_empty() && *unwind != UnwindAction::Cleanup(*target) => {
            Some((dest.local, raw(*target)))
        }
        _ => None,
    }
}

/// The locals whose value a use of the place reads: its own, and those
/// that it is indexed by.
fn place_reads(place: &Place, reads: &mut Vec<LocalId>) {
    reads.push(place.local);
    for step in &place.projections {
        if let ProjectionKind::Index(at) = step.kind {
            reads.push(at);
        }
    }
}

fn operand_reads(operand: &Operand, reads: &mut Vec<LocalId>) {
    if let Operand::Copy(place) | Operand::Move(place) = operand {
        place_reads(place, reads);
    }
}

fn rvalue_reads(rvalue: &Rvalue, reads: &mut Vec<LocalId>) {
    match rvalue {
        Rvalue::Use(of) | Rvalue::Unary(_, of) | Rvalue::Cast(_, of, _) => operand_reads(of, reads),
        Rvalue::Binary(_, left, right) => {
            operand_reads(left, reads);
            operand_reads(right, reads);
        }
        Rvalue::Aggregate(_, operands) => {
            operands
                .iter()
                .for_each(|operand| operand_reads(operand, reads));
        }
        // A local whose own address is taken is not looked at; any
        // other is read on the way to the place.
        Rvalue::AddrOf(place, _) | Rvalue::Len(place) => place_reads(place, reads),
    }
}

fn terminator_reads(term: &TerminatorKind, reads: &mut Vec<LocalId>) {
    match term {
        TerminatorKind::Switch { on, .. } => operand_reads(on, reads),
        TerminatorKind::Call {
            callee, args, dest, ..
        } => {
            operand_reads(callee, reads);
            args.iter().for_each(|arg| operand_reads(arg, reads));
            if !dest.projections.is_empty() {
                place_reads(dest, reads);
            }
        }
        TerminatorKind::Assert { cond, args, .. } => {
            operand_reads(cond, reads);
            args.iter().for_each(|arg| operand_reads(arg, reads));
        }
        TerminatorKind::Drop { place, .. } => place_reads(place, reads),
        TerminatorKind::Goto(_)
        | TerminatorKind::Return
        | TerminatorKind::Unreachable
        | TerminatorKind::Resume => {}
    }
}

impl Verifier<'_> {
    /// Every temporary whose address is not taken has a value on every
    /// way to each read of it: it is assigned before the read in the
    /// same block, or in a block that every way goes through, or on
    /// every way that the search finds as it walks back. The search
    /// gives up after `SEARCH_AT_MOST` edges, so a value that several
    /// ways assign is read near where they meet.
    pub(super) fn initialized(&mut self, func: &Function, taken: &FxHashSet<LocalId>) {
        let checked = |local: LocalId| {
            (local.into_raw().into_u32() as usize) < func.locals.len()
                && func.locals[local].kind == LocalKind::Temp
                && !taken.contains(&local)
        };
        let succs: Vec<Vec<u32>> = func
            .blocks
            .values()
            .map(|block| {
                let all = block.term.kind.successors();
                all.into_iter().map(|(target, _)| raw(target)).collect()
            })
            .collect();
        let graph = Graph::walk(&succs, &mut self.steps);
        let idom = graph.dominators(&mut self.steps);
        let (enter, leave) = graph.intervals(&idom);

        // The blocks that assign each temporary in a statement, and for
        // each temporary the stretches of the walk down the tree in
        // which it has a value: below a block that assigns it, and from
        // the block that only a call that assigns it leads to.
        let mut assigns: FxHashSet<(u32, LocalId)> = FxHashSet::default();
        let mut valued: FxHashMap<LocalId, Vec<(u32, u32)>> = FxHashMap::default();
        for (id, block) in func.blocks.iter() {
            let at = graph.number[raw(id) as usize];
            if at == NONE {
                continue;
            }
            for stmt in &block.stmts {
                self.steps += 1;
                if let StatementKind::Assign(assigned) = &stmt.kind
                    && assigned.0.projections.is_empty()
                    && checked(assigned.0.local)
                    && assigns.insert((raw(id), assigned.0.local))
                {
                    let below = (enter[at as usize] + 1, leave[at as usize] - 1);
                    valued.entry(assigned.0.local).or_default().push(below);
                }
            }
            if let Some((local, target)) = returned(block)
                && checked(local)
                && graph.preds[target as usize] == [raw(id)]
            {
                let to = graph.number[target as usize] as usize;
                valued
                    .entry(local)
                    .or_default()
                    .push((enter[to], leave[to]));
            }
        }
        // Of stretches that lie in one another the outer is kept, so
        // that the last one that starts before a block is the only one
        // that can hold it.
        for stretches in valued.values_mut() {
            self.steps += stretches.len() as u64;
            stretches.sort_unstable_by_key(|&(from, to)| (from, std::cmp::Reverse(to)));
            let mut end = 0;
            stretches.retain(|&(from, to)| {
                let outer = from > end;
                if outer {
                    end = to;
                }
                outer
            });
        }
        let has_value = |local: LocalId, at: u32| {
            valued.get(&local).is_some_and(|stretches| {
                let after = stretches.partition_point(|&(from, _)| from <= enter[at as usize]);
                after > 0 && leave[at as usize] <= stretches[after - 1].1
            })
        };

        // What the search has found: the blocks at whose start a
        // temporary has a value, and the temporaries that are reported.
        let mut proven: FxHashSet<(u32, LocalId)> = FxHashSet::default();
        let mut reported: FxHashSet<LocalId> = FxHashSet::default();
        let mut reads = Vec::new();
        let mut tops = vec![NONE; succs.len()];
        for (id, block) in func.blocks.iter() {
            let here = raw(id);
            let number = graph.number[here as usize];
            if number == NONE {
                continue;
            }
            // The first read of each temporary that the block has not
            // assigned before.
            let mut set: FxHashSet<LocalId> = FxHashSet::default();
            let mut asked: Vec<(LocalId, u32)> = Vec::new();
            let mut ask = |reads: &mut Vec<LocalId>, set: &mut FxHashSet<LocalId>, at: usize| {
                for local in reads.drain(..) {
                    if checked(local) && set.insert(local) {
                        asked.push((local, at as u32));
                    }
                }
            };
            for (at, stmt) in block.stmts.iter().enumerate() {
                self.steps += 1;
                let StatementKind::Assign(assigned) = &stmt.kind else {
                    continue;
                };
                let (dest, rvalue) = &**assigned;
                rvalue_reads(rvalue, &mut reads);
                if !dest.projections.is_empty() {
                    place_reads(dest, &mut reads);
                }
                ask(&mut reads, &mut set, at);
                if dest.projections.is_empty() {
                    set.insert(dest.local);
                }
            }
            terminator_reads(&block.term.kind, &mut reads);
            ask(&mut reads, &mut set, block.stmts.len());

            for (local, at) in asked {
                if reported.contains(&local) || has_value(local, number) {
                    continue;
                }
                // Back from the block: a way ends well at an assignment,
                // and badly at the first block. Edges that are more
                // than the search has left are not taken up.
                let mut left = SEARCH_AT_MOST;
                let mut walk: Vec<(u32, u32)> = Vec::new();
                let mut walked = vec![here];
                let back = |walk: &mut Vec<(u32, u32)>, to: u32, left: usize| {
                    let preds = &graph.preds[to as usize];
                    let fits = to != 0 && walk.len() + preds.len() <= left;
                    if fits {
                        walk.extend(preds.iter().map(|&pred| (pred, to)));
                    }
                    fits
                };
                let mut found = back(&mut walk, here, left);
                while found && let Some((pred, to)) = walk.pop() {
                    left -= 1;
                    self.steps += 1;
                    let from: &BasicBlock = &func.blocks[BlockId::from_raw(pred.into())];
                    if assigns.contains(&(pred, local))
                        || returned(from) == Some((local, to))
                        || has_value(local, graph.number[pred as usize])
                    {
                        continue;
                    }
                    // No block on the way to `top` assigns it, or it
                    // would have a value in `pred`.
                    let top = graph.joined(&mut tops, pred, &mut self.steps);
                    if proven.contains(&(top, local)) || walked.contains(&top) {
                        continue;
                    }
                    walked.push(top);
                    found = back(&mut walk, top, left);
                }
                if found {
                    // Every way to each block that was walked through
                    // is one of those that ended well.
                    proven.extend(walked.into_iter().map(|block| (block, local)));
                    continue;
                }
                reported.insert(local);
                self.at = Some((here, at));
                let message = format!(
                    "the temporary _{} may have no value",
                    local.into_raw().into_u32()
                );
                self.error(85, message);
            }
        }
        self.at = None;
    }
}
