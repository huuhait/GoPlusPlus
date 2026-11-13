//! What the pointers in the variables of a function point into, and
//! where that is changed or ends while a pointer is still used (OWN-B1,
//! OWN-B2, OWN-B3, RNG-3).
//!
//! A place is a variable and a way into it: fields, elements, what a
//! pointer points to, what a handle holds. `&place` is a loan of the
//! place. The walk of `moves.rs` says where a variable gets what may
//! hold loans, where a place is changed, and what is returned; the
//! passes here go over its graph. Forward: which loans each variable
//! may hold. Backward: which variables are read later. A place is not
//! changed while a variable that is read later holds a loan of the
//! place or of a part of it, and no loan of what ends with the function
//! is returned.
//!
//! A pointer may be read and written through another one: what is read
//! through a pointer is what the variables hold that it points into,
//! and what is stored through one they hold from then on. What comes
//! from outside the function (in a parameter, from a call, from a
//! variable of the package) points to what outlives the function: no
//! pointer to a variable of the function is stored behind it.

use std::cmp::Reverse;
use std::collections::BinaryHeap;

use super::moves::{EMPTY, Event, Lists, Sets, Walk, neighbours};
use super::*;

/// What the walk needs of the checker to say what a place is.
pub(super) struct Places<'a> {
    pub types: &'a Types,
    pub memo: &'a FxHashMap<ExprId, Operand>,
    pub selections: &'a FxHashMap<ExprId, Selection>,
    pub locals: &'a FxHashMap<LocalId, Operand>,
    pub base: Spur,
}

/// A step from a place to a part of it.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub(super) enum Step {
    Field(u32),
    /// An element or an entry: which, if a constant says it.
    At(Option<u64>),
    /// What a pointer points to: it is no part of the value that has
    /// the pointer.
    Target,
    /// What a `Unique` or a `Shared` holds, which ends with it.
    Held,
}

/// What a variable may get loans from.
#[derive(Clone)]
pub(super) enum Source {
    /// `&place`: the variable at its root, the way, and where.
    Place(LocalId, Vec<Step>, Span),
    /// What another variable holds, read through as many pointers as
    /// the number says: through every one with `ALL`.
    Holder(LocalId, u8),
    /// A pointer that the function does not make: into what outlives
    /// it.
    Outside,
}

/// Through every pointer, however many.
const ALL: u8 = u8::MAX;

/// The variable that stands for what is outside the function.
const OUTSIDE: LocalId = LocalId(u32::MAX);

/// How a place is changed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) enum What {
    Assign,
    Grow,
    Clear,
    Move,
    /// Its variable is declared again: a loop came around.
    Declare,
    /// The statement that its variable is declared in ends.
    End,
}

pub(super) struct Change {
    pub root: LocalId,
    pub steps: Vec<Step>,
    pub at: Span,
    pub what: What,
}

/// A loop that ranges over a place: it reads the place where it is
/// until it is left (RNG-3).
pub(super) struct Ranged {
    pub root: LocalId,
    pub steps: Vec<Step>,
    pub at: Span,
    /// A string: its bytes are counted while the loop walks them, so
    /// only what the loop can see is refused (RNG-3a).
    pub text: bool,
    /// The loop around it that ranges over a place of the same
    /// variable.
    pub outer: Option<u32>,
    /// The events of its body: from the first number to the second.
    pub events: (u32, u32),
}

/// A place that is changed while something still points into it.
pub(super) struct Broken {
    pub at: Span,
    pub what: What,
    /// The variable of the place.
    pub of: LocalId,
    /// The variable that holds the loan; `None` for a loop that ranges
    /// over the place.
    pub holder: Option<LocalId>,
    /// Where the loan is made.
    pub made: Span,
}

/// A loan of what ends with the function that is returned, or stored
/// behind a pointer of `behind` that leads out of the function.
pub(super) struct Escaped {
    pub at: Span,
    pub of: LocalId,
    pub made: Span,
    pub behind: Option<LocalId>,
}

/// How the way of a change lies to the way of a loan.
enum Lies {
    /// The change replaces what the loan is a part of.
    Above,
    Same,
    Apart,
}

fn lies(change: &[Step], loan: &[Step]) -> Lies {
    for (at, step) in change.iter().enumerate() {
        match (step, loan.get(at)) {
            (_, None) => return Lies::Apart,
            (Step::At(Some(a)), Some(Step::At(Some(b)))) if a != b => return Lies::Apart,
            (Step::At(_), Some(Step::At(_))) => {}
            (a, Some(b)) if a != b => return Lies::Apart,
            _ => {}
        }
    }
    match change.len() == loan.len() {
        true => Lies::Same,
        false => Lies::Above,
    }
}

impl<'a> Walk<'a> {
    /// Whether a value of the type may hold a pointer (OWN-B5).
    fn may_point(&self, ty: TypeId) -> bool {
        self.places
            .is_some_and(|places| places.types.holds_pointer(ty))
    }

    fn type_of(&self, expr: ExprId) -> Option<TypeId> {
        self.places?.memo.get(&expr).map(|operand| operand.ty)
    }

    fn local_type(&self, local: LocalId) -> Option<TypeId> {
        self.places?.locals.get(&local).map(|operand| operand.ty)
    }

