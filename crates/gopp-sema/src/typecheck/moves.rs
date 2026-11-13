//! Where a variable is read after `move` took its value (OWN-R5), where
//! a copy of a variable is the last that is read of it (ADR-0041 rule
//! 4), and where a write to the function's own copy of a slice or a map
//! is lost (rule 9).
//!
//! A function is a graph of blocks, and a block is a run of events: a
//! variable is read, given away as a copy, set, moved, or an element of
//! it is written. Passes go over the graph. Forward: the variables that
//! may be moved; a read of one is an error. Backward: the variables that
//! are read later; a copy of a variable that is not read later is its
//! last use, and the value itself may be given instead; a write to an
//! element of a parameter that is not read later is lost.
//!
//! The sets of both passes share what they have in common, so a join
//! costs as much as its two sides differ.

use std::cmp::Reverse;
use std::collections::BinaryHeap;

use super::*;
use crate::object::{Capture, Transfer};

/// What the function does to one of its variables.
#[derive(Clone, Copy)]
enum Event {
    /// It is read, or a part of it is written.
    Read(LocalId, Span),
    /// A copy of the whole variable is given to a new owner.
    Given(LocalId, ExprId),
    /// The whole variable gets a value.
    Set(LocalId),
    /// `move` takes its value.
    Moved(LocalId, Span),
    /// An element or an entry of a parameter that is a slice or a map
    /// by value is written: the variable is used, and nothing of it is
    /// read.
    Wrote(LocalId, Span),
}

/// A variable that is read where it may hold no value.
pub(super) struct Reused {
    pub local: LocalId,
    pub at: Span,
    /// A `move` that took the value.
    pub moved: Span,
}

#[derive(Default)]
pub(super) struct Found {
    /// The copies after which their variables are not read again.
    pub last: FxHashSet<ExprId>,
    pub errors: Vec<Reused>,
    /// For each parameter with a write that nothing reads: the first
    /// such write.
    pub lost: Vec<(LocalId, Span)>,
    /// Each `move` of a variable that a statement around it still
    /// reads where it is.
    pub held: Vec<(LocalId, Span)>,
    pub steps: u64,
}

/// A node of a set: 64 bits at the bottom, and above them the children
/// that are not empty, with a bit for each that is there.
#[derive(Clone, PartialEq, Eq, Hash)]
enum Node {
    Bits(u64),
    Inner(u64, Box<[u32]>),
}

/// Sets of small numbers. Equal sets are one node, so that they are
/// compared by their numbers, and a set that differs from another in a
/// few members shares the rest with it.
struct Sets {
    nodes: Vec<Node>,
    known: FxHashMap<Node, u32>,
    /// How many levels a set has: its members are below 64 to that
    /// power.
    levels: u32,
    steps: u64,
}

/// The set without members, at every level.
const EMPTY: u32 = 0;

impl Sets {
    fn new(members: usize) -> Self {
        let mut levels = 1;
        while 64usize.saturating_pow(levels) < members {
            levels += 1;
        }
        Self {
            nodes: vec![Node::Bits(0)],
            known: FxHashMap::default(),
            levels,
            steps: 0,
        }
    }

    fn node(&mut self, node: Node) -> u32 {
        if matches!(node, Node::Bits(0) | Node::Inner(0, _)) {
            return EMPTY;
        }
        if let Some(&known) = self.known.get(&node) {
            return known;
        }
        let made = self.nodes.len() as u32;
        self.nodes.push(node.clone());
        self.known.insert(node, made);
        made
    }

    fn bits(&self, node: u32) -> u64 {
        match self.nodes[node as usize] {
            Node::Bits(bits) => bits,
            Node::Inner(..) => 0,
        }
    }

    fn children(&self, node: u32) -> (u64, &[u32]) {
        match &self.nodes[node as usize] {
            Node::Inner(which, children) => (*which, children),
            Node::Bits(_) => (0, &[]),
        }
    }

    fn child(&self, node: u32, digit: u32) -> u32 {
        let (which, children) = self.children(node);
        match which >> digit & 1 {
            1 => children[(which & ((1 << digit) - 1)).count_ones() as usize],
            _ => EMPTY,
        }
    }

    /// The set with the member, or without it.
    fn put(&mut self, set: u32, member: u32, present: bool) -> u32 {
        self.put_at(set, self.levels - 1, member, present)
    }

    fn put_at(&mut self, node: u32, level: u32, member: u32, present: bool) -> u32 {
        self.steps += 1;
        let digit = member >> (6 * level) & 63;
        if level == 0 {
            let bits = match present {
                true => self.bits(node) | 1 << digit,
                false => self.bits(node) & !(1 << digit),
            };
            return self.node(Node::Bits(bits));
        }
        let old = self.child(node, digit);
        let new = self.put_at(old, level - 1, member, present);
        if new == old {
            return node;
        }
        let (which, children) = self.children(node);
        let at = (which & ((1 << digit) - 1)).count_ones() as usize;
        let had = usize::from(which >> digit & 1 == 1);
        let mut next = Vec::with_capacity(children.len() + 1);
        next.extend_from_slice(&children[..at]);
        if new != EMPTY {
            next.push(new);
        }
        next.extend_from_slice(&children[at + had..]);
        let which = match new {
            EMPTY => which & !(1 << digit),
            _ => which | 1 << digit,
        };
        self.node(Node::Inner(which, next.into()))
    }

    fn has(&self, set: u32, member: u32) -> bool {
        let mut node = set;
        for level in (1..self.levels).rev() {
            node = self.child(node, member >> (6 * level) & 63);
        }
        self.bits(node) >> (member & 63) & 1 == 1
    }

