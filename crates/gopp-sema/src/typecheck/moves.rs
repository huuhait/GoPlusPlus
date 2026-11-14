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
pub(super) enum Event {
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
    /// The variable gets what may hold pointers: what the sources from
    /// the first number to the second say. With the flag, the whole
    /// variable is set.
    Holds(LocalId, u32, u32, bool),
    /// The change with the number is made to a place.
    Changed(u32),
    /// What the sources from the first number to the second say is
    /// returned.
    Returned(u32, u32, Span),
    /// A place behind a pointer of the variable gets what the sources
    /// from the first number to the second say: as many pointers behind
    /// the first one as the third number says.
    Stored(LocalId, u32, u32, u8, Span),
    /// The call with the number is made: what it is given may be
    /// stored behind the pointers that it is given.
    Called(u32),
    /// What the sources from the first number to the second say is put
    /// where the function does not follow it.
    Left(u32, u32, Span),
    /// The pointer with the number is taken into what a value owns
    /// apart from itself.
    Borrowed(u32),
    /// The variable is written, or a part of it: before the `Set` of an
    /// assignment, which no declaration has.
    Written(LocalId),
    /// The variable is a new one from here on: that of the next turn
    /// of its loop.
    Fresh(LocalId),
    /// A `go` statement runs the function literal with the number.
    Spawned(u32),
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
    /// What the function and each of its function literals do with
    /// pointers, for the passes that run when every body is walked.
    pub lent: Vec<borrows::Problem>,
    /// The variables that a function literal which `go` runs uses.
    pub spawned: Vec<Spawned>,
    pub steps: u64,
}