    /// The steps through the pointers and the handles that a value of
    /// the type is read through, and the type behind them.
    fn through(&self, mut ty: TypeId, steps: &mut Vec<Step>) -> TypeId {
        let Some(places) = self.places else {
            return ty;
        };
        loop {
            match &*places.types.get(places.types.underlying(ty)) {
                Type::Pointer(to) => {
                    steps.push(Step::Target);
                    ty = *to;
                }
                Type::Unique(to) | Type::Shared(to) => {
                    steps.push(Step::Held);
                    ty = *to;
                }
                _ => return ty,
            }
        }
    }

    /// The place that the expression is: the variable at its root and
    /// the way from it. `None` for what is no place of a variable of
    /// the function.
    pub(super) fn place(&self, expr: ExprId) -> Option<(LocalId, Vec<Step>)> {
        let places = self.places?;
        let types = places.types;
        // The parts on the way to the variable, the outermost first,
        // each with what it is a part of.
        let mut parts: Vec<(ExprId, ExprId)> = Vec::new();
        let mut at = self.unparen(expr);
        let root = loop {
            let inner = match &self.tree.exprs[at].kind {
                ExprKind::Name(name) => {
                    // `base` is the receiver (TYP-20).
                    if *name == places.base && !self.body.uses.contains_key(&at) {
                        let receiver = |local: &crate::Local| local.kind == LocalKind::Receiver;
                        let found = self.body.locals.iter().position(receiver)?;
                        break LocalId(found as u32);
                    }
                    break self.local(at)?;
                }
                ExprKind::Selector { x, .. } => {
                    let selection = places.selections.get(&at)?;
                    let mut inner = self.unparen(*x);
                    match selection.kind {
                        SelectionKind::Field => {}
                        // `base[Parent]` is the receiver too.
                        SelectionKind::BaseField => {
                            if let ExprKind::Index { x, .. } = self.tree.exprs[inner].kind {
                                inner = self.unparen(x);
                            }
                        }
                        _ => return None,
                    }
                    inner
                }
                ExprKind::Index { x, indices } if indices.len() == 1 => self.unparen(*x),
                ExprKind::Slice { x, .. } => self.unparen(*x),
                ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => self.unparen(*x),
                _ => return None,
            };
            parts.push((at, inner));
            at = inner;
        };
        let mut steps = Vec::new();
        for &(part, inner) in parts.iter().rev() {
            // The receiver has no type as an expression.
            let of = match &self.tree.exprs[inner].kind {
                ExprKind::Name(_) if self.type_of(inner).is_none() => self.local_type(root)?,
                _ => self.type_of(inner)?,
            };
            match &self.tree.exprs[part].kind {
                ExprKind::Selector { .. } => {
                    let selection = places.selections.get(&part)?;
                    let mut ty = of;
                    for &step in &selection.path {
                        ty = self.through(ty, &mut steps);
                        steps.push(Step::Field(step));
                        ty = match step & crate::types::PARENT != 0 {
                            true => {
                                let parents = types.parents(ty);
                                *parents.get((step & !crate::types::PARENT) as usize)?
                            }
                            false => match &*types.get(types.underlying(ty)) {
                                Type::Struct(fields) => fields.get(step as usize)?.ty,
                                _ => return None,
                            },
                        };
                    }
                }
                ExprKind::Index { indices, .. } => {
                    let ty = self.through(of, &mut steps);
                    let indexed = matches!(
                        &*types.get(types.underlying(ty)),
                        Type::Slice(_) | Type::Array { .. } | Type::Map { .. }
                    );
                    if !indexed {
                        return None;
                    }
                    let known = match places.memo.get(&indices[0]).map(|operand| &operand.mode) {
                        Some(Mode::Const(value)) => value.to_int().low_bits(),
                        _ => None,
                    };
                    steps.push(Step::At(known));
                }
                ExprKind::Slice { .. } => {
                    self.through(of, &mut steps);
                    steps.push(Step::At(None));
                }
                _ => steps.push(match &*types.get(types.underlying(of)) {
                    Type::Unique(_) | Type::Shared(_) => Step::Held,
                    _ => Step::Target,
                }),
            }
        }
        Some((root, steps))
    }