    /// The members of both sets.
    fn join(&mut self, a: u32, b: u32) -> u32 {
        self.join_at(a, b, self.levels - 1)
    }

    fn join_at(&mut self, a: u32, b: u32, level: u32) -> u32 {
        if a == b || b == EMPTY {
            return a;
        }
        if a == EMPTY {
            return b;
        }
        self.steps += 1;
        if level == 0 {
            return self.node(Node::Bits(self.bits(a) | self.bits(b)));
        }
        let which = self.children(a).0 | self.children(b).0;
        let mut children = Vec::with_capacity(which.count_ones() as usize);
        let mut rest = which;
        while rest != 0 {
            let digit = rest.trailing_zeros();
            rest &= rest - 1;
            let (left, right) = (self.child(a, digit), self.child(b, digit));
            children.push(self.join_at(left, right, level - 1));
        }
        self.node(Node::Inner(which, children.into()))
    }
}

/// How an expression is evaluated.
#[derive(Clone, Copy)]
enum Todo {
    /// For its value, which a new owner may get.
    Value(ExprId),
    /// For a part of it, or to be read where it is.
    Part(ExprId),
    /// `move` has evaluated its operand.
    Moved(LocalId, Span),
    /// A built-in function has evaluated its operands and writes.
    Wrote(LocalId, Span),
}

struct Walk<'a> {
    tree: &'a File,
    body: &'a Body,
    modes: &'a FxHashMap<ExprId, Transfer>,
    captures: &'a FxHashMap<ExprId, Vec<Capture>>,
    /// The parameters and receivers by value that are slices or maps,
    /// in a file with the rules of Go++.
    watched: &'a FxHashSet<LocalId>,
    /// The function that is walked: what is declared in it is its own.
    within: Span,
    events: Vec<Event>,
    /// For each block, where its events start and end.
    blocks: Vec<(u32, u32)>,
    edges: Vec<(u32, u32)>,
    current: u32,
    exit: u32,
    /// The results of the function that have names.
    results: Vec<LocalId>,
    /// Where `break` and `continue` go, the innermost last, each with
    /// the label of its statement.
    breaks: Vec<(Option<Spur>, u32)>,
    continues: Vec<(Option<Spur>, u32)>,
    labels: FxHashMap<Spur, u32>,
    /// The label of the statement that is walked next.
    label: Option<Spur>,
    /// Where `fallthrough` goes.
    falls: Option<u32>,
    /// The variables that something reads where they are while a
    /// statement runs: what a loop ranges over, what is switched on. No
    /// copy of one is its last use there.
    held: FxHashMap<LocalId, u32>,
    /// The variables that a loop ranges over with a variable for the
    /// elements: what is written to an element may be read through it.
    viewed: FxHashMap<LocalId, u32>,
    seen: FxHashMap<LocalId, u32>,
    /// The variables that a function literal uses or whose address is
    /// taken: they may be read at any time, and have no last use.
    reached: FxHashSet<LocalId>,
    /// The function literals that are walked later.
    literals: Vec<ExprId>,
    found: Found,
}

/// What `move` and the last uses say of the declaration `decl`, whose
/// body the checker has walked.
pub(super) fn of(
    tree: &File,
    body: &Body,
    modes: &FxHashMap<ExprId, Transfer>,
    captures: &FxHashMap<ExprId, Vec<Capture>>,
    watched: &FxHashSet<LocalId>,
    decl: &ast::Decl,
) -> Found {
    let mut walk = Walk {
        tree,
        body,
        modes,
        captures,
        watched,
        within: decl.span,
        events: Vec::new(),
        blocks: Vec::new(),
        edges: Vec::new(),
        current: 0,
        exit: 0,
        results: Vec::new(),
        breaks: Vec::new(),
        continues: Vec::new(),
        labels: FxHashMap::default(),
        label: None,
        falls: None,
        held: FxHashMap::default(),
        viewed: FxHashMap::default(),
        seen: FxHashMap::default(),
        reached: FxHashSet::default(),
        literals: Vec::new(),
        found: Found::default(),
    };
    match &decl.kind {
        DeclKind::Func {
            ty,
            body: Some(block),
            ..
        } => walk.function(*ty, block),
        DeclKind::Var { values, .. } | DeclKind::Const { values, .. } => {
            walk.enter();
            walk.unit(values);
            walk.leave();
        }
        _ => {}
    }
    while let Some(literal) = walk.literals.pop() {
        if let ExprKind::FuncLit { ty, body } = &tree.exprs[literal].kind {
            walk.within = tree.exprs[literal].span;
            walk.function(*ty, body);
        }
    }
    walk.found.errors.sort_by_key(|reused| reused.at.lo);
    walk.found.errors.dedup_by_key(|reused| reused.at);
    walk.found.lost.sort_by_key(|&(local, at)| (local, at.lo));
    walk.found.lost.dedup_by_key(|&mut (local, _)| local);
    walk.found.lost.sort_by_key(|&(_, at)| at.lo);
    walk.found
}

impl<'a> Checker<'a> {
    /// The parameters and receivers by value of the body that are
    /// slices or maps: what a function writes to the elements of one is
    /// written to its own copy (PAR-1). A Go file has no such rule.
    fn own_collections(&mut self, cx: &Cx<'a>) -> FxHashSet<LocalId> {
        let mut found = FxHashSet::default();
        if !self.own_rules(cx) {
            return found;
        }
        for (at, declared) in cx.body.locals.iter().enumerate() {
            self.steps += 1;
            if !matches!(declared.kind, LocalKind::Param | LocalKind::Receiver) {
                continue;
            }
            let local = LocalId(at as u32);
            let Some(ty) = self.locals.get(&local).map(|x| x.ty) else {
                continue;
            };
            let Ok(under) = self.types.common_under(ty) else {
                continue;
            };
            if let Type::Slice(_) | Type::Map { .. } = &*self.types.get(under) {
                found.insert(local);
            }
        }
        found
    }