/// A variable that a function literal which `go` runs uses, and what
/// the literal does to it: whether it is touched after the `go`
/// statement, and whether it is written then (CON-13).
pub(super) struct Spawned {
    pub at: Span,
    pub local: LocalId,
    pub how: crate::object::CaptureUse,
    pub touched: bool,
    pub written: bool,
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
pub(super) struct Sets {
    nodes: Vec<Node>,
    known: FxHashMap<Node, u32>,
    /// How many levels a set has: its members are below 64 to that
    /// power.
    levels: u32,
    pub steps: u64,
}

/// The set without members, at every level.
pub(super) const EMPTY: u32 = 0;

impl Sets {
    pub(super) fn new(members: usize) -> Self {
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
    pub(super) fn put(&mut self, set: u32, member: u32, present: bool) -> u32 {
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

    pub(super) fn has(&self, set: u32, member: u32) -> bool {
        let mut node = set;
        for level in (1..self.levels).rev() {
            node = self.child(node, member >> (6 * level) & 63);
        }
        self.bits(node) >> (member & 63) & 1 == 1
    }

    /// The members of both sets.
    pub(super) fn join(&mut self, a: u32, b: u32) -> u32 {
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
    /// A call has evaluated its operands.
    Called(ExprId),
}

pub(super) struct Walk<'a> {
    pub tree: &'a File,
    pub body: &'a Body,
    modes: &'a FxHashMap<ExprId, Transfer>,
    captures: &'a FxHashMap<ExprId, Vec<Capture>>,
    /// The parameters and receivers by value that are slices or maps.
    watched: &'a FxHashSet<LocalId>,
    /// The function that is walked: what is declared in it is its own.
    pub(super) within: Span,
    pub events: Vec<Event>,
    /// For each block, where its events start and end.
    pub blocks: Vec<(u32, u32)>,
    pub(super) edges: Vec<(u32, u32)>,
    current: u32,
    exit: u32,
    /// The results of the function that have names.
    pub(super) results: Vec<LocalId>,
    /// Where `break` and `continue` go, the innermost last, each with
    /// the label of its statement.
    breaks: Vec<(Option<Spur>, u32)>,
    continues: Vec<(Option<Spur>, u32)>,
    labels: FxHashMap<Spur, u32>,
    /// How many statements with variables of their own are around
    /// where a `break` or a `continue` goes, by its block, and around
    /// each label: a jump ends the variables of those that it leaves.
    depths: FxHashMap<u32, usize>,
    label_depths: FxHashMap<Spur, usize>,
    /// Each `goto` to a label that comes later: the block that the
    /// jump goes through, the label, the variables that the statements
    /// around the jump had declared, and where it is.
    gotos: Vec<(u32, Spur, Vec<Vec<LocalId>>, Span)>,
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
    /// The `&` variables of the loops that are walked now, each with
    /// the parameter that it names an element of; and where a write
    /// through one is, with the variable.
    aliases: FxHashMap<LocalId, LocalId>,
    alias_writes: FxHashMap<u32, LocalId>,
    seen: FxHashMap<LocalId, u32>,
    /// The variables that a function literal uses or whose address is
    /// taken: they may be read at any time, and have no last use.
    reached: FxHashSet<LocalId>,
    /// The function literals that are walked later.
    literals: Vec<ExprId>,
    /// For each variable, how many function literals of the body may
    /// change it.
    literal_writes: FxHashMap<LocalId, u32>,
    /// The function literals that `go` statements run, each with where
    /// its statement is; and the variables whose address is taken.
    spawns: Vec<(Span, ExprId)>,
    addressed: FxHashSet<LocalId>,
    pub found: Found,
    /// What says what a place is, for a body that may make loans; and
    /// what the walk has found of them (`borrows.rs`).
    pub places: Option<&'a borrows::Places<'a>>,
    pub sources: Vec<borrows::Source>,
    /// The calls that give back what may hold pointers, and the sources
    /// of their arguments.
    pub calls: Vec<borrows::Call>,
    pub args: Vec<borrows::Source>,
    pub call_ids: FxHashMap<ExprId, u32>,
    pub carriers: FxHashMap<u32, borrows::Carrier>,
    /// The `Returned`, `Stored` and `Left` events, by the number of
    /// their first source, whose value is kept as a plain function.
    pub plain: FxHashSet<u32>,
    /// The function of the declaration, if others call it by its name;
    /// whether it is the one that is walked; and its parameters, the
    /// receiver first.
    pub owner: Option<borrows::Callee>,
    pub top: bool,
    pub params: Vec<Option<LocalId>>,
    pub funcs: Vec<bool>,
    pub changes: Vec<borrows::Change>,
    pub borrows: Vec<borrows::Borrow>,
    /// The changes that a loop around them refuses.
    pub broken: Vec<borrows::Broken>,
    /// The variables that the statements around declare, each
    /// statement with its own.
    pub frames: Vec<Vec<LocalId>>,
    /// The loops that range over a place, and for each variable the
    /// innermost of those that are walked now.
    pub loops: Vec<borrows::Ranged>,
    pub looping: FxHashMap<LocalId, u32>,
    /// How the place of the assignment that is walked is changed.
    change: borrows::What,
    /// For each result of the function, whether it is written as a
    /// pointer or an option of one: a handle is lent to it.
    pub pointers: Vec<bool>,
    /// How many events say what the function is given, before those of
    /// its statements.
    pub given: usize,
}

/// What `move` and the last uses say of the declaration `decl`, whose
/// body the checker has walked.
pub(super) fn of(
    tree: &File,
    body: &Body,
    modes: &FxHashMap<ExprId, Transfer>,
    captures: &FxHashMap<ExprId, Vec<Capture>>,
    watched: &FxHashSet<LocalId>,
    places: Option<&borrows::Places<'_>>,
    (decl, owner): (&ast::Decl, Option<borrows::Callee>),
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
        depths: FxHashMap::default(),
        label_depths: FxHashMap::default(),
        gotos: Vec::new(),
        label: None,
        falls: None,
        held: FxHashMap::default(),
        viewed: FxHashMap::default(),
        aliases: FxHashMap::default(),
        alias_writes: FxHashMap::default(),
        seen: FxHashMap::default(),
        reached: FxHashSet::default(),
        literals: Vec::new(),
        literal_writes: {
            let mut writes: FxHashMap<LocalId, u32> = FxHashMap::default();
            for capture in captures.values().flatten() {
                if capture.how != crate::object::CaptureUse::Read {
                    *writes.entry(capture.local).or_default() += 1;
                }
            }
            writes
        },
        spawns: Vec::new(),
        addressed: FxHashSet::default(),
        found: Found::default(),
        places,
        sources: Vec::new(),
        calls: Vec::new(),
        args: Vec::new(),
        call_ids: FxHashMap::default(),
        carriers: FxHashMap::default(),
        plain: FxHashSet::default(),
        owner,
        top: true,
        params: Vec::new(),
        funcs: Vec::new(),
        changes: Vec::new(),
        borrows: Vec::new(),
        broken: Vec::new(),
        frames: Vec::new(),
        loops: Vec::new(),
        looping: FxHashMap::default(),
        change: borrows::What::Assign,
        pointers: Vec::new(),
        given: 0,
    };
    match &decl.kind {
        DeclKind::Func {
            receiver,
            ty,
            body: Some(block),
            ..
        } => walk.function(receiver.as_ref(), *ty, block),
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
            walk.top = false;
            walk.function(None, *ty, body);
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
    /// The function of the declaration as those who call it name it.
    /// A generic one is one function for every type that it is used
    /// with.
    pub(super) fn lender(&self, cx: &Cx<'a>, decl: &ast::Decl) -> Option<borrows::Callee> {
        let DeclKind::Func { receiver, name, .. } = &decl.kind else {
            return None;
        };
        if receiver.is_none() {
            let object = self.by_span.get(&name.span)?;
            return Some(borrows::Callee::Func(ObjId(*object as u32)));
        }
        let receives = |local: &crate::Local| local.kind == LocalKind::Receiver;
        let local = LocalId(cx.body.locals.iter().position(receives)? as u32);
        let mut ty = self.locals.get(&local)?.ty;
        if let Type::Pointer(to) = &*self.types.get(self.types.underlying(ty)) {
            ty = *to;
        }
        match &*self.types.get(ty) {
            Type::Named { id, .. } => Some(borrows::Callee::Method(*id, name.name)),
            _ => None,
        }
    }

    /// The parameters and receivers by value of the body that are
    /// slices or maps: what a function writes to the elements of one is
    /// written to its own copy (PAR-1).
    fn own_collections(&mut self, cx: &Cx<'a>) -> FxHashSet<LocalId> {
        let mut found = FxHashSet::default();
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
        (decl, lender): (&ast::Decl, Option<borrows::Callee>),
    ) -> (FxHashSet<ExprId>, Vec<borrows::Problem>) {
        let watched = self.own_collections(cx);
        // Nothing is given, moved, written or lent so in most bodies.
        // A body lends if it takes an address or has a loop over
        // something, or a variable of it may hold a pointer or is a
        // handle, which is lent where a pointer is wanted.
        let handle = |held: &Operand| {
            let handle = matches!(
                &*self.types.get(self.types.underlying(held.ty)),
                Type::Unique(_) | Type::Shared(_)
            );
            handle || self.types.may_lend(held.ty)
        };
        let lends = std::mem::take(&mut self.lends) || self.locals.values().any(handle);
        let spawns = std::mem::take(&mut self.spawns);
        if self.var_modes.is_empty() && watched.is_empty() && !lends && !spawns {
            return Default::default();
        }
        let found = {
            let places = self
                .memo
                .as_ref()
                .filter(|_| lends)
                .map(|memo| borrows::Places {
                    types: self.types,
                    memo,
                    selections: &self.selections,
                    locals: &self.locals,
                    instances: &self.instantiated,
                    pointer_methods: &self.pointer_methods,
                    localized: &self.localized,
                    read_only: &self.read_only,
                    of_headers: &self.foreign_uses,
                    base: self.names.get_or_intern_static("base"),
                });
            let modes = &self.var_modes;
            of(
                cx.tree,
                cx.body,
                modes,
                captures,
                &watched,
                places.as_ref(),
                (decl, lender),
            )
        };
        self.steps += found.steps;
        // What the passes say of the functions decides whether a child
        // may be seen as its parent in a `Shared`: asked with the
        // first problem of the body.
        let mut found = found;
        let seen_as = std::mem::take(&mut self.seen_as);
        let only_names = Arc::new(self.only_names(cx));
        for problem in &mut found.lent {
            problem.only_names = only_names.clone();
        }
        if let Some(first) = found.lent.first_mut() {
            first.seen_as = seen_as;
        }
        let named = |this: &Self, local: LocalId| {
            let name = cx.body.locals[local.0 as usize].name;
            this.names.resolve(&name).to_owned()
        };
        self.shared_with_goroutines(cx, &found.spawned);
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
        (found.last, found.lent)
    }
}

impl<'a> Checker<'a> {
    /// A function literal that `go` runs uses the variables of the
    /// function where they are. One that nothing touches after the
    /// `go` statement is the goroutine's, and is of a type that may be
    /// given to one; one that is still touched is shared, so neither
    /// side changes it and its type may be shared (CON-13).
    fn shared_with_goroutines(&mut self, cx: &Cx<'a>, spawned: &[Spawned]) {
        use crate::object::CaptureUse;
        use crate::types::Marker;
        for found in spawned {
            let Some(ty) = self.locals.get(&found.local).map(|x| x.ty) else {
                continue;
            };
            // A pointer in it is the borrow check's.
            if self.open(ty) || self.types.may_hold_pointer(ty) {
                continue;
            }
            let name = cx.body.locals[found.local.0 as usize].name;
            let name = self.names.resolve(&name).to_owned();
            let label = match found.touched {
                true => Marker::Sync,
                false => Marker::Send,
            };
            let unlabeled = self.types.unlabeled(ty, label);
            let why = match (found.touched, found.how, found.written, unlabeled) {
                (true, CaptureUse::Write | CaptureUse::Address, ..) => format!(
                    "the goroutine changes {name}, and {name} is used after the go statement"
                ),
                (true, _, true, _) => format!(
                    "{name} is changed after the go statement, while the goroutine reads it"
                ),
                (true, _, _, Some((part, why))) => format!(
                    "{name} is used after the go statement, and {}",
                    self.unlabeled_text(part, why, "Sync")
                ),
                (false, _, _, Some((part, why))) => self.unlabeled_text(part, why, "Send"),
                _ => continue,
            };
            let message = format!("cannot use {name} in a new goroutine: {why}");
            let fix = "give the goroutine a variable of its own, put what changes in a \
                       sync.Atomic or a sync.Mutex, or send the result on a channel";
            let fix = (Code::NoteGoFixShare, fix.to_owned());
            self.error_with_fix(Code::GoCapture, found.at, message, fix);
        }
    }

    /// Says what was found of the pointers of a function of the body,
    /// whose source is `text`: by its walk, and by the passes (OWN-B2,
    /// OWN-B3, OWN-B5, RNG-3).
    /// `carried` says the type of what carries a loan.
    pub(super) fn lent(
        &mut self,
        (body, text): (&Body, &str),
        carried: &dyn Fn(borrows::Carrier) -> Option<TypeId>,
        problem: &borrows::Problem,
        found: &borrows::Lent,
    ) {
        let walked = &problem.broken;
        use borrows::What;
        let named = |this: &Self, local: LocalId| {
            let name = body.locals[local.0 as usize].name;
            this.names.resolve(&name).to_owned()
        };
        let text = |at: Span| text[at.lo as usize..at.hi as usize].to_owned();
        for written in &found.written {
            if !self.report_once((written.given, Code::OwnSharedWrite)) {
                continue;
            }
            let (called, given) = (text(written.at), text(written.given));
            // The receiver is written in what names the method.
            let receiver = written.at.lo <= written.given.lo && written.given.hi <= written.at.hi;
            let message = match (receiver, written.known) {
                (true, true) => format!(
                    "cannot call {called}: the method writes through its receiver, and what a \
                     Shared holds is only read through it"
                ),
                (true, false) => format!(
                    "cannot call {called}: nothing says that the method only reads through its \
                     receiver, and what a Shared holds is only read through it"
                ),
                (false, true) => format!(
                    "cannot give {given} to {called}: the function writes through it, and what a \
                     Shared holds is only read"
                ),
                (false, false) => format!(
                    "cannot give {given} to {called}: nothing says that the function only reads \
                     through it, and what a Shared holds is only read"
                ),
            };
            // The value has one name: a `Unique` is what is meant. The
            // name may stand behind `&`.
            let at = written.given.lo;
            let only = [at, at + 1]
                .iter()
                .any(|at| problem.only_names.contains(at));
            let fix = match only {
                true => Self::only_name_fix(&given),
                false => "put what changes in a Mutex or an Atomic, or own the value with a Unique"
                    .to_owned(),
            };
            let diagnostic = Diagnostic::new(Code::OwnSharedWrite, written.given, message);
            self.emit(diagnostic.with_note(Code::NoteOwnFixOption, None, fix));
        }
        for broken in walked.iter().chain(&found.broken) {
            let owner = named(self, broken.of);
            let place = match broken.what {
                What::Move => owner.clone(),
                _ => text(broken.at),
            };
            let made = text(broken.made);
            let made = made.strip_prefix('&').unwrap_or(&made).to_owned();
            let (code, does) = match (broken.what, broken.holder) {
                (What::Grow, None) | (What::Clear, None) | (What::Assign, None) => {
                    (Code::OwnRangeMutation, None)
                }
                (What::Grow, _) => (Code::OwnBorrowedAppend, Some("append to")),
                (What::Clear, _) => (Code::OwnBorrowedAppend, Some("clear")),
                (What::Assign, _) => (Code::OwnBorrowedAssign, Some("assign to")),
                (What::Move, _) => (Code::OwnBorrowedAssign, Some("move")),
                (What::Declare | What::End, _) => (Code::OwnBorrowOutlives, None),
                (What::Call, None) => (Code::OwnRangeMutation, None),
                (What::Call, _) => (Code::OwnBorrowedAssign, Some("call")),
            };
            if !self.report_once((broken.at, code)) {
                continue;
            }
            let diagnostic = match (broken.holder, does) {
                (None, _) => {
                    let does = match broken.what {
                        What::Grow => "append to",
                        What::Clear => "clear",
                        What::Move => "move",
                        What::Call => "call",
                        _ => "assign to",
                    };
                    let message = match broken.what {
                        What::Call => format!(
                            "cannot call {place}: it may change {made}, which the loop ranges over"
                        ),
                        _ => format!("cannot {does} {place}: the loop ranges over {made}"),
                    };
                    let fix = "collect the changes and apply them after the loop".to_owned();
                    Diagnostic::new(code, broken.at, message).with_note(
                        Code::NoteOwnRangeCollect,
                        None,
                        fix,
                    )
                }
                (Some(borrows::DEFERRED), _) => {
                    let waits = "a deferred call points into it and runs when the function returns";
                    let message = match (broken.what, does) {
                        (What::Call, _) => format!(
                            "cannot call {place}: it may change {owner}, and a deferred call \
                             points into {owner} and runs when the function returns"
                        ),
                        (_, Some(does)) => format!("cannot {does} {place}: {waits}"),
                        (What::Declare, None) => {
                            format!("{owner} is declared again here, and {waits}")
                        }
                        (_, None) => format!("{owner} ends here, and {waits}"),
                    };
                    let note = "the deferred call points into it from here".to_owned();
                    Diagnostic::new(code, broken.at, message).with_note(
                        Code::NoteBorrowedHere,
                        Some(broken.made),
                        note,
                    )
                }
                // A function literal uses the variable where it is.
                (Some(holder), Some(does)) if broken.captured => {
                    let holder = named(self, holder);
                    let message = format!(
                        "cannot {does} {place}: the function in {holder} uses it, and {holder} is \
                         used later"
                    );
                    let note = format!("the function that uses {owner} is written here");
                    Diagnostic::new(code, broken.at, message).with_note(
                        Code::NoteBorrowedHere,
                        Some(broken.made),
                        note,
                    )
                }
                (Some(holder), Some(does)) => {
                    let holder = named(self, holder);
                    let message = match broken.what {
                        What::Call => format!(
                            "cannot call {place}: it may change {owner}, and {holder} points \
                             into {owner} and is used later"
                        ),
                        _ => format!(
                            "cannot {does} {place}: {holder} points into it and is used later"
                        ),
                    };
                    let note = format!("{holder} points into {made} from here");
                    Diagnostic::new(code, broken.at, message).with_note(
                        Code::NoteBorrowedHere,
                        Some(broken.made),
                        note,
                    )
                }
                (Some(holder), None) => {
                    let holder = named(self, holder);
                    let ends = match broken.what {
                        What::Declare => "is declared again here",
                        _ => "ends here",
                    };
                    let message =
                        format!("{owner} {ends}, and {holder} points into it and is used later");
                    let note = format!("{holder} points into {made} from here");
                    Diagnostic::new(code, broken.at, message).with_note(
                        Code::NoteBorrowedHere,
                        Some(broken.made),
                        note,
                    )
                }
            };
            let holder = broken.holder.map(borrows::Carrier::Local);
            if !self.against_arguments(holder.and_then(carried), &diagnostic) {
                self.emit(diagnostic);
            }
        }
        for shared in &found.shared {
            if !self.report_once((shared.at, Code::FuncSharesCapture)) {
                continue;
            }
            let name = named(self, shared.of);
            let ty = carried(borrows::Carrier::Local(shared.of));
            let unshared = ty.and_then(|ty| self.types.unlabeled(ty, crate::types::Marker::Sync));
            let why = match (shared.changes, unshared) {
                (true, _) => format!("{name} is changed by it or by another function"),
                (false, Some((part, why))) => self.unlabeled_text(part, why, "Sync"),
                (false, None) => continue,
            };
            self.emit(shares_capture(shared.at, &name, &why));
        }
        for reached in &found.reached {
            if !self.report_once((reached.at, Code::OwnBorrowIntoArena)) {
                continue;
            }
            let several = "a pointer to its value was read through another pointer or came from \
                           a call, so a second pointer may reach the value and free what is \
                           pointed into";
            let whole = text(reached.at);
            let message = match reached.given {
                None => format!(
                    "cannot point into {}: {several}",
                    whole.strip_prefix('&').unwrap_or(&whole)
                ),
                Some(given) => format!(
                    "cannot give {} to {whole}: the function points into what the value owns, \
                     and {several}",
                    text(given)
                ),
            };
            let fix = "use the index each time, or copy the part out, change it and store it back";
            let diagnostic = Diagnostic::new(Code::OwnBorrowIntoArena, reached.at, message);
            self.emit(diagnostic.with_note(Code::NoteOwnFixAlias, None, fix.to_owned()));
        }
        for aliased in &found.aliased {
            if !self.report_once((aliased.at, Code::OwnAliasAtCall)) {
                continue;
            }
            let message = format!(
                "cannot give {} and {} to {}: both may point to the same place in {}",
                text(aliased.both.0),
                text(aliased.both.1),
                text(aliased.at),
                named(self, aliased.of)
            );
            let fix = "give places that cannot be the same: two variables, two fields, or two \
                       elements with constant indexes";
            let diagnostic = Diagnostic::new(Code::OwnAliasAtCall, aliased.at, message);
            self.emit(diagnostic.with_note(Code::NoteOwnFixAlias, None, fix.to_owned()));
        }
        for escaped in &found.escaped {
            use borrows::Leaves;
            let owner = named(self, escaped.of);
            let whole = text(escaped.at);
            // A function literal that uses a variable which may hold a
            // pointer is not kept (CLO-3a).
            let uses = format!(
                "a function that uses {owner}: {owner} may hold a pointer, and the function may \
                 run when what it points to has ended"
            );
            let (code, message) = match (escaped.how, escaped.given) {
                (Leaves::Returned, _) if escaped.captured => {
                    (Code::OwnCaptureKept, format!("cannot return {uses}"))
                }
                (Leaves::Stored(_), _) if escaped.captured => (
                    Code::OwnCaptureKept,
                    format!("cannot store in {whole} {uses}"),
                ),
                (Leaves::Called | Leaves::Kept, _) if escaped.captured => {
                    (Code::OwnCaptureKept, format!("cannot give {whole} {uses}"))
                }
                (Leaves::Left, _) if escaped.captured => {
                    (Code::OwnCaptureKept, format!("cannot keep {uses}"))
                }
                (Leaves::Returned, _) => (
                    Code::OwnReturnAddrLocal,
                    format!(
                        "cannot return a pointer into {owner}: {owner} ends when the function \
                         returns"
                    ),
                ),
                (Leaves::Stored(behind), false) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot store a pointer into {owner} in {whole}: what {} points to \
                         outlives {owner}",
                        named(self, behind)
                    ),
                ),
                (Leaves::Stored(behind), true) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot store what {owner} points to in {whole}: what {} points to may \
                         outlive it",
                        named(self, behind)
                    ),
                ),
                (Leaves::Called, false) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot give a pointer into {owner} to {whole}: the call may store it \
                         where it outlives {owner}"
                    ),
                ),
                (Leaves::Called, true) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot give what {owner} points to to {whole}: the call may store it \
                         where it outlives what {owner} points to"
                    ),
                ),
                (Leaves::Kept, false) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot give a pointer into {owner} to {whole}: the call may keep it \
                         longer than {owner}"
                    ),
                ),
                (Leaves::Kept, true) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot give what {owner} points to to {whole}: the call may keep it \
                         longer than what {owner} points to"
                    ),
                ),
                (Leaves::Left, false) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot keep a pointer into {owner} in {whole}: it may be used when \
                         {owner} has ended"
                    ),
                ),
                (Leaves::Left, true) => (
                    Code::OwnStoredOutlives,
                    format!(
                        "cannot keep what {owner} points to in {whole}: nothing says how long \
                         it is kept there"
                    ),
                ),
            };
            if !self.report_once((escaped.at, code)) {
                continue;
            }
            let note = match (escaped.captured, escaped.given) {
                (true, _) => format!("the function that uses {owner} is written here"),
                (_, true) => format!("{owner} points to what the function is given"),
                (_, false) => format!("the pointer into {owner} is made here"),
            };
            let diagnostic = Diagnostic::new(code, escaped.at, message);
            if !self.against_arguments(escaped.carrier.and_then(carried), &diagnostic) {
                self.emit(diagnostic.with_note(Code::NoteBorrowedHere, Some(escaped.made), note));
                continue;
            }
            // What is said of a pointer is held against the type
            // arguments; that the function changes the variable is true
            // of every one of them.
            if escaped.changes && self.report_once((escaped.at, Code::FuncSharesCapture)) {
                let why = format!("{owner} is changed by it or by another function");
                self.emit(shares_capture(escaped.at, &owner, &why));
            }
        }
    }

    /// What is found is carried by a value of the type `ty`. If only a
    /// type parameter says that such a value may hold a pointer, it is
    /// true of the type arguments with one and of no other: nothing is
    /// said here, and no type argument for the parameter holds one.
    fn against_arguments(&mut self, ty: Option<TypeId>, found: &Diagnostic) -> bool {
        let Some(Held::Params(params)) = ty.map(|ty| self.types.held(ty)) else {
            return false;
        };
        if params.is_empty() {
            return false;
        }
        let why = format!(
            "the function is not for a type with a pointer in it: {}",
            found.message
        );
        self.kept_where(&params, found.span, &why);
        true
    }
}