    /// What the value of the expression may get loans from: every
    /// `&place` in it, every variable in it that may hold some, the
    /// receiver of a method that it calls, and what is outside the
    /// function for what the function does not make.
    fn sources_of(&mut self, expr: ExprId, found: &mut Vec<Source>) {
        let tree = self.tree;
        // Each with how many pointers its value is read through.
        let mut rest = vec![(expr, 0u8)];
        while let Some((expr, hops)) = rest.pop() {
            self.found.steps += 1;
            let span = tree.exprs[expr].span;
            match &tree.exprs[expr].kind {
                ExprKind::Paren(x) | ExprKind::Assert { x, .. } => rest.push((*x, hops)),
                ExprKind::Name(_) => {
                    let Some(local) = self.local(expr) else {
                        // A variable of the package, or of a function
                        // around a function literal.
                        if self.type_of(expr).is_some_and(|ty| self.may_point(ty)) {
                            found.push(Source::Outside);
                        }
                        continue;
                    };
                    let Some(ty) = self.local_type(local) else {
                        continue;
                    };
                    if self.may_point(ty) {
                        found.push(Source::Holder(local, hops));
                    }
                    // A handle where a pointer is wanted is lent
                    // (OWN-P6).
                    let mut steps = Vec::new();
                    self.through(ty, &mut steps);
                    if steps.first() == Some(&Step::Held) {
                        found.push(Source::Place(local, vec![Step::Held], span));
                    }
                }
                ExprKind::Unary {
                    op: UnaryOp::Addr,
                    x,
                } => match self.place(*x) {
                    Some((root, steps)) => {
                        found.push(Source::Place(root, steps, span));
                        // A call may give back what it reads there.
                        if hops == ALL {
                            rest.push((*x, ALL));
                        }
                    }
                    None => rest.push((*x, hops)),
                },
                // A variable of another package.
                ExprKind::Selector { .. }
                    if self.body.uses.contains_key(&expr)
                        && self.type_of(expr).is_some_and(|ty| self.may_point(ty)) =>
                {
                    found.push(Source::Outside);
                }
                ExprKind::Unary { x, .. }
                | ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Slice { x, .. } => {
                    // A place in a variable is read through the
                    // pointers on the way to it.
                    let through = match hops {
                        ALL => None,
                        _ => self.place(expr),
                    };
                    match through {
                        Some((root, steps)) => {
                            let more = steps.iter().filter(|step| **step == Step::Target).count();
                            let hops = (hops as usize + more).min(ALL as usize) as u8;
                            let holds = self.local_type(root).is_some_and(|ty| self.may_point(ty));
                            if holds {
                                found.push(Source::Holder(root, hops));
                            }
                        }
                        None => rest.push((*x, ALL)),
                    }
                }
                ExprKind::CompositeLit { elems, .. } => {
                    rest.extend(elems.iter().map(|&elem| (elem, hops)));
                }
                ExprKind::KeyValue { value, .. } => rest.push((*value, hops)),
                ExprKind::Call { fun, args, .. } => {
                    // What a call gives holds nothing of its operands
                    // if it can hold no pointer.
                    let points = self.type_of(expr).is_none_or(|ty| self.may_point(ty));
                    if !points {
                        continue;
                    }
                    // It may give back what it reads through them, and
                    // what is outside the function.
                    found.push(Source::Outside);
                    rest.extend(args.iter().map(|&arg| (arg, ALL)));
                    let callee = self.unparen(*fun);
                    if let ExprKind::Selector { x, .. } = tree.exprs[callee].kind {
                        // A method may give back a pointer into its
                        // receiver: into what a pointer or a handle
                        // leads to, if the receiver is one.
                        if let Some((root, mut steps)) = self.place(x) {
                            if let Some(ty) = self.type_of(self.unparen(x)) {
                                self.through(ty, &mut steps);
                            }
                            found.push(Source::Place(root, steps, span));
                        }
                        rest.push((x, ALL));
                    }
                }
                _ => {}
            }
        }
    }

    /// The function is given the variable: a pointer in it points to
    /// what is outside the function.
    pub(super) fn given_from_outside(&mut self, local: LocalId, at: Span) {
        let holds = self.local_type(local).is_some_and(|ty| self.may_point(ty));
        if self.places.is_none() || !holds {
            return;
        }
        let from = self.sources.len() as u32;
        self.sources
            .push(Source::Place(local, vec![Step::Target], at));
        self.events.push(Event::Holds(local, from, from + 1, true));
    }

    /// The variable gets the values of the expressions: what it may
    /// point into from now on. With `only`, the whole variable is set.
    fn holds(&mut self, local: LocalId, values: &[ExprId], only: bool) {
        let Some(ty) = self.local_type(local) else {
            return;
        };
        if !self.may_point(ty) {
            return;
        }
        let from = self.sources.len() as u32;
        let mut found = std::mem::take(&mut self.sources);
        for &value in values {
            self.sources_of(value, &mut found);
        }
        self.sources = found;
        let to = self.sources.len() as u32;
        self.events.push(Event::Holds(local, from, to, only));
    }

    /// The places get the values: each the one at its place, or all the
    /// results of one call.
    pub(super) fn assigned(&mut self, places: &[ExprId], values: &[ExprId]) {
        if self.places.is_none() {
            return;
        }
        for (at, &place) in places.iter().enumerate() {
            let inner = self.unparen(place);
            let named = matches!(self.tree.exprs[inner].kind, ExprKind::Name(_));
            let declared = self.body.defs.get(&self.tree.exprs[inner].span.lo);
            // The pointers on the way to the place, after the first.
            let (root, behind) = match declared.copied().filter(|_| named) {
                Some(local) => (local, None),
                None => match self.place(inner) {
                    Some((root, steps)) => {
                        let pointers = steps.iter().filter(|step| **step == Step::Target);
                        (root, pointers.count().checked_sub(1))
                    }
                    None => continue,
                },
            };
            // A part that can hold no pointer gets none.
            let part = self.type_of(inner).filter(|_| !named);
            if part.is_some_and(|ty| !self.may_point(ty)) {
                continue;
            }
            let values = match places.len() == values.len() {
                true => &values[at..=at],
                false => values,
            };
            match behind {
                None => self.holds(root, values, named),
                Some(behind) => {
                    let from = self.sources.len() as u32;
                    let mut found = std::mem::take(&mut self.sources);
                    for &value in values {
                        self.sources_of(value, &mut found);
                    }
                    self.sources = found;
                    let to = self.sources.len() as u32;
                    let behind = behind.min(ALL as usize - 1) as u8;
                    let at = self.tree.exprs[inner].span;
                    self.events.push(Event::Stored(root, from, to, behind, at));
                }
            }
        }
    }