    /// Reports each read of a variable that `move` may have emptied
    /// (OWN-R5), each `move` of what a statement around it still reads
    /// (OWN-B2) and each write to a parameter that is lost (PAR-1), and
    /// gives the copies that are last uses.
    pub(super) fn moves(
        &mut self,
        cx: &Cx<'a>,
        captures: &FxHashMap<ExprId, Vec<Capture>>,
        decl: &ast::Decl,
    ) -> FxHashSet<ExprId> {
        let watched = self.own_collections(cx);
        // Nothing is given, moved or written so in most bodies.
        if self.var_modes.is_empty() && watched.is_empty() {
            return FxHashSet::default();
        }
        let found = of(cx.tree, cx.body, &self.var_modes, captures, &watched, decl);
        self.steps += found.steps;
        let named = |this: &Self, local: LocalId| {
            let name = cx.body.locals[local.0 as usize].name;
            this.names.resolve(&name).to_owned()
        };
        // What follows of a move that is refused is said once.
        let refused: FxHashSet<Span> = found.held.iter().map(|&(_, at)| at).collect();
        for (local, at) in found.held {
            let name = named(self, local);
            let message = format!(
                "cannot move {name}: the loop that ranges over it, or the switch on it, still \
                 reads it"
            );
            self.error(Code::OwnBorrowedMove, at, message);
        }
        for (local, at) in found.lost {
            let name = named(self, local);
            let ty = self.locals.get(&local).map_or(Types::INVALID, |x| x.ty);
            let message = format!(
                "this write to {name} is lost: {name} is the function's own copy, and nothing \
                 reads it afterwards"
            );
            let fix = format!(
                "declare {name} *{} to change the value of the caller, or return {name}",
                self.type_string(ty)
            );
            self.error_with_fix(
                Code::OwnLostWrite,
                at,
                message,
                (Code::NoteOwnFixPointer, fix),
            );
        }
        for reused in found.errors {
            let name = cx.body.locals[reused.local.0 as usize].name;
            let name = self.names.resolve(&name).to_owned();
            if refused.contains(&reused.moved) || !self.report_once((reused.at, Code::UseAfterMove))
            {
                continue;
            }
            // A move that comes around a loop to itself.
            if reused.at == reused.moved {
                let message = format!(
                    "{name} is moved again where it was moved before: it has no value until it \
                     is assigned again"
                );
                self.emit(Diagnostic::new(Code::UseAfterMove, reused.at, message));
                continue;
            }
            let message = format!(
                "{name} is used after it was moved: it has no value until it is assigned again"
            );
            let diagnostic = Diagnostic::new(Code::UseAfterMove, reused.at, message);
            let note = format!("{name} is moved here");
            self.emit(diagnostic.with_note(Code::NoteMovedHere, Some(reused.moved), note));
        }
        found.last
    }
}

impl<'a> Walk<'a> {
    fn unparen(&self, mut expr: ExprId) -> ExprId {
        while let ExprKind::Paren(inner) = self.tree.exprs[expr].kind {
            expr = inner;
        }
        expr
    }

    /// The variable of the function that is walked which the name
    /// denotes.
    fn local(&self, name: ExprId) -> Option<LocalId> {
        let body = self.body;
        match body.uses.get(&name).or_else(|| body.keys.get(&name)) {
            Some(&Ref::Local(local)) => self.own(local).then_some(local),
            _ => None,
        }
    }

    fn own(&self, local: LocalId) -> bool {
        use LocalKind::*;
        let declared = &self.body.locals[local.0 as usize];
        matches!(declared.kind, Var | Receiver | Param | Result)
            && self.within.lo <= declared.span.lo
            && declared.span.hi <= self.within.hi
    }

    /// The variable that the place of the expression is in, or is
    /// reached through.
    fn root(&self, mut expr: ExprId) -> Option<LocalId> {
        loop {
            expr = match &self.tree.exprs[expr].kind {
                ExprKind::Paren(x)
                | ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Slice { x, .. }
                | ExprKind::Unary { x, .. } => *x,
                ExprKind::Name(_) => return self.local(expr),
                _ => return None,
            };
        }
    }

    // ---- the graph

    fn block(&mut self) -> u32 {
        self.blocks.push((0, 0));
        self.blocks.len() as u32 - 1
    }

    /// The events from here on are those of the block.
    fn start(&mut self, block: u32) {
        let at = self.events.len() as u32;
        self.blocks[self.current as usize].1 = at;
        self.blocks[block as usize] = (at, at);
        self.current = block;
    }

    fn jump(&mut self, to: u32) {
        self.edges.push((self.current, to));
    }

    /// What follows is reached from nowhere.
    fn dead(&mut self) {
        let block = self.block();
        self.start(block);
    }

    fn label_block(&mut self, label: Spur) -> u32 {
        if let Some(&block) = self.labels.get(&label) {
            return block;
        }
        let block = self.block();
        self.labels.insert(label, block);
        block
    }

    fn hold(&mut self, local: Option<LocalId>) {
        if let Some(local) = local {
            *self.held.entry(local).or_default() += 1;
        }
    }

    fn release(&mut self, local: Option<LocalId>) {
        if let Some(count) = local.and_then(|local| self.held.get_mut(&local)) {
            *count -= 1;
        }
    }

    fn enter(&mut self) {
        self.events.clear();
        self.blocks.clear();
        self.edges.clear();
        self.labels.clear();
        self.held.clear();
        self.viewed.clear();
        self.results.clear();
        self.current = self.block();
        self.exit = self.block();
    }