/// A function literal that is kept as a plain function uses the
/// variable `name` as it may not (CLO-2).
fn shares_capture(at: Span, name: &str, why: &str) -> Diagnostic {
    let message = format!(
        "cannot keep a function that uses {name} as a func value, which may run on another \
         goroutine: {why}"
    );
    let fix = format!(
        "keep it as a Local[func(...)], which stays with one goroutine, or put {name} in a \
         sync.Atomic or a sync.Mutex"
    );
    Diagnostic::new(Code::FuncSharesCapture, at, message).with_note(Code::NoteGoFixShare, None, fix)
}

impl<'a> Walk<'a> {
    pub(super) fn unparen(&self, mut expr: ExprId) -> ExprId {
        while let ExprKind::Paren(inner) = self.tree.exprs[expr].kind {
            expr = inner;
        }
        expr
    }

    /// The variable of the function that is walked which the name
    /// denotes.
    pub(super) fn local(&self, name: ExprId) -> Option<LocalId> {
        let body = self.body;
        match body.uses.get(&name).or_else(|| body.keys.get(&name)) {
            Some(&Ref::Local(local)) => self.own(local).then_some(local),
            _ => None,
        }
    }

    /// Whether a function literal other than `literal` may change the
    /// variable.
    pub(super) fn changed_by_other(&self, literal: ExprId, local: LocalId) -> bool {
        let writes = |capture: &&Capture| {
            capture.local == local && capture.how != crate::object::CaptureUse::Read
        };
        let own = self
            .captured(literal)
            .iter()
            .any(|capture| writes(&capture));
        let all = self.literal_writes.get(&local).copied().unwrap_or(0);
        all > u32::from(own)
    }