    /// The variables that a declaration gives values.
    pub(super) fn declared(&mut self, locals: &[LocalId], values: &[ExprId]) {
        if self.places.is_none() {
            return;
        }
        for (at, &local) in locals.iter().enumerate() {
            let values = match locals.len() == values.len() {
                true => &values[at..=at],
                false => values,
            };
            self.holds(local, values, true);
        }
    }

    /// Whether the change of the place breaks what the loop reads: it
    /// replaces the place or what the place is in. `through` says that
    /// the change is made through a pointer.
    fn unranges(loop_: &Ranged, way: &[Step], through: bool) -> bool {
        match lies(way, &loop_.steps) {
            Lies::Above => !loop_.steps[way.len()..].contains(&Step::Target),
            Lies::Same => !through || !loop_.text,
            Lies::Apart => false,
        }
    }

    /// The loops around here that range over a place of the variable,
    /// the innermost first: a change that the loop can see is refused
    /// where it is written.
    fn ranged_over(&mut self, change: &Change) {
        // A `move` under a loop is said by the walk.
        if change.what == What::Move {
            return;
        }
        let mut next = self.looping.get(&change.root).copied();
        while let Some(at) = next {
            self.found.steps += 1;
            let loop_ = &self.loops[at as usize];
            if Self::unranges(loop_, &change.steps, false) {
                self.found.broken.push(Broken {
                    at: change.at,
                    what: change.what,
                    of: change.root,
                    holder: None,
                    made: loop_.at,
                });
                return;
            }
            next = loop_.outer;
        }
    }

    /// The place is changed.
    pub(super) fn change(&mut self, place: ExprId, what: What) {
        if self.places.is_none() {
            return;
        }
        let inner = self.unparen(place);
        let span = self.tree.exprs[inner].span;
        let named = matches!(self.tree.exprs[inner].kind, ExprKind::Name(_));
        let declared = self.body.defs.get(&span.lo).copied().filter(|_| named);
        let (root, steps, what) = match declared {
            Some(local) => {
                if let Some(frame) = self.frames.last_mut() {
                    frame.push(local);
                }
                (local, Vec::new(), What::Declare)
            }
            None => match self.place(inner) {
                Some((root, steps)) => (root, steps, what),
                None => return,
            },
        };
        let change = Change {
            root,
            steps,
            at: span,
            what,
        };
        self.ranged_over(&change);
        self.changes.push(change);
        self.events
            .push(Event::Changed(self.changes.len() as u32 - 1));
    }

    /// A variable of the function is declared by a declaration.
    pub(super) fn declares(&mut self, local: LocalId, at: Span) {
        if self.places.is_none() {
            return;
        }
        if let Some(frame) = self.frames.last_mut() {
            frame.push(local);
        }
        self.changes.push(Change {
            root: local,
            steps: Vec::new(),
            at,
            what: What::Declare,
        });
        self.events
            .push(Event::Changed(self.changes.len() as u32 - 1));
    }

    /// The statement ends: so do the variables that it declared.
    pub(super) fn ends(&mut self, at: Span) {
        let Some(frame) = self.frames.pop() else {
            return;
        };
        let end = Span {
            lo: at.hi.saturating_sub(1),
            ..at
        };
        for local in frame {
            self.changes.push(Change {
                root: local,
                steps: Vec::new(),
                at: end,
                what: What::End,
            });
            self.events
                .push(Event::Changed(self.changes.len() as u32 - 1));
            // What the variable held is held no more: a loop around
            // does not carry it to its next turn.
            self.events.push(Event::Holds(local, 0, 0, true));
        }
    }

    /// A loop ranges over the place of `x` from here on: which of the
    /// loops of the walk it is.
    pub(super) fn ranges(&mut self, x: ExprId) -> Option<u32> {
        let (root, steps) = self.place(x)?;
        let places = self.places?;
        // A string is counted, and what a pointer leads to is not the
        // function's to replace.
        let ty = self.type_of(self.unparen(x))?;
        let text = matches!(
            &*places.types.get(places.types.underlying(ty)),
            Type::Basic(Basic::String | Basic::UntypedString)
        );
        let ranged = matches!(
            &*places.types.get(places.types.underlying(ty)),
            Type::Slice(_) | Type::Map { .. }
        );
        if !ranged && !text {
            return None;
        }
        let at = self.loops.len() as u32;
        let from = self.events.len() as u32;
        self.loops.push(Ranged {
            root,
            steps,
            at: self.tree.exprs[x].span,
            text,
            outer: self.looping.insert(root, at),
            events: (from, from),
        });
        Some(at)
    }