    fn leave(&mut self) {
        self.jump(self.exit);
        self.start(self.exit);
        self.solve();
    }

    fn function(&mut self, ty: ExprId, block: &'a Block) {
        self.enter();
        if let ExprKind::FuncType { results, .. } = &self.tree.exprs[ty].kind {
            let named = results.iter().filter_map(|result| result.name);
            let locals = named.filter_map(|name| self.body.defs.get(&name.span.lo));
            self.results = locals.copied().collect();
        }
        self.stmts(&block.stmts);
        self.leave();
    }

    // ---- expressions

    /// The expressions of one statement, in the order in which they are
    /// evaluated. What a statement reads twice it may read in any
    /// order: none of its copies of such a variable is the last use.
    fn unit(&mut self, exprs: &[ExprId]) {
        let from = self.events.len();
        self.values(exprs);
        self.settle(from);
    }

    fn settle(&mut self, from: usize) {
        let given = |event: &Event| matches!(event, Event::Given(..));
        if !self.events[from..].iter().any(given) {
            return;
        }
        self.seen.clear();
        for event in &self.events[from..] {
            self.found.steps += 1;
            if let Event::Read(local, _)
            | Event::Given(local, _)
            | Event::Moved(local, _)
            | Event::Wrote(local, _) = event
            {
                *self.seen.entry(*local).or_default() += 1;
            }
        }
        for event in &mut self.events[from..] {
            if let Event::Given(local, expr) = *event
                && self.seen[&local] > 1
            {
                *event = Event::Read(local, self.tree.exprs[expr].span);
            }
        }
    }

    fn values(&mut self, exprs: &[ExprId]) {
        self.walk(exprs.iter().rev().map(|&expr| Todo::Value(expr)).collect());
    }

    fn parts(&mut self, exprs: &[ExprId]) {
        self.walk(exprs.iter().rev().map(|&expr| Todo::Part(expr)).collect());
    }

    /// What is to be evaluated, the first last.
    fn walk(&mut self, mut rest: Vec<Todo>) {
        while let Some(todo) = rest.pop() {
            self.found.steps += 1;
            match todo {
                Todo::Moved(local, span) => self.events.push(Event::Moved(local, span)),
                Todo::Wrote(local, span) => self.wrote(local, span),
                Todo::Value(expr) => self.expr(expr, false, &mut rest),
                Todo::Part(expr) => self.expr(expr, true, &mut rest),
            }
        }
    }

    fn expr(&mut self, expr: ExprId, part: bool, rest: &mut Vec<Todo>) {
        let tree = self.tree;
        let span = tree.exprs[expr].span;
        let values = |rest: &mut Vec<Todo>, exprs: &[ExprId]| {
            rest.extend(exprs.iter().rev().map(|&expr| Todo::Value(expr)));
        };
        match &tree.exprs[expr].kind {
            ExprKind::Name(_) => {
                let Some(local) = self.local(expr) else {
                    return;
                };
                let copied = self.modes.get(&expr) == Some(&Transfer::Copy);
                let held = self.held.get(&local).is_some_and(|&count| count > 0);
                self.events.push(match copied && !part && !held {
                    true => Event::Given(local, expr),
                    false => Event::Read(local, span),
                });
            }
            ExprKind::Paren(x) => rest.push(match part {
                true => Todo::Part(*x),
                false => Todo::Value(*x),
            }),
            // A function literal reads what it uses of the functions
            // around it where it is written.
            ExprKind::FuncLit { .. } => {
                self.literals.push(expr);
                for capture in self.captures.get(&expr).map_or(&[][..], |all| &all[..]) {
                    if self.own(capture.local) {
                        self.events.push(Event::Read(capture.local, span));
                        self.reached.insert(capture.local);
                    }
                }
            }
            ExprKind::CompositeLit { elems, .. } => values(rest, elems),
            ExprKind::KeyValue { key, value } => {
                rest.push(Todo::Value(*value));
                rest.push(Todo::Part(*key));
            }
            ExprKind::Unary { op, x } => {
                if *op == UnaryOp::Addr {
                    self.reached.extend(self.root(*x));
                }
                rest.push(Todo::Part(*x));
            }
            ExprKind::Selector { x, .. }
            | ExprKind::Assert { x, .. }
            | ExprKind::TypeSwitchGuard { x, .. } => rest.push(Todo::Part(*x)),
            ExprKind::Index { x, indices } => {
                values(rest, indices);
                rest.push(Todo::Part(*x));
            }
            ExprKind::Slice { x, lo, hi, max, .. } => {
                let bounds: Vec<ExprId> = [lo, hi, max].into_iter().flatten().copied().collect();
                values(rest, &bounds);
                rest.push(Todo::Part(*x));
            }
            ExprKind::Binary { x, y, .. } => {
                rest.push(Todo::Part(*y));
                rest.push(Todo::Part(*x));
            }
            ExprKind::Call { fun, args, .. } => {
                let callee = self.unparen(*fun);
                let moves = self.body.uses.get(&callee) == Some(&Ref::Universe(Predeclared::Move));
                let operand = args.first().map(|&arg| self.unparen(arg));
                // A `move` that the checker refused takes nothing.
                let taken = operand.filter(|_| moves).and_then(|name| {
                    let accepted = self.modes.get(&name) == Some(&Transfer::Move);
                    self.local(name).filter(|_| accepted)
                });
                if let Some(local) = taken {
                    if self.held.get(&local).is_some_and(|&count| count > 0) {
                        self.found.held.push((local, span));
                    }
                    rest.push(Todo::Moved(local, span));
                    return;
                }
                // `delete` and `clear` write what their first operand
                // names.
                let empties = matches!(
                    self.body.uses.get(&callee),
                    Some(Ref::Universe(Predeclared::Delete | Predeclared::Clear))
                );
                let written = operand.filter(|_| empties).and_then(|first| {
                    let root = self.root(first);
                    root.filter(|root| self.watched.contains(root))
                });
                match (written, operand) {
                    (Some(local), Some(first)) => {
                        rest.push(Todo::Wrote(local, span));
                        values(rest, &args[1..]);
                        values(rest, &self.indexes(first));
                    }
                    _ => {
                        values(rest, args);
                        rest.push(Todo::Part(*fun));
                    }
                }
            }
            ExprKind::MacroCall { args, .. } => {
                rest.extend(args.iter().rev().map(|&arg| Todo::Part(arg)));
            }
            _ => {}
        }
    }