    /// The variables that the function literal uses of the functions
    /// around it.
    pub(super) fn captured(&self, literal: ExprId) -> &'a [Capture] {
        self.captures.get(&literal).map_or(&[], |all| &all[..])
    }

    pub(super) fn own(&self, local: LocalId) -> bool {
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
        self.sources.clear();
        self.changes.clear();
        self.borrows.clear();
        self.broken.clear();
        self.calls.clear();
        self.args.clear();
        self.call_ids.clear();
        self.carriers.clear();
        self.plain.clear();
        self.params.clear();
        self.funcs.clear();
        self.frames.clear();
        self.loops.clear();
        self.looping.clear();
        self.blocks.clear();
        self.edges.clear();
        self.labels.clear();
        self.spawns.clear();
        self.addressed.clear();
        self.depths.clear();
        self.label_depths.clear();
        self.gotos.clear();
        self.held.clear();
        self.viewed.clear();
        self.aliases.clear();
        self.alias_writes.clear();
        self.results.clear();
        self.given = 0;
        self.current = self.block();
        self.exit = self.block();
    }

    fn leave(&mut self) {
        self.jump(self.exit);
        // A `goto` to a label that came later goes through a block of
        // its own, where the variables end that it leaves.
        for (through, label, frames, at) in std::mem::take(&mut self.gotos) {
            let depth = self.label_depths.get(&label).copied().unwrap_or(0);
            self.start(through);
            for frame in frames.iter().skip(depth) {
                self.ended(frame, at);
            }
            let to = self.label_block(label);
            self.jump(to);
        }
        self.start(self.exit);
        self.solve();
    }

    fn function(&mut self, receiver: Option<&Param>, ty: ExprId, block: &'a Block) {
        self.enter();
        if let ExprKind::FuncType {
            params, results, ..
        } = &self.tree.exprs[ty].kind
        {
            for param in receiver.into_iter().chain(params.iter()) {
                let name = param.name;
                let local = name.and_then(|name| self.body.defs.get(&name.span.lo).copied());
                if let (Some(local), Some(name)) = (local, name) {
                    self.given_from_outside(local, name.span);
                }
                let ty = local.and_then(|local| self.places?.locals.get(&local));
                self.funcs.push(ty.is_some_and(|x| self.only_lends(x.ty)));
                self.params.push(local);
            }
            self.given = self.events.len();
            let named = results.iter().filter_map(|result| result.name);
            let locals = named.filter_map(|name| self.body.defs.get(&name.span.lo));
            self.results = locals.copied().collect();
            self.pointers = results
                .iter()
                .map(|result| self.pointer(result.ty))
                .collect();
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
                Todo::Called(call) => self.called(call),
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
                    self.addressed.extend(self.root(*x));
                    self.borrowing(*x, span, false);
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
                let clears =
                    self.body.uses.get(&callee) == Some(&Ref::Universe(Predeclared::Clear));
                if let (true, Some(first)) = (clears, operand) {
                    self.change(first, borrows::What::Clear);
                }
                match (written, operand) {
                    (Some(local), Some(first)) => {
                        rest.push(Todo::Wrote(local, span));
                        values(rest, &args[1..]);
                        values(rest, &self.indexes(first));
                    }
                    _ => {
                        rest.push(Todo::Called(expr));
                        values(rest, args);
                        // A receiver by value is given like an
                        // argument: the checker says where it is copied.
                        let receiver = match tree.exprs[callee].kind {
                            ExprKind::Selector { x, .. } => Some(self.unparen(x)),
                            _ => None,
                        };
                        let given = receiver.filter(|x| {
                            matches!(tree.exprs[*x].kind, ExprKind::Name(_))
                                && self.modes.get(x) == Some(&Transfer::Copy)
                        });
                        rest.push(match given {
                            Some(receiver) => Todo::Value(receiver),
                            None => Todo::Part(*fun),
                        });
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

    /// The statements of a block, on a stack that grows on demand:
    /// blocks are in blocks as deep as the parser takes them.
    fn stmts(&mut self, stmts: &[StmtId]) {
        for &stmt in stmts {
            gopp_syntax::with_stack(|| self.stmt(stmt));
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
                if declared.is_none() {
                    self.events.push(Event::Written(local));
                }
                self.events.push(Event::Set(local));
                self.wrote_through(local, span);
            }
        } else if let Some(local) = self.root(inner) {
            self.events.push(Event::Written(local));
            if !self.wrote_through(local, span) {
                self.wrote(local, span);
            }
        }
        self.change(inner, self.change);
    }

    /// A part of the variable is written. For a parameter that is
    /// watched the write is one that may be lost, unless a loop around
    /// it has a variable for the elements, which sees it.
    /// A write to the `&` variable of a loop over a parameter writes
    /// an element of the parameter: it is read through the variable
    /// until the next turn, and in the parameter after that. Whether
    /// the variable is one.
    fn wrote_through(&mut self, local: LocalId, span: Span) -> bool {
        let Some(&of) = self.aliases.get(&local) else {
            return false;
        };
        self.alias_writes.insert(span.lo, local);
        self.events.push(Event::Wrote(local, span));
        self.events.push(Event::Wrote(of, span));
        true
    }

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

    /// Whether the type is written as `*T` or as `Option[*T]`.
    fn pointer(&self, ty: ExprId) -> bool {
        let mut ty = self.unparen(ty);
        if let ExprKind::Index { indices, .. } = &self.tree.exprs[ty].kind
            && let [held] = indices[..]
        {
            ty = self.unparen(held);
        }
        matches!(
            self.tree.exprs[ty].kind,
            ExprKind::Unary {
                op: UnaryOp::Star,
                ..
            }
        )
    }

    /// Whether the one value is a call of `append`.
    fn appends(&self, values: &[ExprId]) -> bool {
        let [value] = values else {
            return false;
        };
        let ExprKind::Call { fun, .. } = &self.tree.exprs[self.unparen(*value)].kind else {
            return false;
        };
        self.body.uses.get(&self.unparen(*fun)) == Some(&Ref::Universe(Predeclared::Append))
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
        // What a statement with a body declares ends with it.
        let scoped = matches!(
            self.tree.stmts[stmt].kind,
            StmtKind::Block(_)
                | StmtKind::For { .. }
                | StmtKind::Range { .. }
                | StmtKind::If { .. }
                | StmtKind::Switch { .. }
                | StmtKind::Select { .. }
        );
        let scoped = scoped && self.places.is_some();
        if scoped {
            self.frames.push(Vec::new());
        }
        self.found.steps += 1;
        let tree = self.tree;
        let label = self.label.take();
        match &tree.stmts[stmt].kind {
            StmtKind::Bad | StmtKind::Empty => {}
            StmtKind::Labeled { label, stmt } => {
                self.label_depths.insert(label.name, self.frames.len());
                let at = self.label_block(label.name);
                self.jump(at);
                self.start(at);
                self.label = Some(label.name);
                self.stmt(*stmt);
            }
            StmtKind::Block(block) => self.stmts(&block.stmts),
            StmtKind::Expr(x) => self.unit(&[*x]),
            StmtKind::Defer(x) => {
                self.unit(&[*x]);
                self.deferred(*x);
            }
            // What a new goroutine runs is kept longer than the
            // statement, and so is what a channel is given.
            StmtKind::Go(x) => {
                self.unit(&[*x]);
                // A function value that is no `Local` one, which no
                // goroutine is given.
                if let ExprKind::Call { fun, .. } = &tree.exprs[self.unparen(*x)].kind
                    && self.runs_function(*fun)
                {
                    let literal = self.unparen(*fun);
                    let taken = matches!(tree.exprs[literal].kind, ExprKind::FuncLit { .. });
                    self.left(&[*fun], tree.exprs[*fun].span, taken);
                }
                // What a literal that it runs uses is the goroutine's
                // from here on (CON-13).
                if let ExprKind::Call { fun, .. } = &tree.exprs[self.unparen(*x)].kind {
                    let literal = self.unparen(*fun);
                    if let ExprKind::FuncLit { .. } = tree.exprs[literal].kind {
                        self.spawns.push((tree.stmts[stmt].span, literal));
                        self.events
                            .push(Event::Spawned(self.spawns.len() as u32 - 1));
                    }
                }
            }
            StmtKind::Send { chan, value } => {
                self.unit(&[*chan, *value]);
                self.left(&[*value], tree.exprs[*value].span, false);
            }
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    let DeclKind::Var { names, values, .. } = &tree.decl_nodes[decl].kind else {
                        continue;
                    };
                    self.unit(values);
                    let mut locals = Vec::with_capacity(names.len());
                    for name in names {
                        if let Some(&local) = self.body.defs.get(&name.span.lo) {
                            self.events.push(Event::Set(local));
                            self.declares(local, name.span);
                            locals.push(local);
                        }
                    }
                    self.declared(&locals, values);
                }
            }
            StmtKind::Assign { op, lhs, rhs } => {
                let from = self.events.len();
                // The parameter grows by what it has: it is used, and
                // nothing of it is read for anything else.
                if let Some((local, more)) = self.grown(*op, lhs, rhs) {
                    self.values(more);
                    self.wrote(local, tree.stmts[stmt].span);
                    self.change(lhs[0], borrows::What::Grow);
                    self.settle(from);
                    return;
                }
                self.places(lhs, matches!(op, AssignOp::Op(_)));
                self.values(rhs);
                if self.appends(rhs) {
                    self.change = borrows::What::Grow;
                }
                for &place in lhs {
                    self.stored(place);
                }
                self.change = borrows::What::Assign;
                self.assigned(lhs, rhs);
                self.settle(from);
            }
            StmtKind::IncDec { x, .. } => {
                self.places(&[*x], true);
                self.stored(*x);
            }
            StmtKind::Return(values) => {
                self.unit(values);
                self.returned(values, tree.stmts[stmt].span);
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
                let at = tree.stmts[stmt].span;
                let to = match kind {
                    BranchKind::Break => self.target(&self.breaks, *label),
                    BranchKind::Continue => self.target(&self.continues, *label),
                    // A label that comes later says where it is when
                    // the function is walked.
                    BranchKind::Goto => {
                        label.map(|label| match self.label_depths.contains_key(&label.name) {
                            true => self.label_block(label.name),
                            false => {
                                let through = self.block();
                                let frames = self.frames.clone();
                                self.gotos.push((through, label.name, frames, at));
                                through
                            }
                        })
                    }
                    BranchKind::Fallthrough => self.falls,
                };
                // The variables of the statements that the jump leaves
                // end with it.
                let depth = match (kind, label, to) {
                    (BranchKind::Goto, Some(label), _) => {
                        self.label_depths.get(&label.name).copied()
                    }
                    (BranchKind::Break | BranchKind::Continue, _, Some(to)) => {
                        self.depths.get(&to).copied()
                    }
                    _ => None,
                };
                for frame in depth.unwrap_or(usize::MAX)..self.frames.len() {
                    let ending = self.frames[frame].clone();
                    self.ended(&ending, at);
                }
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
                // The variables that the loop declares: each turn has
                // its own (CLO-8).
                let declared = self.events.len();
                if let Some(init) = init {
                    self.stmt(*init);
                }
                let written = |event: &Event| match event {
                    Event::Written(local) => Some(*local),
                    _ => None,
                };
                let assigned: Vec<LocalId> =
                    self.events[declared..].iter().filter_map(written).collect();
                let fresh: Vec<LocalId> = (self.events[declared..].iter())
                    .filter_map(|event| match event {
                        Event::Set(local) if !assigned.contains(local) => Some(*local),
                        _ => None,
                    })
                    .collect();
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
                self.depths.insert(after, self.frames.len());
                self.continues.push((label, next));
                self.depths.insert(next, self.frames.len());
                self.stmts(&body.stmts);
                self.breaks.pop();
                self.continues.pop();
                self.jump(next);
                self.start(next);
                self.events.extend(fresh.into_iter().map(Event::Fresh));
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
                let mut named_alias = None;
                for (at, &left) in lhs.iter().enumerate() {
                    let mut left = self.unparen(left);
                    // `&v` is a variable too.
                    let mut alias = false;
                    if let ExprKind::Unary {
                        op: UnaryOp::Addr,
                        x,
                    } = tree.exprs[left].kind
                    {
                        left = self.unparen(x);
                        alias = true;
                    }
                    if !define {
                        self.places(&[left], false);
                    }
                    let before = self.events.len();
                    self.stored(left);
                    // A second variable is each element in its turn.
                    elements |= at == 1 && self.events.len() > before;
                    if at == 1 {
                        self.ranged(left, *x, alias);
                    }
                    let declared = self.body.defs.get(&tree.exprs[left].span.lo).copied();
                    let named = declared.or_else(|| self.local(left));
                    let of = ranged.filter(|of| alias && self.watched.contains(of));
                    if let (1, Some(named), Some(of)) = (at, named, of) {
                        self.aliases.insert(named, of);
                        named_alias = Some(named);
                    }
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
                self.depths.insert(after, self.frames.len());
                self.continues.push((label, head));
                self.depths.insert(head, self.frames.len());
                // The loop reads what it ranges over until it is left.
                let looped = self.ranges(*x);
                self.stmts(&body.stmts);
                self.ranged_out(looped);
                if let Some(named) = named_alias {
                    self.aliases.remove(&named);
                }
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
                self.depths.insert(after, self.frames.len());
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
                self.depths.insert(after, self.frames.len());
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
        if scoped {
            self.ends(tree.stmts[stmt].span);
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
        // What the function is given holds pointers from the start.
        let holds = self.events[self.given..].iter().any(|event| match event {
            Event::Holds(_, from, to, _) => to > from,
            Event::Returned(..) | Event::Stored(..) | Event::Called(_) | Event::Left(..) => true,
            Event::Borrowed(_) => true,
            // What is changed through a pointer, those who call the
            // function are told.
            Event::Changed(change) => self.changes[*change as usize]
                .steps
                .contains(&borrows::Step::Target),
            _ => false,
        });
        let holds = holds || !self.broken.is_empty();
        let spawns = !self.spawns.is_empty();
        if !given && !moved && !wrote && !holds && !spawns {
            return;
        }
        self.blocks[self.current as usize].1 = self.events.len() as u32;
        let count = self.blocks.len();
        let after = lists(count, &self.edges, false);
        let before = lists(count, &self.edges, true);
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
        if spawns {
            self.spawning(&before, &after);
        }
        // What it does with pointers is looked at when every body is
        // walked.
        if holds {
            let problem = self.problem();
            self.found.lent.push(problem);
        }
    }

    /// Backward: for each variable that a function literal which `go`
    /// runs uses, whether it is touched after the `go` statement, and
    /// whether it is written then. A turn of a loop that comes later
    /// is after the statement too. A variable that another function
    /// literal uses, or whose address is taken, may be touched at any
    /// time.
    fn spawning(&mut self, before: &Lists, after: &Lists) {
        let mut members: FxHashMap<LocalId, u32> = FxHashMap::default();
        for &(_, literal) in &self.spawns {
            for capture in self.captured(literal) {
                let next = members.len() as u32 * 2;
                members.entry(capture.local).or_insert(next);
            }
        }
        let mut sets = Sets::new(members.len() * 2);
        let step = |sets: &mut Sets, set: u32, event: &Event| {
            let (local, touched, written) = match *event {
                Event::Read(local, _)
                | Event::Given(local, _)
                | Event::Moved(local, _)
                | Event::Wrote(local, _) => (local, true, None),
                Event::Written(local) => (local, true, Some(true)),
                Event::Set(local) | Event::Fresh(local) => (local, false, Some(false)),
                _ => return set,
            };
            let Some(&member) = members.get(&local) else {
                return set;
            };
            let set = sets.put(set, member, touched);
            match written {
                Some(written) => sets.put(set, member + 1, written),
                None => set,
            }
        };
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
        // What another function literal does to a variable.
        let mut elsewhere: FxHashMap<LocalId, Vec<(ExprId, bool)>> = FxHashMap::default();
        for (&literal, captures) in self.captures {
            for capture in captures
                .iter()
                .filter(|capture| members.contains_key(&capture.local))
            {
                let writes = capture.how != crate::object::CaptureUse::Read;
                elsewhere
                    .entry(capture.local)
                    .or_default()
                    .push((literal, writes));
            }
        }
        for block in 0..count {
            let mut set = leaving(&mut sets, &entering, block);
            let (first, last) = self.blocks[block];
            for at in (first as usize..last as usize).rev() {
                if let Event::Spawned(spawn) = self.events[at] {
                    let (span, literal) = self.spawns[spawn as usize];
                    for capture in self.captured(literal) {
                        let local = capture.local;
                        if !self.own(local) {
                            continue;
                        }
                        let member = members[&local];
                        let others = elsewhere.get(&local).map_or(&[][..], |all| &all[..]);
                        let mut others = others.iter().filter(|(other, _)| *other != literal);
                        let other = others.clone().next().is_some();
                        let addressed = self.addressed.contains(&local);
                        self.found.spawned.push(Spawned {
                            at: span,
                            local,
                            how: capture.how,
                            touched: sets.has(set, member) || other || addressed,
                            written: sets.has(set, member + 1)
                                || others.any(|&(_, writes)| writes)
                                || addressed,
                        });
                    }
                }
                set = step(&mut sets, set, &self.events[at]);
            }
        }
        self.found.steps += sets.steps + self.events.len() as u64;
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
                    Event::Holds(..)
                    | Event::Changed(_)
                    | Event::Returned(..)
                    | Event::Stored(..)
                    | Event::Called(_)
                    | Event::Left(..)
                    | Event::Borrowed(_)
                    | Event::Written(_)
                    | Event::Fresh(_)
                    | Event::Spawned(_) => continue,
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
            Event::Holds(..)
            | Event::Changed(_)
            | Event::Returned(..)
            | Event::Stored(..)
            | Event::Called(_)
            | Event::Left(..)
            | Event::Borrowed(_)
            | Event::Written(_)
            | Event::Fresh(_)
            | Event::Spawned(_) => None,
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
            Event::Wrote(..)
            | Event::Holds(..)
            | Event::Changed(_)
            | Event::Returned(..)
            | Event::Stored(..)
            | Event::Called(_)
            | Event::Left(..)
            | Event::Borrowed(_)
            | Event::Written(_)
            | Event::Fresh(_)
            | Event::Spawned(_) => None,
        };
        // What a function literal uses or a pointer leads to may be read
        // at any time.
        let found = self.not_read_later(before, after, asked, effect);
        let mut lost: FxHashSet<(LocalId, u32)> = FxHashSet::default();
        for &at in &found {
            if let Event::Wrote(local, span) = self.events[at]
                && !self.reached.contains(&local)
            {
                lost.insert((local, span.lo));
            }
        }
        for at in found {
            let Event::Wrote(local, span) = self.events[at] else {
                continue;
            };
            // A write through the `&` variable of a loop is lost if
            // neither the variable nor the parameter is read later.
            let through = self.alias_writes.get(&span.lo);
            let read =
                through.is_some_and(|&alias| alias == local || !lost.contains(&(alias, span.lo)));
            if lost.contains(&(local, span.lo)) && !read {
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
pub(super) type Lists = (Vec<u32>, Vec<u32>);

/// For each block, the blocks after it, or with `back` those before it.
pub(super) fn lists(count: usize, edges: &[(u32, u32)], back: bool) -> Lists {
    let ends = |&(from, to): &(u32, u32)| match back {
        true => (to, from),
        false => (from, to),
    };
    let mut starts = vec![0u32; count + 1];
    for edge in edges {
        starts[ends(edge).0 as usize + 1] += 1;
    }
    for at in 0..count {
        starts[at + 1] += starts[at];
    }
    let mut filled = starts.clone();
    let mut all = vec![0u32; edges.len()];
    for edge in edges {
        let (from, to) = ends(edge);
        all[filled[from as usize] as usize] = to;
        filled[from as usize] += 1;
    }
    (starts, all)
}

pub(super) fn neighbours(lists: &Lists, block: usize) -> &[u32] {
    let (starts, all) = lists;
    &all[starts[block] as usize..starts[block + 1] as usize]
}