    /// The body of the loop ends here.
    pub(super) fn ranged_out(&mut self, at: Option<u32>) {
        let Some(at) = at else {
            return;
        };
        let loop_ = &mut self.loops[at as usize];
        loop_.events.1 = self.events.len() as u32;
        match loop_.outer {
            Some(outer) => self.looping.insert(loop_.root, outer),
            None => self.looping.remove(&loop_.root),
        };
    }

    /// A variable of a range clause is each element in its turn: with
    /// `alias` it is the element itself (RNG-1a), otherwise it may hold
    /// what the elements hold.
    pub(super) fn ranged(&mut self, var: ExprId, x: ExprId, alias: bool) {
        if self.places.is_none() {
            return;
        }
        let inner = self.unparen(var);
        let span = self.tree.exprs[inner].span;
        let declared = self.body.defs.get(&span.lo).copied();
        let Some(local) = declared.or_else(|| self.local(inner)) else {
            return;
        };
        let from = self.sources.len() as u32;
        match (alias, self.place(x)) {
            (true, Some((root, mut steps))) => {
                if let Some(ty) = self.type_of(self.unparen(x)) {
                    self.through(ty, &mut steps);
                }
                steps.push(Step::At(None));
                let of = self.tree.exprs[x].span;
                self.sources.push(Source::Place(root, steps, of));
            }
            (false, Some((root, steps))) => {
                let holds = self.local_type(local).is_some_and(|ty| self.may_point(ty));
                if !holds {
                    return;
                }
                let pointers = steps.iter().filter(|step| **step == Step::Target).count();
                let hops = pointers.min(ALL as usize) as u8;
                self.sources.push(Source::Holder(root, hops));
            }
            _ => return,
        }
        let to = self.sources.len() as u32;
        self.events.push(Event::Holds(local, from, to, true));
    }

    /// The values are returned.
    pub(super) fn returned(&mut self, values: &[ExprId], at: Span) {
        if self.places.is_none() || values.is_empty() {
            return;
        }
        let from = self.sources.len() as u32;
        let mut found = std::mem::take(&mut self.sources);
        let each = values.len() == self.pointers.len();
        for (at, &value) in values.iter().enumerate() {
            let Some(ty) = self.type_of(value) else {
                continue;
            };
            // A handle is lent where the result is a pointer (OWN-P6).
            let mut steps = Vec::new();
            self.through(ty, &mut steps);
            let lent = steps.first() == Some(&Step::Held) && each && self.pointers[at];
            if self.may_point(ty) || lent {
                self.sources_of(value, &mut found);
            }
        }
        self.sources = found;
        let to = self.sources.len() as u32;
        if to > from {
            self.events.push(Event::Returned(from, to, at));
        }
    }

    // ---- the passes