    // ---- statements

    fn stmts(&mut self, stmts: &[StmtId]) {
        for &stmt in stmts {
            self.stmt(stmt);
        }
    }

    /// The place gets its value: a variable is set, and a part of one
    /// is written where the variable is.
    fn stored(&mut self, place: ExprId) {
        let tree = self.tree;
        let inner = self.unparen(place);
        let span = tree.exprs[inner].span;
        if let ExprKind::Name(_) = tree.exprs[inner].kind {
            let declared = self.body.defs.get(&span.lo).copied();
            if let Some(local) = declared.or_else(|| self.local(inner)) {
                self.events.push(Event::Set(local));
            }
        } else if let Some(local) = self.root(inner) {
            self.wrote(local, span);
        }
    }

    /// A part of the variable is written. For a parameter that is
    /// watched the write is one that may be lost, unless a loop around
    /// it has a variable for the elements, which sees it.
    fn wrote(&mut self, local: LocalId, span: Span) {
        let viewed = self.viewed.get(&local).is_some_and(|&count| count > 0);
        self.events
            .push(match self.watched.contains(&local) && !viewed {
                true => Event::Wrote(local, span),
                false => Event::Read(local, span),
            });
    }

    /// The indexes on the way to the place, in the order in which they
    /// are evaluated. The variable at the root is not read for them.
    fn indexes(&self, mut place: ExprId) -> Vec<ExprId> {
        let mut found = Vec::new();
        loop {
            place = match &self.tree.exprs[place].kind {
                ExprKind::Paren(x) | ExprKind::Selector { x, .. } | ExprKind::Unary { x, .. } => *x,
                ExprKind::Index { x, indices } => {
                    found.extend(indices.iter().rev());
                    *x
                }
                ExprKind::Slice { x, lo, hi, max, .. } => {
                    found.extend([max, hi, lo].into_iter().flatten());
                    *x
                }
                _ => break,
            };
        }
        found.reverse();
        found
    }

    /// `p = append(p, more...)` for a parameter `p` that is a slice by
    /// value: the parameter, and what is appended.
    fn grown(
        &self,
        op: AssignOp,
        lhs: &[ExprId],
        rhs: &'a [ExprId],
    ) -> Option<(LocalId, &'a [ExprId])> {
        let (AssignOp::Assign, [place], [value]) = (op, lhs, rhs) else {
            return None;
        };
        let place = self.unparen(*place);
        let named = matches!(self.tree.exprs[place].kind, ExprKind::Name(_));
        let local = self
            .local(place)
            .filter(|local| named && self.watched.contains(local))?;
        let ExprKind::Call { fun, args, .. } = &self.tree.exprs[self.unparen(*value)].kind else {
            return None;
        };
        let appends =
            self.body.uses.get(&self.unparen(*fun)) == Some(&Ref::Universe(Predeclared::Append));
        let (first, more) = args.split_first()?;
        let first = self.unparen(*first);
        let same = matches!(self.tree.exprs[first].kind, ExprKind::Name(_))
            && self.local(first) == Some(local);
        (appends && same).then_some((local, more))
    }

    /// What is evaluated of the places before their values are: all of
    /// a place that is no variable, and a variable that is read for
    /// its new value.
    fn places(&mut self, places: &[ExprId], read: bool) {
        for &place in places {
            let inner = self.unparen(place);
            let named = matches!(self.tree.exprs[inner].kind, ExprKind::Name(_));
            if named && !read {
                continue;
            }
            // A write to an element of a parameter reads its indexes
            // only: what was written before is not seen by it.
            let written = self.root(inner).filter(|root| self.watched.contains(root));
            match written.filter(|_| !named) {
                Some(_) => self.values(&self.indexes(inner)),
                None => self.parts(&[inner]),
            }
        }
    }

    fn target(&self, targets: &[(Option<Spur>, u32)], label: Option<Ident>) -> Option<u32> {
        let found = match label {
            Some(label) => targets.iter().rev().find(|(of, _)| *of == Some(label.name)),
            None => targets.last(),
        };
        found.map(|&(_, block)| block)
    }

