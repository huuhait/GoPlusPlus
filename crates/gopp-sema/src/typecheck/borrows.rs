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
//! Only the variables of the function hold loans here: what is stored
//! elsewhere is not followed.

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
    /// What another variable holds.
    Holder(LocalId),
}

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

/// A loan of what ends with the function that is returned.
pub(super) struct Escaped {
    pub at: Span,
    pub of: LocalId,
    pub made: Span,
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
    /// Whether a value of the type may hold a pointer: a pointer, or a
    /// struct, an array or an option with one. What is on the heap
    /// holds none (OWN-B5).
    fn may_point(&mut self, ty: TypeId) -> bool {
        let Some(places) = self.places else {
            return false;
        };
        if let Some(&known) = self.pointing.get(&ty) {
            return known;
        }
        let types = places.types;
        let mut seen = FxHashSet::default();
        let mut rest = vec![ty];
        let mut found = false;
        while let Some(next) = rest.pop() {
            self.found.steps += 1;
            if !seen.insert(next) {
                continue;
            }
            match &*types.get(types.underlying(next)) {
                Type::Pointer(_) => {
                    found = true;
                    break;
                }
                Type::Struct(fields) => {
                    rest.extend(fields.iter().map(|field| field.ty));
                    rest.extend(types.parents(next));
                }
                Type::Array { elem, .. } | Type::Option(elem) => rest.push(*elem),
                Type::Tuple(parts) => rest.extend(parts.iter()),
                _ => {}
            }
        }
        self.pointing.insert(ty, found);
        found
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
    /// `&place` in it, every variable in it that may hold some, and the
    /// receiver of a method that it calls.
    fn sources_of(&mut self, expr: ExprId, found: &mut Vec<Source>) {
        let tree = self.tree;
        let mut rest = vec![expr];
        while let Some(expr) = rest.pop() {
            self.found.steps += 1;
            let span = tree.exprs[expr].span;
            match &tree.exprs[expr].kind {
                ExprKind::Paren(x) | ExprKind::Assert { x, .. } => rest.push(*x),
                ExprKind::Name(_) => {
                    let Some(local) = self.local(expr) else {
                        continue;
                    };
                    let Some(ty) = self.local_type(local) else {
                        continue;
                    };
                    if self.may_point(ty) {
                        found.push(Source::Holder(local));
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
                    Some((root, steps)) => found.push(Source::Place(root, steps, span)),
                    None => rest.push(*x),
                },
                ExprKind::Unary { x, .. } | ExprKind::Selector { x, .. } => rest.push(*x),
                ExprKind::Index { x, .. } | ExprKind::Slice { x, .. } => rest.push(*x),
                ExprKind::CompositeLit { elems, .. } => rest.extend(elems.iter()),
                ExprKind::KeyValue { value, .. } => rest.push(*value),
                ExprKind::Call { fun, args, .. } => {
                    // What a call gives holds nothing of its operands
                    // if it can hold no pointer.
                    let points = self.type_of(expr).is_none_or(|ty| self.may_point(ty));
                    if !points {
                        continue;
                    }
                    rest.extend(args.iter());
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
                        rest.push(x);
                    }
                }
                _ => {}
            }
        }
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
            let root = match declared.copied().filter(|_| named) {
                Some(local) => Some(local),
                None => self.place(inner).map(|(root, _)| root),
            };
            let Some(root) = root else {
                continue;
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
            self.holds(root, values, named);
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
            (false, Some((root, _))) => {
                let holds = self.local_type(local).is_some_and(|ty| self.may_point(ty));
                if !holds {
                    return;
                }
                self.sources.push(Source::Holder(root));
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
        /// The most facts that the sets have room for.
        const ROOM: usize = 1 << 24;
        let sources = std::mem::take(&mut self.sources);
        let changes = std::mem::take(&mut self.changes);
        // The loops over the places of each variable, for what is
        // changed through a pointer.
        let mut loops_of: FxHashMap<LocalId, Vec<u32>> = FxHashMap::default();
        for (at, loop_) in self.loops.iter().enumerate() {
            loops_of.entry(loop_.root).or_default().push(at as u32);
        }
        let count = self.blocks.len();
        // A loan is a place and where it was made; a fact says that a
        // variable may hold a loan.
        let mut loans: Vec<(LocalId, Vec<Step>, Span)> = Vec::new();
        let mut loan_ids: FxHashMap<(LocalId, Vec<Step>), u32> = FxHashMap::default();
        let mut facts: Vec<(LocalId, u32)> = Vec::new();
        let mut fact_ids: FxHashMap<(LocalId, u32), u32> = FxHashMap::default();
        let mut of_holder: FxHashMap<LocalId, Vec<u32>> = FxHashMap::default();
        let mut of_root: FxHashMap<LocalId, Vec<u32>> = FxHashMap::default();
        let mut sets = Sets::new(ROOM);
        let mut steps = 0u64;

        // The loans that the sources give, where the variables hold
        // what `set` says. A place behind a pointer of a variable is a
        // place in what the variable points into.
        macro_rules! given {
            ($set:expr, $from:expr, $to:expr) => {{
                let mut given: Vec<u32> = Vec::new();
                for source in &sources[$from as usize..$to as usize] {
                    steps += 1;
                    match source {
                        Source::Holder(holder) => {
                            for &fact in of_holder.get(holder).map_or(&[][..], |all| &all[..]) {
                                if sets.has($set, fact) {
                                    given.push(facts[fact as usize].1);
                                }
                            }
                        }
                        Source::Place(root, way, made) => {
                            let behind = way.iter().position(|step| *step == Step::Target);
                            let mut through = Vec::new();
                            if let Some(behind) = behind {
                                for &fact in of_holder.get(root).map_or(&[][..], |all| &all[..]) {
                                    if sets.has($set, fact) {
                                        let (of, held, _) = &loans[facts[fact as usize].1 as usize];
                                        let mut whole = held.clone();
                                        whole.extend(&way[behind + 1..]);
                                        through.push((*of, whole));
                                    }
                                }
                            }
                            if through.is_empty() {
                                through.push((*root, way.clone()));
                            }
                            for key in through {
                                let next = loans.len() as u32;
                                let loan = *loan_ids.entry(key.clone()).or_insert_with(|| {
                                    loans.push((key.0, key.1, *made));
                                    next
                                });
                                given.push(loan);
                            }
                        }
                    }
                }
                given
            }};
        }
        macro_rules! apply {
            ($set:expr, $event:expr) => {{
                if let Event::Holds(holder, from, to, only) = $event {
                    let given = given!($set, from, to);
                    if only {
                        for &fact in of_holder.get(&holder).map_or(&[][..], |all| &all[..]) {
                            $set = sets.put($set, fact, false);
                        }
                    }
                    for loan in given {
                        let next = facts.len() as u32;
                        if next as usize >= ROOM {
                            break;
                        }
                        let fact = *fact_ids.entry((holder, loan)).or_insert_with(|| {
                            facts.push((holder, loan));
                            of_holder.entry(holder).or_default().push(next);
                            of_root
                                .entry(loans[loan as usize].0)
                                .or_default()
                                .push(next);
                            next
                        });
                        $set = sets.put($set, fact, true);
                    }
                }
            }};
        }

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
                let event = self.events[at];
                apply!(set, event);
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
        for &(holder, _) in &facts {
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
            for at in first..last {
                let event = self.events[at];
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
                // A change through a pointer under a loop that ranges
                // over what the pointer points into.
                let pointed = change.filter(|change| change.steps.contains(&Step::Target));
                if let Some(change) = pointed.filter(|_| !loops_of.is_empty()) {
                    let behind = change.steps.iter().position(|step| *step == Step::Target);
                    let behind = behind.unwrap_or(0);
                    'held: for &fact in of_holder.get(&change.root).map_or(&[][..], |all| &all[..])
                    {
                        if !sets.has(set, fact) {
                            continue;
                        }
                        let (of, held, _) = &loans[facts[fact as usize].1 as usize];
                        let mut way = held.clone();
                        way.extend(&change.steps[behind + 1..]);
                        for &looped in loops_of.get(of).map_or(&[][..], |all| &all[..]) {
                            steps += 1;
                            let loop_ = &self.loops[looped as usize];
                            let inside =
                                loop_.events.0 <= at as u32 && (at as u32) < loop_.events.1;
                            if inside && Self::unranges(loop_, &way, true) {
                                self.found.broken.push(Broken {
                                    at: change.at,
                                    what: change.what,
                                    of: *of,
                                    holder: None,
                                    made: loop_.at,
                                });
                                break 'held;
                            }
                        }
                    }
                }
                // Nothing is broken where no variable with a loan is read
                // later.
                if let Some(change) = change.filter(|_| later[at - first] != EMPTY) {
                    // The places that are written: through a pointer of
                    // a variable, those that it may point into.
                    let behind = change.steps.iter().position(|step| *step == Step::Target);
                    let mut written: Vec<(LocalId, Vec<Step>, bool)> = Vec::new();
                    if let Some(behind) = behind {
                        for &fact in of_holder.get(&change.root).map_or(&[][..], |all| &all[..]) {
                            if sets.has(set, fact) {
                                let (of, held, _) = &loans[facts[fact as usize].1 as usize];
                                let mut whole = held.clone();
                                whole.extend(&change.steps[behind + 1..]);
                                written.push((*of, whole, true));
                            }
                        }
                    }
                    if written.is_empty() {
                        written.push((change.root, change.steps.clone(), false));
                    }
                    'written: for (root, way, through) in written {
                        for &fact in of_root.get(&root).map_or(&[][..], |all| &all[..]) {
                            steps += 1;
                            if !sets.has(set, fact) {
                                continue;
                            }
                            let (holder, loan) = facts[fact as usize];
                            let read = members
                                .get(&holder)
                                .is_some_and(|&member| reads.has(later[at - first], member));
                            if !read {
                                continue;
                            }
                            let (_, held, made) = &loans[loan as usize];
                            let breaks = match lies(&way, held) {
                                // A pointer that the place holds is
                                // replaced, not what it points to.
                                Lies::Above => !held[way.len()..].contains(&Step::Target),
                                // A write through a pointer to the
                                // place changes what is there, and the
                                // place stays.
                                Lies::Same => !through,
                                Lies::Apart => false,
                            };
                            if breaks {
                                self.found.broken.push(Broken {
                                    at: change.at,
                                    what: change.what,
                                    of: root,
                                    holder: Some(holder),
                                    made: *made,
                                });
                                // One report says that the change is
                                // refused.
                                break 'written;
                            }
                        }
                    }
                }
                if let Event::Returned(from, to, span) = event {
                    for loan in given!(set, from, to) {
                        let (of, way, made) = &loans[loan as usize];
                        // What a pointer points to is not the
                        // function's, and outlives it.
                        if of.0 < locals && !way.contains(&Step::Target) {
                            self.found.escaped.push(Escaped {
                                at: span,
                                of: *of,
                                made: *made,
                            });
                        }
                    }
                }
                apply!(set, event);
            }
        }
        self.found.steps += steps + sets.steps + reads.steps;
        self.sources = sources;
        self.changes = changes;
    }
}