    /// Which loans each variable may hold, which variables are read
    /// later, and with both: the places that are changed under a loan
    /// that is still used, and the loans that leave the function.
    pub(super) fn borrowed(&mut self, before: &Lists, after: &Lists) {
        let sources = std::mem::take(&mut self.sources);
        let changes = std::mem::take(&mut self.changes);
        // The loops over the places of each variable, for what is
        // changed through a pointer.
        let mut loops_of: FxHashMap<LocalId, Vec<u32>> = FxHashMap::default();
        for (at, loop_) in self.loops.iter().enumerate() {
            loops_of.entry(loop_.root).or_default().push(at as u32);
        }
        let count = self.blocks.len();
        let mut known = Known::new(self.within);
        let mut sets = Sets::new(ROOM);

        // Forward: the facts at the end of each block.
        let mut out = vec![EMPTY; count];
        let mut queued = vec![true; count];
        let mut queue: BinaryHeap<Reverse<u32>> = (0..count as u32).map(Reverse).collect();
        while let Some(Reverse(block)) = queue.pop() {
            let block = block as usize;
            queued[block] = false;
            let mut set = EMPTY;
            for &from in neighbours(before, block) {
                set = sets.join(set, out[from as usize]);
            }
            let (first, last) = self.blocks[block];
            for at in first as usize..last as usize {
                set = known.apply(&mut sets, set, &sources, self.events[at]);
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

        // Backward: the variables with loans that are read later.
        let mut members: FxHashMap<LocalId, u32> = FxHashMap::default();
        for &(holder, _) in &known.facts {
            let next = members.len() as u32;
            members.entry(holder).or_insert(next);
        }
        let mut reads = Sets::new(members.len());
        let effect = |event: &Event| match *event {
            Event::Read(local, _)
            | Event::Given(local, _)
            | Event::Moved(local, _)
            | Event::Wrote(local, _) => Some((local, true)),
            Event::Set(local) | Event::Holds(local, _, _, true) => Some((local, false)),
            _ => None,
        };
        let mut live = vec![EMPTY; count];
        let mut queued = vec![true; count];
        let mut queue: BinaryHeap<u32> = (0..count as u32).collect();
        while let Some(block) = queue.pop() {
            let block = block as usize;
            queued[block] = false;
            let mut set = EMPTY;
            for &to in neighbours(after, block) {
                set = reads.join(set, live[to as usize]);
            }
            let (first, last) = self.blocks[block];
            for event in self.events[first as usize..last as usize].iter().rev() {
                if let Some((local, read)) = effect(event)
                    && let Some(&member) = members.get(&local)
                {
                    set = reads.put(set, member, read);
                }
            }
            if set != live[block] {
                live[block] = set;
                for &from in neighbours(before, block) {
                    if !std::mem::replace(&mut queued[from as usize], true) {
                        queue.push(from);
                    }
                }
            }
        }

        // Both together, event by event.
        let locals = self.body.locals.len() as u32;
        let mut later: Vec<u32> = Vec::new();
        for block in 0..count {
            let (first, last) = (self.blocks[block].0 as usize, self.blocks[block].1 as usize);
            // What is read after each event of the block.
            later.clear();
            later.resize(last - first, EMPTY);
            let mut set = EMPTY;
            for &to in neighbours(after, block) {
                set = reads.join(set, live[to as usize]);
            }
            for at in (first..last).rev() {
                later[at - first] = set;
                if let Some((local, read)) = effect(&self.events[at])
                    && let Some(&member) = members.get(&local)
                {
                    set = reads.put(set, member, read);
                }
            }
            let mut set = EMPTY;
            for &from in neighbours(before, block) {
                set = sets.join(set, out[from as usize]);
            }
            // The variables that nothing which is read later points
            // into: true as long as variables only end.
            let mut unused: FxHashSet<LocalId> = FxHashSet::default();
            for at in first..last {
                let event = self.events[at];
                let ends = match event {
                    Event::Changed(change) => changes[change as usize].what == What::End,
                    Event::Holds(_, from, to, true) => from == to,
                    _ => false,
                };
                if !ends {
                    unused.clear();
                }
                let moved;
                let change = match event {
                    Event::Changed(change) => Some(&changes[change as usize]),
                    Event::Moved(local, span) => {
                        moved = Change {
                            root: local,
                            steps: Vec::new(),
                            at: span,
                            what: What::Move,
                        };
                        Some(&moved)
                    }
                    _ => None,
                };
                // The places that are written: through a pointer of a
                // variable, those that it may point into.
                let written = match change {
                    Some(change) if later[at - first] != EMPTY || !loops_of.is_empty() => {
                        known.written(&sets, set, change)
                    }
                    _ => Vec::new(),
                };
                // A change through a pointer under a loop that ranges
                // over what the pointer points into.
                if let Some(change) = change.filter(|_| !loops_of.is_empty()) {
                    'ranged: for (of, way, through) in &written {
                        if !through {
                            continue;
                        }
                        for &looped in loops_of.get(of).map_or(&[][..], |all| &all[..]) {
                            known.steps += 1;
                            let loop_ = &self.loops[looped as usize];
                            let inside =
                                loop_.events.0 <= at as u32 && (at as u32) < loop_.events.1;
                            if inside && Self::unranges(loop_, way, true) {
                                self.found.broken.push(Broken {
                                    at: change.at,
                                    what: change.what,
                                    of: *of,
                                    holder: None,
                                    made: loop_.at,
                                });
                                break 'ranged;
                            }
                        }
                    }
                }
                // Nothing is broken where no variable with a loan is read
                // later.
                if let Some(change) = change.filter(|_| later[at - first] != EMPTY) {
                    let read_later = |holder: LocalId| {
                        let member = members.get(&holder);
                        member.is_some_and(|&member| reads.has(later[at - first], member))
                    };
                    let pointed = change.steps.contains(&Step::Target);
                    'written: for (root, way, through) in &written {
                        for at in 0..known.under(*root, way).len() {
                            known.steps += 1;
                            let fact = known.under(*root, way)[at];
                            if !sets.has(set, fact) {
                                continue;
                            }
                            let (holder, loan) = known.facts[fact as usize];
                            let (_, held, made) = &known.loans[loan as usize];
                            let breaks = match lies(way, held) {
                                // A pointer that the place holds is
                                // replaced, not what it points to.
                                Lies::Above => !held[way.len()..].contains(&Step::Target),
                                // A write through a pointer to the
                                // place changes what is there, and the
                                // place stays: through the variable
                                // that holds the pointer too.
                                Lies::Same => !(*through || pointed && holder == change.root),
                                Lies::Apart => false,
                            };
                            let made = *made;
                            if breaks && known.used(&sets, set, holder, &read_later, &mut unused) {
                                self.found.broken.push(Broken {
                                    at: change.at,
                                    what: change.what,
                                    of: *root,
                                    holder: Some(holder),
                                    made,
                                });
                                // One report says that the change is
                                // refused.
                                break 'written;
                            }
                        }
                    }
                }
                // What ends with the function does not leave it: not as
                // a result, and not behind a pointer that leads out of
                // the function.
                let leaves = match event {
                    Event::Returned(from, to, span) => Some((from, to, span, None)),
                    Event::Stored(root, from, to, behind, span) => {
                        let places = known.read(&sets, set, root, behind);
                        let outside =
                            |&place: &u32| known.loans[place as usize].1.contains(&Step::Target);
                        let out = places.is_empty() || places.iter().any(outside);
                        out.then_some((from, to, span, Some(root)))
                    }
                    _ => None,
                };
                if let Some((from, to, span, behind)) = leaves {
                    let given = known.given(&sets, set, &sources[from as usize..to as usize]);
                    for loan in given {
                        let (of, way, made) = &known.loans[loan as usize];
                        // What a pointer points to is not the
                        // function's, and outlives it.
                        if of.0 < locals && !way.contains(&Step::Target) {
                            self.found.escaped.push(Escaped {
                                at: span,
                                of: *of,
                                made: *made,
                                behind,
                            });
                        }
                    }
                }
                set = known.apply(&mut sets, set, &sources, event);
            }
        }
        self.found.steps += known.steps + sets.steps + reads.steps;
        self.sources = sources;
        self.changes = changes;
    }
}