    fn stmt(&mut self, stmt: StmtId) {
        self.found.steps += 1;
        let tree = self.tree;
        let label = self.label.take();
        match &tree.stmts[stmt].kind {
            StmtKind::Bad | StmtKind::Empty => {}
            StmtKind::Labeled { label, stmt } => {
                let at = self.label_block(label.name);
                self.jump(at);
                self.start(at);
                self.label = Some(label.name);
                self.stmt(*stmt);
            }
            StmtKind::Block(block) => self.stmts(&block.stmts),
            StmtKind::Expr(x) | StmtKind::Go(x) | StmtKind::Defer(x) => self.unit(&[*x]),
            StmtKind::Send { chan, value } => self.unit(&[*chan, *value]),
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    let DeclKind::Var { names, values, .. } = &tree.decl_nodes[decl].kind else {
                        continue;
                    };
                    self.unit(values);
                    for name in names {
                        if let Some(&local) = self.body.defs.get(&name.span.lo) {
                            self.events.push(Event::Set(local));
                        }
                    }
                }
            }
            StmtKind::Assign { op, lhs, rhs } => {
                let from = self.events.len();
                // The parameter grows by what it has: it is used, and
                // nothing of it is read for anything else.
                if let Some((local, more)) = self.grown(*op, lhs, rhs) {
                    self.values(more);
                    self.wrote(local, tree.stmts[stmt].span);
                    self.settle(from);
                    return;
                }
                self.places(lhs, matches!(op, AssignOp::Op(_)));
                self.values(rhs);
                for &place in lhs {
                    self.stored(place);
                }
                self.settle(from);
            }
            StmtKind::IncDec { x, .. } => {
                self.places(&[*x], true);
                self.stored(*x);
            }
            StmtKind::Return(values) => {
                self.unit(values);
                // The results with names are what is returned: set by
                // the values, and read as they are without values.
                for at in 0..self.results.len() {
                    let result = self.results[at];
                    self.events.push(match values.is_empty() {
                        true => Event::Read(result, tree.stmts[stmt].span),
                        false => Event::Set(result),
                    });
                }
                self.jump(self.exit);
                self.dead();
            }
            StmtKind::Branch { kind, label } => {
                let to = match kind {
                    BranchKind::Break => self.target(&self.breaks, *label),
                    BranchKind::Continue => self.target(&self.continues, *label),
                    BranchKind::Goto => label.map(|label| self.label_block(label.name)),
                    BranchKind::Fallthrough => self.falls,
                };
                if let Some(to) = to {
                    self.jump(to);
                }
                self.dead();
            }
            StmtKind::If { .. } => self.if_stmt(stmt),
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                if let Some(init) = init {
                    self.stmt(*init);
                }
                let [head, turn, next, after] = [(); 4].map(|()| self.block());
                self.jump(head);
                self.start(head);
                if let Some(cond) = cond {
                    self.unit(&[*cond]);
                    self.jump(after);
                }
                self.jump(turn);
                self.start(turn);
                self.breaks.push((label, after));
                self.continues.push((label, next));
                self.stmts(&body.stmts);
                self.breaks.pop();
                self.continues.pop();
                self.jump(next);
                self.start(next);
                if let Some(post) = post {
                    self.stmt(*post);
                }
                self.jump(head);
                self.start(after);
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                self.parts(&[*x]);
                let ranged = self.root(*x);
                let [head, turn, after] = [(); 3].map(|()| self.block());
                self.jump(head);
                self.start(head);
                let mut elements = false;
                for (at, &left) in lhs.iter().enumerate() {
                    let mut left = self.unparen(left);
                    // `&v` is a variable too.
                    if let ExprKind::Unary {
                        op: UnaryOp::Addr,
                        x,
                    } = tree.exprs[left].kind
                    {
                        left = self.unparen(x);
                    }
                    if !define {
                        self.places(&[left], false);
                    }
                    let before = self.events.len();
                    self.stored(left);
                    // A second variable is each element in its turn.
                    elements |= at == 1 && self.events.len() > before;
                }
                self.jump(after);
                self.jump(turn);
                self.start(turn);
                // The loop reads what it ranges over where it is, until
                // it is left by any way.
                self.hold(ranged);
                let viewed = ranged.filter(|_| elements);
                if let Some(viewed) = viewed {
                    *self.viewed.entry(viewed).or_default() += 1;
                }
                self.breaks.push((label, after));
                self.continues.push((label, head));
                self.stmts(&body.stmts);
                self.breaks.pop();
                self.continues.pop();
                if let Some(count) = viewed.and_then(|viewed| self.viewed.get_mut(&viewed)) {
                    *count -= 1;
                }
                self.release(ranged);
                self.jump(head);
                self.start(after);
            }
            StmtKind::Switch { init, tag, clauses } => {
                if let Some(init) = init {
                    self.stmt(*init);
                }
                let from = self.events.len();
                self.parts(tag.as_slice());
                let guard = tag.and_then(|tag| match tree.exprs[tag].kind {
                    ExprKind::TypeSwitchGuard { x, .. } => Some(x),
                    _ => None,
                });
                // What is switched on is read where it is by every
                // comparison, and a variable of a type switch may be
                // the value itself.
                let mut tested: Vec<LocalId> = Vec::new();
                for event in &self.events[from..] {
                    if let Event::Read(local, _) = *event {
                        tested.push(local);
                    }
                }
                let switched = guard.and_then(|x| self.root(x));
                tested.iter().for_each(|&local| self.hold(Some(local)));
                let after = self.block();
                let bodies: Vec<u32> = clauses.iter().map(|_| self.block()).collect();
                let mut default = after;
                for (clause, &body) in clauses.iter().zip(&bodies) {
                    let Some(cases) = &clause.cases else {
                        default = body;
                        continue;
                    };
                    if guard.is_none() {
                        self.parts(cases);
                    }
                    self.jump(body);
                    let next = self.block();
                    self.jump(next);
                    self.start(next);
                }
                self.jump(default);
                tested.iter().for_each(|&local| self.release(Some(local)));
                self.hold(switched);
                self.breaks.push((label, after));
                let vars = self.body.switch_vars.get(&stmt);
                for (at, clause) in clauses.iter().enumerate() {
                    self.start(bodies[at]);
                    if let Some(&var) = vars.and_then(|vars| vars.get(at)) {
                        self.events.push(Event::Set(var));
                    }
                    let outer = std::mem::replace(&mut self.falls, bodies.get(at + 1).copied());
                    self.stmts(&clause.body);
                    self.falls = outer;
                    self.jump(after);
                }
                self.breaks.pop();
                self.release(switched);
                self.start(after);
            }
            StmtKind::Select { clauses } => {
                // Every channel and every value that is sent is
                // evaluated first, then one clause runs.
                let from = self.events.len();
                for clause in clauses {
                    match clause.comm.map(|comm| &tree.stmts[comm].kind) {
                        Some(StmtKind::Send { chan, value }) => self.values(&[*chan, *value]),
                        Some(StmtKind::Expr(x)) => self.values(&[*x]),
                        Some(StmtKind::Assign { op, lhs, rhs }) => {
                            self.places(lhs, matches!(op, AssignOp::Op(_)));
                            self.values(rhs);
                        }
                        _ => {}
                    }
                }
                self.settle(from);
                let entry = self.current;
                let after = self.block();
                self.breaks.push((label, after));
                for clause in clauses {
                    let at = self.block();
                    self.edges.push((entry, at));
                    self.start(at);
                    if let Some(StmtKind::Assign { lhs, .. }) =
                        clause.comm.map(|comm| &tree.stmts[comm].kind)
                    {
                        for &place in lhs {
                            self.stored(place);
                        }
                    }
                    self.stmts(&clause.body);
                    self.jump(after);
                }
                self.breaks.pop();
                self.start(after);
            }
        }
    }

    /// `if`, with every `else if` after it: a chain of them is no
    /// nesting.
    fn if_stmt(&mut self, mut stmt: StmtId) {
        let tree = self.tree;
        let after = self.block();
        loop {
            self.found.steps += 1;
            let StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } = &tree.stmts[stmt].kind
            else {
                self.stmt(stmt);
                break;
            };
            if let Some(init) = init {
                self.stmt(*init);
            }
            self.unit(&[*cond]);
            let [yes, no] = [(); 2].map(|()| self.block());
            self.jump(yes);
            self.jump(no);
            self.start(yes);
            self.stmts(&then.stmts);
            self.jump(after);
            self.start(no);
            match otherwise {
                Some(next) => stmt = *next,
                None => break,
            }
        }
        self.jump(after);
        self.start(after);
    }

    // ---- the passes

    fn solve(&mut self) {
        let given = self
            .events
            .iter()
            .any(|event| matches!(event, Event::Given(..)));
        let moved = self
            .events
            .iter()
            .any(|event| matches!(event, Event::Moved(..)));
        let wrote = self
            .events
            .iter()
            .any(|event| matches!(event, Event::Wrote(..)));
        if !given && !moved && !wrote {
            return;
        }
        self.blocks[self.current as usize].1 = self.events.len() as u32;
        let count = self.blocks.len();
        // For each block, the blocks after it and those before it.
        let lists = |back: bool| {
            let ends = |&(from, to): &(u32, u32)| match back {
                true => (to, from),
                false => (from, to),
            };
            let mut starts = vec![0u32; count + 1];
            for edge in &self.edges {
                starts[ends(edge).0 as usize + 1] += 1;
            }
            for at in 0..count {
                starts[at + 1] += starts[at];
            }
            let mut filled = starts.clone();
            let mut all = vec![0u32; self.edges.len()];
            for edge in &self.edges {
                let (from, to) = ends(edge);
                all[filled[from as usize] as usize] = to;
                filled[from as usize] += 1;
            }
            (starts, all)
        };
        let after = lists(false);
        let before = lists(true);
        self.found.steps += (count + self.edges.len() + self.events.len()) as u64;
        if moved {
            self.moved(&before, &after);
        }
        if given {
            self.last(&before, &after);
        }
        if wrote {
            self.lost(&before, &after);
        }
    }

    /// A number for each variable that an event of the kind names.
    fn members(&self, wanted: impl Fn(&Event) -> Option<LocalId>) -> FxHashMap<LocalId, u32> {
        let mut members = FxHashMap::default();
        for local in self.events.iter().filter_map(wanted) {
            let next = members.len() as u32;
            members.entry(local).or_insert(next);
        }
        members
    }

    /// Forward: the variables that `move` may have emptied. A read of
    /// one is an error.
    fn moved(&mut self, before: &Lists, after: &Lists) {
        let members = self.members(|event| match event {
            Event::Moved(local, _) => Some(*local),
            _ => None,
        });
        let mut sets = Sets::new(members.len());
        let count = self.blocks.len();
        let mut out = vec![EMPTY; count];
        let mut queued = vec![true; count];
        let mut queue: BinaryHeap<Reverse<u32>> = (0..count as u32).map(Reverse).collect();
        let entering = |sets: &mut Sets, out: &[u32], block: usize| {
            let mut set = EMPTY;
            for &from in neighbours(before, block) {
                set = sets.join(set, out[from as usize]);
            }
            set
        };
        while let Some(Reverse(block)) = queue.pop() {
            let block = block as usize;
            queued[block] = false;
            let mut set = entering(&mut sets, &out, block);
            let (first, last) = self.blocks[block];
            for event in &self.events[first as usize..last as usize] {
                match event {
                    Event::Moved(local, _) => set = sets.put(set, members[local], true),
                    Event::Set(local) => {
                        if let Some(&member) = members.get(local) {
                            set = sets.put(set, member, false);
                        }
                    }
                    _ => {}
                }
            }
            if set != out[block] {
                out[block] = set;
                for &to in neighbours(after, block) {
                    if !std::mem::replace(&mut queued[to as usize], true) {
                        queue.push(Reverse(to));
                    }
                }
            }
        }
        // Where each variable is moved, in the order of the source.
        let mut moves: FxHashMap<LocalId, Vec<Span>> = FxHashMap::default();
        for event in &self.events {
            if let Event::Moved(local, span) = *event {
                moves.entry(local).or_default().push(span);
            }
        }
        for spans in moves.values_mut() {
            spans.sort_by_key(|span| span.lo);
        }
        for block in 0..count {
            let mut set = entering(&mut sets, &out, block);
            let (first, last) = self.blocks[block];
            for at in first as usize..last as usize {
                let (local, span, moves_it) = match self.events[at] {
                    Event::Read(local, span) | Event::Wrote(local, span) => (local, span, false),
                    Event::Given(local, expr) => (local, self.tree.exprs[expr].span, false),
                    Event::Moved(local, span) => (local, span, true),
                    Event::Set(local) => {
                        if let Some(&member) = members.get(&local) {
                            set = sets.put(set, member, false);
                        }
                        continue;
                    }
                };
                let Some(&member) = members.get(&local) else {
                    continue;
                };
                if sets.has(set, member) {
                    // The move before the read, or the first one when
                    // the read comes around a loop.
                    let spans = &moves[&local];
                    let earlier = spans.iter().rev().find(|moved| moved.lo < span.lo);
                    let moved = *earlier.unwrap_or(&spans[0]);
                    self.found.errors.push(Reused {
                        local,
                        at: span,
                        moved,
                    });
                }
                // One report is enough for what follows in the block.
                set = sets.put(set, member, moves_it);
            }
        }
        self.found.steps += sets.steps;
    }

    /// Backward: the variables that are read later. A copy of a
    /// variable that is not is its last use.
    fn last(&mut self, before: &Lists, after: &Lists) {
        let asked = |event: &Event| match event {
            Event::Given(local, _) => Some(*local),
            _ => None,
        };
        let effect = |event: &Event| match *event {
            Event::Read(local, _)
            | Event::Given(local, _)
            | Event::Moved(local, _)
            | Event::Wrote(local, _) => Some((local, true)),
            Event::Set(local) => Some((local, false)),
        };
        for at in self.not_read_later(before, after, asked, effect) {
            if let Event::Given(local, expr) = self.events[at]
                && !self.reached.contains(&local)
            {
                self.found.last.insert(expr);
            }
        }
    }

    /// Backward: the parameters that are read later. A write to an
    /// element of one that is not is lost: nothing sees it, and the
    /// caller has its own value.
    fn lost(&mut self, before: &Lists, after: &Lists) {
        let asked = |event: &Event| match event {
            Event::Wrote(local, _) => Some(*local),
            _ => None,
        };
        // A new value ends what was written; a write reads nothing
        // and ends nothing.
        let effect = |event: &Event| match *event {
            Event::Read(local, _) | Event::Given(local, _) | Event::Moved(local, _) => {
                Some((local, true))
            }
            Event::Set(local) => Some((local, false)),
            Event::Wrote(..) => None,
        };
        // What a function literal uses or a pointer leads to may be read
        // at any time.
        for at in self.not_read_later(before, after, asked, effect) {
            if let Event::Wrote(local, span) = self.events[at]
                && !self.reached.contains(&local)
            {
                self.found.lost.push((local, span));
            }
        }
    }

    /// The events that `asked` names a variable for after which the
    /// variable is not read on any way. `effect` says of an event
    /// whether its variable is read by it, or is not read before it.
    fn not_read_later(
        &mut self,
        before: &Lists,
        after: &Lists,
        asked: fn(&Event) -> Option<LocalId>,
        effect: fn(&Event) -> Option<(LocalId, bool)>,
    ) -> Vec<usize> {
        let members = self.members(asked);
        let mut sets = Sets::new(members.len());
        let count = self.blocks.len();
        let mut entering = vec![EMPTY; count];
        let mut queued = vec![true; count];
        let mut queue: BinaryHeap<u32> = (0..count as u32).collect();
        let leaving = |sets: &mut Sets, entering: &[u32], block: usize| {
            let mut set = EMPTY;
            for &to in neighbours(after, block) {
                set = sets.join(set, entering[to as usize]);
            }
            set
        };
        let step = |sets: &mut Sets, set: u32, event: &Event| {
            let member =
                effect(event).and_then(|(local, read)| Some((*members.get(&local)?, read)));
            match member {
                Some((member, read)) => sets.put(set, member, read),
                None => set,
            }
        };
        while let Some(block) = queue.pop() {
            let block = block as usize;
            queued[block] = false;
            let mut set = leaving(&mut sets, &entering, block);
            let (first, last) = self.blocks[block];
            for event in self.events[first as usize..last as usize].iter().rev() {
                set = step(&mut sets, set, event);
            }
            if set != entering[block] {
                entering[block] = set;
                for &from in neighbours(before, block) {
                    if !std::mem::replace(&mut queued[from as usize], true) {
                        queue.push(from);
                    }
                }
            }
        }
        let mut found = Vec::new();
        for block in 0..count {
            let mut set = leaving(&mut sets, &entering, block);
            let (first, last) = self.blocks[block];
            for at in (first as usize..last as usize).rev() {
                let event = &self.events[at];
                if let Some(local) = asked(event)
                    && !sets.has(set, members[&local])
                {
                    found.push(at);
                }
                set = step(&mut sets, set, event);
            }
        }
        self.found.steps += sets.steps;
        found
    }
}

/// For each block where its neighbours start in the list of all, and
/// that list.
type Lists = (Vec<u32>, Vec<u32>);

fn neighbours(lists: &Lists, block: usize) -> &[u32] {
    let (starts, all) = lists;
    &all[starts[block] as usize..starts[block + 1] as usize]
}