/// The step with any index, for an element.
fn any_index(step: Step) -> Step {
    match step {
        Step::At(_) => Step::At(None),
        other => other,
    }
}

/// The most facts that the sets have room for.
const ROOM: usize = 1 << 24;

/// What is known of the loans while the passes run. A loan is a place
/// and where it was made; a fact says that a variable may hold a loan.
struct Known {
    /// The loan of what is outside the function.
    outside: u32,
    loans: Vec<(LocalId, Vec<Step>, Span)>,
    loan_ids: FxHashMap<(LocalId, Vec<Step>), u32>,
    facts: Vec<(LocalId, u32)>,
    fact_ids: FxHashMap<(LocalId, u32), u32>,
    /// The facts of each variable that holds, those of the loans of
    /// each variable, and of these the ones whose way starts with a
    /// step (with any index for an element).
    of_holder: FxHashMap<LocalId, Vec<u32>>,
    of_root: FxHashMap<LocalId, Vec<u32>>,
    of_part: FxHashMap<(LocalId, Step), Vec<u32>>,
    steps: u64,
}

impl Known {
    fn new(function: Span) -> Known {
        let mut known = Known {
            outside: 0,
            loans: Vec::new(),
            loan_ids: FxHashMap::default(),
            facts: Vec::new(),
            fact_ids: FxHashMap::default(),
            of_holder: FxHashMap::default(),
            of_root: FxHashMap::default(),
            of_part: FxHashMap::default(),
            steps: 0,
        };
        known.outside = known.loan(OUTSIDE, vec![Step::Target], function);
        known
    }

    /// The facts of the loans of places of the variable.
    fn rooted(&self, root: LocalId) -> &[u32] {
        self.of_root.get(&root).map_or(&[], |all| &all[..])
    }

    /// The facts of the loans that a change of the place may break:
    /// those of the place, of what it is in and of its parts. A loan of
    /// another part of the variable is none.
    fn under(&self, root: LocalId, way: &[Step]) -> &[u32] {
        match way.first() {
            None => self.rooted(root),
            Some(&first) => {
                let key = (root, any_index(first));
                self.of_part.get(&key).map_or(&[], |all| &all[..])
            }
        }
    }

    /// The loans that the variable holds in the set.
    fn held(&mut self, sets: &Sets, set: u32, holder: LocalId, into: &mut Vec<u32>) {
        let facts = self.of_holder.get(&holder).map_or(&[][..], |all| &all[..]);
        self.steps += facts.len() as u64;
        let held = facts.iter().filter(|&&fact| sets.has(set, fact));
        into.extend(held.map(|&fact| self.facts[fact as usize].1));
    }

    fn loan(&mut self, root: LocalId, way: Vec<Step>, made: Span) -> u32 {
        let next = self.loans.len() as u32;
        let key = (root, way);
        if let Some(&loan) = self.loan_ids.get(&key) {
            return loan;
        }
        self.loans.push((key.0, key.1.clone(), made));
        self.loan_ids.insert(key, next);
        next
    }

    /// The set with the fact that the variable may hold the loan.
    fn hold(&mut self, sets: &mut Sets, set: u32, holder: LocalId, loan: u32) -> u32 {
        let next = self.facts.len() as u32;
        let fact = match self.fact_ids.get(&(holder, loan)) {
            Some(&fact) => fact,
            None if next as usize >= ROOM => return set,
            None => {
                self.fact_ids.insert((holder, loan), next);
                self.facts.push((holder, loan));
                self.of_holder.entry(holder).or_default().push(next);
                let (root, way, _) = &self.loans[loan as usize];
                self.of_root.entry(*root).or_default().push(next);
                if let Some(&first) = way.first() {
                    let key = (*root, any_index(first));
                    self.of_part.entry(key).or_default().push(next);
                }
                next
            }
        };
        sets.put(set, fact, true)
    }

    /// The loans in a value that is read from the variable through as
    /// many pointers as `hops` says: what the variables hold that its
    /// loans are places of. What is read through a pointer from outside
    /// the function is from outside too.
    fn read(&mut self, sets: &Sets, set: u32, holder: LocalId, hops: u8) -> Vec<u32> {
        let mut now = Vec::new();
        self.held(sets, set, holder, &mut now);
        let mut all = Vec::new();
        let mut seen: FxHashSet<LocalId> = FxHashSet::default();
        let mut left = hops;
        while left > 0 && !now.is_empty() {
            if hops == ALL {
                all.extend(&now);
            } else {
                left -= 1;
            }
            let mut next = Vec::new();
            let mut outside = false;
            for loan in now {
                let (of, way, _) = &self.loans[loan as usize];
                let of = *of;
                if way.contains(&Step::Target) {
                    outside = true;
                } else if seen.insert(of) || hops != ALL {
                    self.held(sets, set, of, &mut next);
                }
            }
            if outside {
                next.push(self.outside);
            }
            next.sort_unstable();
            next.dedup();
            // Outside leads to outside and nowhere else.
            if hops == ALL && next.len() == 1 && outside && !seen.insert(OUTSIDE) {
                next.clear();
            }
            now = next;
        }
        if hops == ALL {
            all.extend(&now);
            all.sort_unstable();
            all.dedup();
            return all;
        }
        now
    }

    /// The loans that the sources give, where the variables hold what
    /// the set says. A place behind a pointer of a variable is a place
    /// in what the variable points into.
    fn given(&mut self, sets: &Sets, set: u32, sources: &[Source]) -> Vec<u32> {
        let mut given: Vec<u32> = Vec::new();
        for source in sources {
            self.steps += 1;
            match source {
                Source::Outside => given.push(self.outside),
                Source::Holder(holder, hops) => given.extend(self.read(sets, set, *holder, *hops)),
                Source::Place(root, way, made) => {
                    // Each pointer on the way leads into what the
                    // variable before it holds.
                    let mut parts = way.split(|step| *step == Step::Target);
                    let first = parts.next().unwrap_or(&[]);
                    let mut places = vec![(*root, first.to_vec())];
                    for part in parts {
                        let mut next = Vec::new();
                        for (of, mut way) in places {
                            let mut held = Vec::new();
                            if !way.contains(&Step::Target) {
                                self.held(sets, set, of, &mut held);
                            }
                            // A pointer that is not known, or comes
                            // from outside, leads behind its variable.
                            let outside = held.contains(&self.outside);
                            held.retain(|&loan| loan != self.outside);
                            for &loan in &held {
                                let (of, held, _) = &self.loans[loan as usize];
                                let mut whole = held.clone();
                                whole.extend(part);
                                next.push((*of, whole));
                            }
                            if held.is_empty() || outside {
                                way.push(Step::Target);
                                way.extend(part);
                                next.push((of, way));
                            }
                        }
                        places = next;
                    }
                    for (of, way) in places {
                        given.push(self.loan(of, way, *made));
                    }
                }
            }
        }
        given
    }

    /// The set after the event: what a variable holds from then on.
    fn apply(&mut self, sets: &mut Sets, mut set: u32, sources: &[Source], event: Event) -> u32 {
        match event {
            Event::Holds(holder, from, to, only) => {
                let given = self.given(sets, set, &sources[from as usize..to as usize]);
                if only {
                    for at in 0..self.of_holder.get(&holder).map_or(0, Vec::len) {
                        set = sets.put(set, self.of_holder[&holder][at], false);
                    }
                }
                for loan in given {
                    set = self.hold(sets, set, holder, loan);
                }
            }
            // The variables that the pointer leads into hold what is
            // stored there.
            Event::Stored(root, from, to, behind, _) => {
                let given = self.given(sets, set, &sources[from as usize..to as usize]);
                for place in self.read(sets, set, root, behind) {
                    let (of, way, _) = &self.loans[place as usize];
                    let of = *of;
                    if way.contains(&Step::Target) {
                        continue;
                    }
                    for &loan in &given {
                        set = self.hold(sets, set, of, loan);
                    }
                }
            }
            _ => {}
        }
        set
    }

    /// The places that the change writes, each with whether it is
    /// written through a pointer: through a pointer of a variable,
    /// those that the variable may point into.
    fn written(
        &mut self,
        sets: &Sets,
        set: u32,
        change: &Change,
    ) -> Vec<(LocalId, Vec<Step>, bool)> {
        let behind = change.steps.iter().position(|step| *step == Step::Target);
        let mut written = Vec::new();
        if let Some(behind) = behind {
            let mut held = Vec::new();
            self.held(sets, set, change.root, &mut held);
            for loan in held {
                // What is outside is written behind the variable.
                if loan == self.outside {
                    continue;
                }
                let (of, held, _) = &self.loans[loan as usize];
                // What the function is given is written where it is.
                let given = *of == change.root && held[..] == [Step::Target];
                let mut whole = held.clone();
                whole.extend(&change.steps[behind + 1..]);
                written.push((*of, whole, !given));
            }
        }
        if written.is_empty() {
            written.push((change.root, change.steps.clone(), false));
        }
        written
    }

    /// Whether the variable is read later, or one that points into it:
    /// what it holds is read through the other. `unused` are those of
    /// which this is known not to be so, and gets what is found.
    fn used(
        &mut self,
        sets: &Sets,
        set: u32,
        holder: LocalId,
        read_later: &impl Fn(LocalId) -> bool,
        unused: &mut FxHashSet<LocalId>,
    ) -> bool {
        let mut rest = vec![holder];
        let mut seen = Vec::new();
        while let Some(next) = rest.pop() {
            self.steps += 1;
            if !unused.insert(next) {
                continue;
            }
            seen.push(next);
            if read_later(next) {
                for local in seen {
                    unused.remove(&local);
                }
                return true;
            }
            let pointing = self
                .rooted(next)
                .iter()
                .filter(|&&fact| sets.has(set, fact));
            rest.extend(pointing.map(|&fact| self.facts[fact as usize].0));
        }
        false
    }
}
