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

use std::sync::Arc;

use super::moves::{EMPTY, Event, Sets, Walk, lists, neighbours};
use super::*;
use crate::types::Lends;

/// What the walk needs of the checker to say what a place is.
pub(super) struct Places<'a> {
    pub types: &'a Types,
    pub memo: &'a FxHashMap<ExprId, Operand>,
    pub selections: &'a FxHashMap<ExprId, Selection>,
    pub locals: &'a FxHashMap<LocalId, Operand>,
    /// The generic functions and types that the body names with type
    /// arguments.
    pub instances: &'a FxHashMap<ExprId, crate::object::Instance>,
    /// The selectors that name a method with a pointer receiver.
    pub pointer_methods: &'a FxHashSet<ExprId>,
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
    /// The variable as a function literal uses it: where it is, at
    /// any time. With the first flag the variable may lend: it may be
    /// assigned, and not ended or moved, while the literal is used.
    /// With the second the literal may change it.
    Captured(bool, bool),
}

/// What a variable may get loans from.
#[derive(Clone)]
pub(super) enum Source {
    /// `&place`: the variable at its root, the way, and where.
    Place(LocalId, Vec<Step>, Span),
    /// What another variable holds, read through as many pointers as
    /// the number says; with the flag, also all that is reached from
    /// there through more pointers.
    Holder(LocalId, u8, bool),
    /// A pointer that the function does not make: into what outlives
    /// it.
    Outside,
    /// What the call with the number gives back.
    Call(u32),
}

/// The function that a call runs, if the checker knows it.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub(super) enum Callee {
    /// A function of the package.
    Func(ObjId),
    /// A function of a package that is imported: the import, and the
    /// object there.
    Imported(ObjId, ObjId),
    /// A method of a declared type, by its name.
    Method(crate::types::NamedId, Spur),
    /// A function value, a method of an interface or one that a child
    /// may replace, a generic function, a conversion.
    Unknown,
}

/// A call that gives back what may hold pointers, or is given a
/// pointer to store behind: for each parameter, the receiver first, the
/// sources that its argument gives.
pub(super) struct Call {
    pub callee: Callee,
    pub args: Vec<(u32, u32)>,
    /// The arguments that lead to a place which may be given a pointer.
    pub sinks: Vec<bool>,
    /// The arguments that hold a function value and no pointer: the
    /// function may keep them.
    pub funcs: Vec<bool>,
    /// The arguments through which the function may change what they
    /// reach: those that may hold a pointer.
    pub changes: Vec<bool>,
    /// The arguments that are pointers to a value which owns something
    /// apart from itself, or hold such a pointer.
    pub owns: Vec<bool>,
    /// For each argument that is or holds a pointer: the sources of
    /// what it points to.
    pub pointees: Vec<Option<(u32, u32)>>,
    /// The arguments with where each is written, and where the
    /// function is named.
    pub exprs: Vec<ExprId>,
    pub spans: Vec<Span>,
    pub at: Span,
}

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
    /// A call is given a pointer to it, or a function value that may
    /// change it.
    Call,
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

/// A loan that leaves the function at `at`: one of what ends with the
/// function, or with `given` one of what a parameter points to that is
/// put where the function does not know how long it lives.
pub(super) struct Escaped {
    pub at: Span,
    pub of: LocalId,
    pub made: Span,
    pub how: Leaves,
    pub given: bool,
    /// The loan is the use of a variable by a function literal.
    pub captured: bool,
    pub carrier: Option<Carrier>,
}

/// What holds a loan where something is found: if only a type
/// parameter in its type says that it may hold a pointer, what is found
/// is held against the type arguments.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Carrier {
    Expr(ExprId),
    Local(LocalId),
}

/// How a loan leaves the function.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Leaves {
    Returned,
    /// It is stored behind a pointer of the variable.
    Stored(LocalId),
    /// It is given to a call that may store it behind another pointer
    /// that it is given.
    Called,
    /// It is given to a call that may keep it longer than the call.
    Kept,
    /// It is put where the function does not follow it: into a
    /// variable of the package or of a function around, into a channel,
    /// or it is what a new goroutine runs.
    Left,
}

/// A pointer that is taken into what a value owns apart from itself:
/// the way to the value, and where the pointer is taken.
pub(super) struct Borrow {
    pub root: LocalId,
    pub steps: Vec<Step>,
    pub at: Span,
}

/// A pointer into what a value owns apart from itself, where the value
/// is reached through a pointer that may be one of several to it
/// (OWN-B10): taken at `at`, or by the call there that is given `given`.
pub(super) struct Reached {
    pub at: Span,
    pub given: Option<Span>,
}

/// What a function does with pointers, as its walk found it: the
/// passes go over it when every body is walked.
pub(super) struct Problem {
    pub events: Vec<Event>,
    /// For each block, where its events start and end; and the jumps
    /// from block to block.
    pub blocks: Vec<(u32, u32)>,
    pub edges: Vec<(u32, u32)>,
    pub sources: Vec<Source>,
    /// The calls, and the sources of their arguments.
    pub calls: Vec<Call>,
    pub args: Vec<Source>,
    pub changes: Vec<Change>,
    pub borrows: Vec<Borrow>,
    pub loops: Vec<Ranged>,
    /// What the walk itself found broken.
    pub broken: Vec<Broken>,
    /// The function, if others call it by its name; and its parameters
    /// that have names, the receiver first.
    pub owner: Option<Callee>,
    pub params: Vec<Option<LocalId>>,
    /// The parameters that hold a function value and no pointer.
    pub funcs: Vec<bool>,
    /// What carries the loans of a `Returned` or a `Stored` event, by
    /// the number of its first source.
    pub carriers: FxHashMap<u32, Carrier>,
    /// How many variables the body has, and where the function is.
    pub locals: u32,
    pub within: Span,
}

/// Two pointers that one call is given and that may point to one place
/// of the variable (OWN-B4).
pub(super) struct Aliased {
    /// Where the function is named, and where the two are written.
    pub at: Span,
    pub both: (Span, Span),
    pub of: LocalId,
}

/// What the passes found in a problem.
pub(super) struct Lent {
    pub broken: Vec<Broken>,
    pub escaped: Vec<Escaped>,
    pub aliased: Vec<Aliased>,
    pub reached: Vec<Reached>,
    /// What the function does with the pointers that it is given.
    pub lends: Lends,
    pub steps: u64,
}

/// Whether the change of the place breaks what the loop reads: it
/// replaces the place or what the place is in. `through` says that the
/// change is made through a pointer.
fn unranges(loop_: &Ranged, way: &[Step], through: bool) -> bool {
    match lies(way, &loop_.steps) {
        Lies::Above => !loop_.steps[way.len()..].contains(&Step::Target),
        Lies::Same => !through || !loop_.text,
        Lies::Apart => false,
    }
}

/// The problems in groups, each with those that it calls: the members
/// of a group call each other, and a group comes after every group that
/// one of its members calls (Tarjan).
pub(super) fn groups(calls: &[Vec<usize>], steps: &mut u64) -> Vec<Vec<usize>> {
    const UNSEEN: usize = usize::MAX;
    let count = calls.len();
    let mut order = vec![UNSEEN; count];
    let mut low = vec![0; count];
    let mut grouped = vec![false; count];
    let mut open = Vec::new();
    let mut groups = Vec::new();
    let mut next = 0;
    for root in 0..count {
        if order[root] != UNSEEN {
            continue;
        }
        let mut work = vec![(root, 0)];
        while let Some(top) = work.last_mut() {
            *steps += 1;
            let (v, at) = *top;
            if order[v] == UNSEEN {
                order[v] = next;
                low[v] = next;
                next += 1;
                open.push(v);
            }
            if let Some(&to) = calls[v].get(at) {
                top.1 += 1;
                if order[to] == UNSEEN {
                    work.push((to, 0));
                } else if !grouped[to] {
                    low[v] = low[v].min(order[to]);
                }
                continue;
            }
            work.pop();
            if let Some(&(from, _)) = work.last() {
                low[from] = low[from].min(low[v]);
            }
            if low[v] == order[v] {
                let mut group = Vec::new();
                while let Some(member) = open.pop() {
                    grouped[member] = true;
                    group.push(member);
                    if member == v {
                        break;
                    }
                }
                group.sort_unstable();
                groups.push(group);
            }
        }
    }
    groups
}

/// Solves the problems of a group, each with those that it calls.
/// `done` says what the functions do that were looked at before. The
/// members of a group call each other: each starts as a function that
/// does nothing with what it is given, and one is solved again when
/// what a function that it calls says has grown, until nothing grows.
pub(super) fn solve_together(
    group: &[usize],
    problems: &[(usize, usize, Problem)],
    calls: &[Vec<usize>],
    done: &FxHashMap<Callee, Arc<Lends>>,
) -> Vec<(usize, Lent)> {
    let place: FxHashMap<usize, usize> = (group.iter().enumerate())
        .map(|(at, &member)| (member, at))
        .collect();
    let mut called_by: Vec<Vec<usize>> = vec![Vec::new(); group.len()];
    let mut own: FxHashMap<Callee, Arc<Lends>> = FxHashMap::default();
    for (at, &member) in group.iter().enumerate() {
        let owner = problems[member].2.owner;
        own.extend(owner.map(|owner| (owner, Arc::new(Lends::default()))));
        for to in calls[member].iter().filter_map(|to| place.get(to)) {
            called_by[*to].push(at);
        }
    }
    let mut solved: Vec<Option<Lent>> = group.iter().map(|_| None).collect();
    let mut waits = vec![true; group.len()];
    let mut rest: std::collections::VecDeque<usize> = (0..group.len()).collect();
    let mut steps = 0;
    while let Some(at) = rest.pop_front() {
        waits[at] = false;
        let problem = &problems[group[at]].2;
        let known = |call: &Call| own.get(&call.callee).or_else(|| done.get(&call.callee));
        let lends: Vec<Option<Arc<Lends>>> = (problem.calls.iter())
            .map(|call| known(call).cloned())
            .collect();
        let mut lent = problem.solve(&lends);
        steps += std::mem::take(&mut lent.steps);
        if let Some(owner) = problem.owner
            && *own[&owner] != lent.lends
        {
            own.insert(owner, Arc::new(lent.lends.clone()));
            for &caller in &called_by[at] {
                if !std::mem::replace(&mut waits[caller], true) {
                    rest.push_back(caller);
                }
            }
        }
        solved[at] = Some(lent);
    }
    let mut all: Vec<(usize, Lent)> = (group.iter().copied())
        .zip(solved.into_iter().flatten())
        .collect();
    if let Some((_, first)) = all.first_mut() {
        first.steps = steps;
    }
    all
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
    /// Whether a value of the type may hold a pointer (OWN-B5): it
    /// holds one, or a value of a type parameter, which stands for a
    /// type that may hold one. What is found with such a value alone
    /// is held against the type arguments.
    fn may_point(&self, ty: TypeId) -> bool {
        self.places.is_some_and(|places| places.types.may_lend(ty))
    }

    /// Whether a value of the type holds a function value and no
    /// pointer: a function may keep what it is given of such a type.
    pub(super) fn only_lends(&self, ty: TypeId) -> bool {
        let types = self.places.map(|places| places.types);
        types.is_some_and(|all| all.may_lend(ty) && !all.may_hold_pointer(ty))
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

    /// `through`, with the number of the first step to what a handle
    /// holds in `apart`.
    fn through_apart(
        &self,
        ty: TypeId,
        steps: &mut Vec<Step>,
        apart: &mut Option<usize>,
    ) -> TypeId {
        let before = steps.len();
        let ty = self.through(ty, steps);
        if let Some(held) = steps[before..].iter().position(|step| *step == Step::Held) {
            apart.get_or_insert(before + held);
        }
        ty
    }

    /// A pointer is taken into the place of the expression, at `at`.
    /// If it goes into what a value owns apart from itself, and a
    /// pointer is on the way to that value, the passes say whether the
    /// pointer may be one of several to the value (OWN-B10). With
    /// `whole`, the place itself is what is owned apart: the value of
    /// an interface, the elements that a loop ranges over.
    pub(super) fn borrowing(&mut self, expr: ExprId, at: Span, whole: bool) {
        let mut apart = None;
        let Some((root, mut steps)) = self.place_with(expr, &mut apart) else {
            return;
        };
        let apart = apart.or(Some(steps.len()).filter(|_| whole));
        let Some(apart) = apart else {
            return;
        };
        steps.truncate(apart);
        if !steps.contains(&Step::Target) {
            return;
        }
        self.borrows.push(Borrow { root, steps, at });
        self.events
            .push(Event::Borrowed(self.borrows.len() as u32 - 1));
    }

    /// The place that the expression is: the variable at its root and
    /// the way from it. `None` for what is no place of a variable of
    /// the function.
    pub(super) fn place(&self, expr: ExprId) -> Option<(LocalId, Vec<Step>)> {
        self.place_with(expr, &mut None)
    }

    /// `place`; `apart` gets the number of the first step into what
    /// the value owns apart from itself (the elements of a slice, the
    /// entries of a map, what a handle holds), if there is one.
    fn place_with(&self, expr: ExprId, apart: &mut Option<usize>) -> Option<(LocalId, Vec<Step>)> {
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
                        ty = self.through_apart(ty, &mut steps, apart);
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
                    let ty = self.through_apart(of, &mut steps, apart);
                    let indexed = matches!(
                        &*types.get(types.underlying(ty)),
                        Type::Slice(_) | Type::Array { .. } | Type::Map { .. }
                    );
                    if !indexed {
                        return None;
                    }
                    if !matches!(&*types.get(types.underlying(ty)), Type::Array { .. }) {
                        apart.get_or_insert(steps.len());
                    }
                    let known = match places.memo.get(&indices[0]).map(|operand| &operand.mode) {
                        Some(Mode::Const(value)) => value.to_int().low_bits(),
                        _ => None,
                    };
                    steps.push(Step::At(known));
                }
                ExprKind::Slice { .. } => {
                    let ty = self.through_apart(of, &mut steps, apart);
                    if matches!(&*types.get(types.underlying(ty)), Type::Slice(_)) {
                        apart.get_or_insert(steps.len());
                    }
                    steps.push(Step::At(None));
                }
                _ => steps.push(match &*types.get(types.underlying(of)) {
                    Type::Unique(_) | Type::Shared(_) => {
                        apart.get_or_insert(steps.len());
                        Step::Held
                    }
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
        // The arguments of the calls in it, each with its call and its
        // place there: their sources are kept apart, one after another.
        let mut given = Vec::new();
        self.sources_in(expr, false, found, &mut given);
        self.given_to_calls(given);
    }

    /// The sources of the arguments of calls, each with its call and
    /// its place there.
    fn given_to_calls(&mut self, mut given: Vec<(u32, usize, ExprId, bool)>) {
        while let Some((call, position, arg, receiver)) = given.pop() {
            let mut args = std::mem::take(&mut self.args);
            let from = args.len() as u32;
            // A method may give back a pointer into its receiver: into
            // what a pointer or a handle leads to, if the receiver is
            // one. A method of a type parameter is one that a value
            // has, and is given the value.
            let ty = self.type_of(self.unparen(arg));
            let value = ty.is_some_and(|ty| {
                let types = self.places.map(|places| places.types);
                types.is_some_and(|all| matches!(&*all.get(all.underlying(ty)), Type::Param(_)))
            });
            if let Some((root, mut steps)) = self.place(arg).filter(|_| receiver && !value) {
                if let Some(ty) = ty {
                    self.through(ty, &mut steps);
                }
                args.push(Source::Place(root, steps, self.tree.exprs[arg].span));
            }
            // A call may give back what it reads through its arguments.
            self.sources_in(arg, true, &mut args, &mut given);
            self.calls[call as usize].args[position] = (from, args.len() as u32);
            self.args = args;
        }
    }

    /// What is given before the arguments of a call of `fun`: the
    /// receiver of a method, and what a function value is a part of.
    fn given_first(&self, fun: ExprId) -> Option<ExprId> {
        let selector = self.unparen(fun);
        let ExprKind::Selector { x, .. } = self.tree.exprs[selector].kind else {
            return None;
        };
        let selections = self.places.map(|places| places.selections);
        let selection = selections.and_then(|all| all.get(&selector));
        let named = self.body.uses.contains_key(&selector);
        let typed = selection.map(|found| found.kind) == Some(SelectionKind::MethodExpr);
        Some(x).filter(|_| !named && !typed)
    }

    /// The number of the call `expr` of `fun` with `args` among the
    /// calls of the function. A call that is met first has its
    /// arguments put into `given`.
    fn call_of(
        &mut self,
        expr: ExprId,
        fun: ExprId,
        args: &[ExprId],
        given: &mut Vec<(u32, usize, ExprId, bool)>,
    ) -> u32 {
        if let Some(&call) = self.call_ids.get(&expr) {
            return call;
        }
        let callee = self.callee(fun);
        let (before, receiver) = self.given_before(fun, callee);
        let call = self.calls.len() as u32;
        let all = before.iter().chain(args.iter());
        for (position, &arg) in all.enumerate() {
            given.push((call, position, arg, position == 0 && receiver));
        }
        let count = args.len() + before.iter().count();
        self.calls.push(Call {
            callee,
            args: vec![(0, 0); count],
            sinks: vec![false; count],
            funcs: vec![false; count],
            changes: vec![false; count],
            owns: vec![false; count],
            pointees: vec![None; count],
            spans: (before.iter().chain(args.iter()))
                .map(|&arg| self.tree.exprs[arg].span)
                .collect(),
            exprs: before.iter().chain(args.iter()).copied().collect(),
            at: self.tree.exprs[fun].span,
        });
        self.call_ids.insert(expr, call);
        call
    }

    /// What a call of `fun` is given before its arguments, and whether
    /// that is the receiver of a method. A function value that is
    /// called may give back what it uses of the function that made it:
    /// it is given first too.
    fn given_before(&self, fun: ExprId, callee: Callee) -> (Option<ExprId>, bool) {
        if let Some(first) = self.given_first(fun) {
            // A function value in a field is called, not a method.
            let selector = self.unparen(fun);
            let selections = self.places.map(|places| places.selections);
            let kind = selections.and_then(|all| all.get(&selector).map(|found| found.kind));
            let field = matches!(kind, Some(SelectionKind::Field | SelectionKind::BaseField));
            return (Some(first), !field);
        }
        let value = self
            .type_of(self.unparen(fun))
            .filter(|_| callee == Callee::Unknown);
        let value = value.filter(|&ty| self.only_lends(ty) && self.runs_value(fun));
        (value.map(|_| fun), false)
    }

    /// Whether the call of `fun` runs a value: no conversion, and no
    /// built-in function.
    fn runs_value(&self, fun: ExprId) -> bool {
        let callee = self.unparen(fun);
        let memo = self.places.map(|places| places.memo);
        let typed = memo.and_then(|memo| memo.get(&callee).map(|operand| &operand.mode));
        let built_in = matches!(self.body.uses.get(&callee), Some(Ref::Universe(_)));
        !built_in && !matches!(typed, Some(Mode::Type))
    }

    /// Whether the value that is called holds a function value and no
    /// pointer: no `Local` function, which may be made of one.
    pub(super) fn runs_function(&self, fun: ExprId) -> bool {
        let ty = self.type_of(self.unparen(fun));
        ty.is_none_or(|ty| self.only_lends(ty))
    }

    /// What the sources of the values say is put where the function
    /// does not follow it.
    pub(super) fn left(&mut self, values: &[ExprId], at: Span) {
        if self.places.is_none() {
            return;
        }
        for &value in values {
            let lends = self
                .type_of(self.unparen(value))
                .is_none_or(|ty| self.may_point(ty));
            if !lends {
                continue;
            }
            let from = self.sources.len() as u32;
            let mut found = std::mem::take(&mut self.sources);
            self.sources_of(value, &mut found);
            self.sources = found;
            let to = self.sources.len() as u32;
            if to > from {
                self.carriers.insert(from, Carrier::Expr(value));
                self.events.push(Event::Left(from, to, at));
            }
        }
    }

    /// The variable that the place of the expression is in, if it is
    /// one of a function around the function literal that is walked.
    fn around(&self, place: ExprId) -> bool {
        let mut at = self.unparen(place);
        loop {
            at = match &self.tree.exprs[at].kind {
                ExprKind::Name(_) => {
                    let local = match self.body.uses.get(&at) {
                        Some(&Ref::Local(local)) => local,
                        _ => return false,
                    };
                    return self.local(at).is_none()
                        && self.body.locals[local.0 as usize].kind != LocalKind::Type;
                }
                ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Slice { x, .. }
                | ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => self.unparen(*x),
                _ => return false,
            };
        }
    }

    /// The call has its operands: what it is given may be stored
    /// behind the pointers that it is given, as the function says.
    pub(super) fn called(&mut self, expr: ExprId) {
        let Some(places) = self.places else {
            return;
        };
        let ExprKind::Call { fun, args, .. } = &self.tree.exprs[expr].kind else {
            return;
        };
        // A conversion and a built-in function store nothing.
        let callee = self.unparen(*fun);
        let typed = places.memo.get(&callee).map(|operand| &operand.mode);
        let built_in = matches!(self.body.uses.get(&callee), Some(Ref::Universe(_)));
        if built_in || matches!(typed, Some(Mode::Type)) {
            return;
        }
        // Nor does a method that an option or a handle has of its own.
        let (before, receiver) = self.given_before(*fun, self.callee(*fun));
        let own = before.filter(|_| receiver).is_some_and(|x| {
            let mut steps = Vec::new();
            let ty = self.type_of(self.unparen(x));
            let ty = ty.map(|ty| self.through(ty, &mut steps));
            let handle = steps.last() == Some(&Step::Held);
            let option = ty.is_some_and(|ty| places.types.is_option(ty));
            (handle || option) && !places.selections.contains_key(&callee)
        });
        if own {
            return;
        }
        // Only an argument that leads to a place which holds a pointer
        // can have one stored behind it; a method may be given the
        // address of its receiver.
        let types = places.types;
        let leads = |arg: &ExprId| {
            let ty = self.type_of(self.unparen(*arg));
            ty.is_some_and(|ty| types.leads_to_store(ty))
        };
        let receives = before.filter(|_| receiver).is_some_and(|x| {
            let ty = self.type_of(self.unparen(x));
            ty.is_some_and(|ty| types.may_hold_pointer(ty))
        });
        // A function value among the arguments may be kept.
        let lends = |arg: &ExprId| {
            let ty = self.type_of(self.unparen(*arg));
            ty.is_some_and(|ty| self.only_lends(ty))
        };
        let mut sinks: Vec<bool> = before.iter().chain(args.iter()).map(leads).collect();
        let mut funcs: Vec<bool> = before.iter().chain(args.iter()).map(lends).collect();
        // The function value that is called is not given away, and
        // nothing is stored behind it.
        if before.is_some() {
            sinks[0] = (sinks[0] && receiver) || receives;
            funcs[0] &= receiver;
        }
        // The pointers that the call is given: no two of them to one
        // place (OWN-B4). A method with a pointer receiver is given a
        // pointer to its receiver.
        let pointer = |arg: &ExprId| {
            let ty = self.type_of(self.unparen(*arg));
            ty.is_some_and(|ty| matches!(&*types.get(types.underlying(ty)), Type::Pointer(_)))
        };
        let typed: Vec<bool> = before.iter().chain(args.iter()).map(pointer).collect();
        let mut pointers = typed.clone();
        let by_address = receiver && places.pointer_methods.contains(&callee);
        if before.is_some() {
            pointers[0] = receiver && (pointers[0] || by_address);
        }
        // What the function may point into: what the value behind a
        // pointer owns apart from itself (OWN-B10).
        let owning = |(position, arg): (usize, &ExprId)| {
            let ty = self.type_of(self.unparen(*arg));
            let addressed = position == 0 && by_address && !typed[0];
            ty.is_some_and(|ty| types.owns_apart(ty, !addressed))
        };
        let all = before.iter().chain(args.iter()).enumerate();
        let owns: Vec<bool> = all.map(owning).collect();
        // The receiver of a method that is given its address is pointed
        // into, and so is the value of an interface while its method
        // runs.
        let interface = before.filter(|_| receiver).is_some_and(|x| {
            let ty = self.type_of(self.unparen(x));
            let is = |ty| matches!(&*types.get(types.underlying(ty)), Type::Interface { .. });
            ty.is_some_and(is)
        });
        let lent = before.filter(|_| (by_address && !typed[0]) || interface);
        // What the function may change: what a pointer among its
        // arguments reaches, and what a function value that it is
        // given, or that is called, may change.
        let holds = |arg: &ExprId| {
            let ty = self.type_of(self.unparen(*arg));
            ty.is_some_and(|ty| types.may_lend(ty))
        };
        let mut changes: Vec<bool> = before.iter().chain(args.iter()).map(holds).collect();
        if before.is_some() && receiver {
            changes[0] = typed[0] || by_address;
        }
        if !sinks.contains(&true) && !funcs.contains(&true) && !changes.contains(&true) {
            return;
        }
        let mut given = Vec::new();
        let call = self.call_of(expr, *fun, args, &mut given);
        self.given_to_calls(given);
        self.calls[call as usize].sinks = sinks;
        self.calls[call as usize].funcs = funcs;
        self.calls[call as usize].changes = changes;
        self.calls[call as usize].owns = owns;
        if let Some(receiver) = lent {
            self.borrowing(receiver, self.tree.exprs[receiver].span, interface);
        }
        for position in (0..pointers.len()).filter(|&at| pointers[at]) {
            let arg = self.calls[call as usize].exprs[position];
            let mut found = std::mem::take(&mut self.args);
            let from = found.len() as u32;
            let mut given = Vec::new();
            // The receiver itself, where the method is given its
            // address.
            let addressed = position == 0 && by_address && !typed[position];
            match self.place(arg).filter(|_| addressed) {
                Some((root, steps)) => {
                    found.push(Source::Place(root, steps, self.tree.exprs[arg].span));
                }
                None => self.sources_in(arg, false, &mut found, &mut given),
            }
            self.calls[call as usize].pointees[position] = Some((from, found.len() as u32));
            self.args = found;
            self.given_to_calls(given);
        }
        self.events.push(Event::Called(call));
    }

    /// The function that the call of `fun` runs.
    fn callee(&self, fun: ExprId) -> Callee {
        let Some(places) = self.places else {
            return Callee::Unknown;
        };
        // A generic function is one function for every type that it is
        // used with.
        let mut fun = self.unparen(fun);
        if let ExprKind::Index { x, .. } = self.tree.exprs[fun].kind
            && places.instances.contains_key(&fun)
        {
            fun = self.unparen(x);
        }
        match self.body.uses.get(&fun) {
            Some(&Ref::Object(object)) => return Callee::Func(object),
            Some(&Ref::Member { import, member }) => return Callee::Imported(import, member),
            Some(_) => return Callee::Unknown,
            None => {}
        }
        let ExprKind::Selector { sel, .. } = &self.tree.exprs[fun].kind else {
            return Callee::Unknown;
        };
        let Some(selection) = places.selections.get(&fun) else {
            return Callee::Unknown;
        };
        let types = places.types;
        let named = match &*types.get(selection.holder) {
            Type::Named { id, .. } => *id,
            _ => return Callee::Unknown,
        };
        let interface = matches!(
            &*types.get(types.underlying(selection.holder)),
            Type::Interface { .. }
        );
        match selection.kind {
            SelectionKind::BaseMethod => Callee::Method(named, sel.name),
            // A child may replace the method of a type with a table.
            SelectionKind::Method | SelectionKind::MethodExpr
                if !interface && !types.has_table(selection.holder) =>
            {
                Callee::Method(named, sel.name)
            }
            _ => Callee::Unknown,
        }
    }

    /// `sources_of`, without the arguments of the calls, which `given`
    /// gets. With `beyond`, also what is reached from the value through
    /// pointers.
    fn sources_in(
        &mut self,
        expr: ExprId,
        beyond: bool,
        found: &mut Vec<Source>,
        given: &mut Vec<(u32, usize, ExprId, bool)>,
    ) {
        let tree = self.tree;
        let mut rest = vec![(expr, beyond)];
        while let Some((expr, beyond)) = rest.pop() {
            self.found.steps += 1;
            let span = tree.exprs[expr].span;
            match &tree.exprs[expr].kind {
                ExprKind::Paren(x) | ExprKind::Assert { x, .. } => rest.push((*x, beyond)),
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
                        found.push(Source::Holder(local, 0, beyond));
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
                        // And what is read there.
                        if beyond {
                            rest.push((*x, true));
                        }
                    }
                    None => rest.push((*x, beyond)),
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
                    match self.place(expr) {
                        Some((root, steps)) => {
                            let hops = steps.iter().filter(|step| **step == Step::Target).count();
                            let hops = hops.min(u8::MAX as usize) as u8;
                            let holds = self.local_type(root).is_some_and(|ty| self.may_point(ty));
                            if holds {
                                found.push(Source::Holder(root, hops, beyond));
                            }
                        }
                        // What it is a part of is no variable: all
                        // that is reached from that counts.
                        None => {
                            let mut of = *x;
                            loop {
                                of = self.unparen(of);
                                of = match &tree.exprs[of].kind {
                                    ExprKind::Selector { x, .. }
                                    | ExprKind::Index { x, .. }
                                    | ExprKind::Slice { x, .. }
                                    | ExprKind::Unary {
                                        op: UnaryOp::Star,
                                        x,
                                    } => *x,
                                    _ => break,
                                };
                            }
                            rest.push((of, true));
                        }
                    }
                }
                // A function literal uses the variables around it
                // where they are: those that may lend, it lends, with
                // all that they reach.
                ExprKind::FuncLit { .. } => {
                    for capture in self.captured(expr) {
                        let local = capture.local;
                        let lends = self.local_type(local).is_some_and(|ty| self.may_point(ty));
                        let writes = capture.how != crate::object::CaptureUse::Read;
                        if self.own(local) && (lends || writes) {
                            let step = Step::Captured(lends, writes);
                            found.push(Source::Place(local, vec![step], span));
                        }
                        if self.own(local) && lends {
                            found.push(Source::Holder(local, 0, true));
                        }
                    }
                }
                ExprKind::CompositeLit { elems, .. } => {
                    rest.extend(elems.iter().map(|&elem| (elem, beyond)));
                }
                ExprKind::KeyValue { value, .. } => rest.push((*value, beyond)),
                ExprKind::Call { fun, args, .. } => {
                    // What a call gives holds nothing of its operands
                    // if it can hold no pointer.
                    let points = self.type_of(expr).is_none_or(|ty| self.may_point(ty));
                    if !points {
                        continue;
                    }
                    // What it gives back of its arguments, the
                    // function says.
                    let call = self.call_of(expr, *fun, args, given);
                    found.push(Source::Call(call));
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
                    // A variable of a function around a function
                    // literal is not followed from the literal, and a
                    // variable of the package is checked for pointers
                    // where it is assigned (OWN-B5): what such a place
                    // gets leaves the function.
                    None => {
                        let ty = self.type_of(inner);
                        let lends = ty.is_none_or(|ty| self.may_point(ty));
                        let func = ty.is_some_and(|ty| self.only_lends(ty));
                        if lends && (self.around(inner) || func) {
                            let values = match places.len() == values.len() {
                                true => &values[at..=at],
                                false => values,
                            };
                            self.left(values, self.tree.exprs[inner].span);
                        }
                        continue;
                    }
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
                    let behind = behind.min(u8::MAX as usize) as u8;
                    let at = self.tree.exprs[inner].span;
                    self.carriers.insert(from, Carrier::Expr(inner));
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
            if unranges(loop_, &change.steps, false) {
                self.broken.push(Broken {
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
        self.ended(&frame, end);
    }

    /// The variables end at `at`: where their statement ends, or where
    /// a jump leaves it.
    pub(super) fn ended(&mut self, locals: &[LocalId], at: Span) {
        for &local in locals {
            self.changes.push(Change {
                root: local,
                steps: Vec::new(),
                at,
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
        // A variable that is the element itself points into the
        // elements: of a slice or a map, into what is owned apart.
        if alias {
            let apart = self.type_of(self.unparen(x)).is_some_and(|ty| {
                let types = self.places.map(|places| places.types);
                let ty = self.through(ty, &mut Vec::new());
                types.is_some_and(|all| {
                    matches!(
                        &*all.get(all.underlying(ty)),
                        Type::Slice(_) | Type::Map { .. }
                    )
                })
            });
            self.borrowing(x, self.tree.exprs[x].span, apart);
        }
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
                let hops = pointers.min(u8::MAX as usize) as u8;
                self.sources.push(Source::Holder(root, hops, false));
            }
            _ => return,
        }
        let to = self.sources.len() as u32;
        self.events.push(Event::Holds(local, from, to, true));
    }

    /// The values are returned.
    pub(super) fn returned(&mut self, values: &[ExprId], at: Span) {
        if self.places.is_none() {
            return;
        }
        // Without values, the results that have names are returned.
        // Each value is an event of its own.
        if values.is_empty() {
            for result in self.results.clone() {
                if self.local_type(result).is_some_and(|ty| self.may_point(ty)) {
                    let from = self.sources.len() as u32;
                    self.sources.push(Source::Holder(result, 0, false));
                    self.carriers.insert(from, Carrier::Local(result));
                    self.events.push(Event::Returned(from, from + 1, at));
                }
            }
        }
        let each = values.len() == self.pointers.len();
        for (position, &value) in values.iter().enumerate() {
            let Some(ty) = self.type_of(value) else {
                continue;
            };
            // A handle is lent where the result is a pointer (OWN-P6).
            let mut steps = Vec::new();
            self.through(ty, &mut steps);
            let lent = steps.first() == Some(&Step::Held) && each && self.pointers[position];
            if !self.may_point(ty) && !lent {
                continue;
            }
            let from = self.sources.len() as u32;
            let mut found = std::mem::take(&mut self.sources);
            self.sources_of(value, &mut found);
            self.sources = found;
            let to = self.sources.len() as u32;
            if to > from {
                self.carriers.insert(from, Carrier::Expr(value));
                self.events.push(Event::Returned(from, to, at));
            }
        }
    }

    /// What the walk found, for the passes.
    pub(super) fn problem(&mut self) -> Problem {
        Problem {
            events: std::mem::take(&mut self.events),
            blocks: std::mem::take(&mut self.blocks),
            edges: std::mem::take(&mut self.edges),
            sources: std::mem::take(&mut self.sources),
            calls: std::mem::take(&mut self.calls),
            args: std::mem::take(&mut self.args),
            owner: self.owner.filter(|_| self.top),
            params: std::mem::take(&mut self.params),
            funcs: std::mem::take(&mut self.funcs),
            carriers: std::mem::take(&mut self.carriers),
            changes: std::mem::take(&mut self.changes),
            borrows: std::mem::take(&mut self.borrows),
            loops: std::mem::take(&mut self.loops),
            broken: std::mem::take(&mut self.broken),
            locals: self.body.locals.len() as u32,
            within: self.within,
        }
    }
}

impl Problem {
    /// Which loans each variable may hold, which variables are read
    /// later, and with both: the places that are changed under a loan
    /// that is still used, and the loans that leave the function.
    /// `lends` says for each call what the function that it runs does
    /// with what it is given, where that is known.
    pub(super) fn solve(&self, lends: &[Option<Arc<Lends>>]) -> Lent {
        let changes = &self.changes;
        let sources = &Sources {
            problem: self,
            lends,
        };
        let mut lent = Lent {
            broken: Vec::new(),
            escaped: Vec::new(),
            aliased: Vec::new(),
            reached: Vec::new(),
            lends: Lends {
                gives: vec![false; self.params.len()],
                stores: Vec::new(),
                keeps: vec![false; self.params.len()],
                changes: vec![false; self.params.len()],
                borrows: vec![false; self.params.len()],
                deep: vec![false; self.params.len()],
                outside: false,
            },
            steps: 0,
        };
        // The loops over the places of each variable, for what is
        // changed through a pointer.
        let mut loops_of: FxHashMap<LocalId, Vec<u32>> = FxHashMap::default();
        for (at, loop_) in self.loops.iter().enumerate() {
            loops_of.entry(loop_.root).or_default().push(at as u32);
        }
        let count = self.blocks.len();
        let (before, after) = (
            &lists(count, &self.edges, true),
            &lists(count, &self.edges, false),
        );
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
                set = known.apply(&mut sets, set, sources, self.events[at]);
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
                let made;
                let change = match event {
                    Event::Changed(change) => Some(&changes[change as usize]),
                    Event::Moved(local, span) => {
                        made = Change {
                            root: local,
                            steps: Vec::new(),
                            at: span,
                            what: What::Move,
                        };
                        Some(&made)
                    }
                    // A call may change what it is given a pointer to.
                    Event::Called(call) => {
                        made = Change {
                            root: OUTSIDE,
                            steps: Vec::new(),
                            at: self.calls[call as usize].at,
                            what: What::Call,
                        };
                        Some(&made)
                    }
                    _ => None,
                };
                // The places that are written: through a pointer of a
                // variable, those that it may point into. What is
                // written behind a parameter, those who call the
                // function are told.
                let told = !self.params.is_empty();
                let written = match (change, event) {
                    (Some(_), Event::Called(call)) => known.changed_by(&sets, set, sources, call),
                    (Some(change), _)
                        if later[at - first] != EMPTY || !loops_of.is_empty() || told =>
                    {
                        known.written(&sets, set, change)
                    }
                    _ => Vec::new(),
                };
                for (of, way, _) in &written {
                    let param = self.params.iter().position(|param| param == &Some(*of));
                    if let (true, Some(param)) = (way.contains(&Step::Target), param) {
                        lent.lends.changes[param] = true;
                    }
                }
                // A change through a pointer under a loop that ranges
                // over what the pointer points into.
                if let Some(change) = change.filter(|_| !loops_of.is_empty()) {
                    'ranged: for (of, way, through) in &written {
                        // A change that is written at the place itself
                        // was looked at by the walk.
                        if !through && change.what != What::Call {
                            continue;
                        }
                        for &looped in loops_of.get(of).map_or(&[][..], |all| &all[..]) {
                            known.steps += 1;
                            let loop_ = &self.loops[looped as usize];
                            let inside =
                                loop_.events.0 <= at as u32 && (at as u32) < loop_.events.1;
                            if inside && unranges(loop_, way, true) {
                                lent.broken.push(Broken {
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
                    // A holder that is read later itself is named
                    // before one that is read through another.
                    let mut through_other = None;
                    'written: for (root, way, through) in &written {
                        // The loans that the change may break, and
                        // then those who hold one of them: many may
                        // hold a loan that it does not break.
                        let loans = known.under(*root, way).len();
                        for at in 0..loans {
                            known.steps += 1;
                            let loan = known.under(*root, way)[at];
                            let (_, held, made) = &known.loans[loan as usize];
                            let lie = lies(way, held);
                            let same = matches!(lie, Lies::Same);
                            let breaks = match lie {
                                // A pointer that the place holds is
                                // replaced, not what it points to.
                                // A function literal uses a variable
                                // where it is: what is assigned to
                                // it, the literal sees.
                                Lies::Above if matches!(held[..], [Step::Captured(..)]) => {
                                    matches!(held[..], [Step::Captured(true, _)])
                                        && matches!(
                                            change.what,
                                            What::End | What::Declare | What::Move
                                        )
                                }
                                Lies::Above => !held[way.len()..].contains(&Step::Target),
                                // A write through a pointer to the
                                // place changes what is there, and the
                                // place stays: through the variable
                                // that holds the pointer too.
                                Lies::Same => !*through,
                                Lies::Apart => false,
                            };
                            if !breaks {
                                continue;
                            }
                            let made = *made;
                            for at in 0..known.holders[loan as usize].len() {
                                known.steps += 1;
                                let fact = known.holders[loan as usize][at];
                                let holder = known.facts[fact as usize].0;
                                let own = same && pointed && holder == change.root;
                                if own || !sets.has(set, fact) {
                                    continue;
                                }
                                let broken = Broken {
                                    at: change.at,
                                    what: change.what,
                                    of: *root,
                                    holder: Some(holder),
                                    made,
                                };
                                // One report says that the change is
                                // refused.
                                if read_later(holder) {
                                    through_other = Some(broken);
                                    break 'written;
                                }
                                if through_other.is_none()
                                    && known.used(&sets, set, holder, &read_later, &mut unused)
                                {
                                    through_other = Some(broken);
                                }
                            }
                        }
                    }
                    lent.broken.extend(through_other);
                }
                // What ends with the function does not leave it: not as
                // a result, and not behind a pointer that leads out of
                // the function.
                match event {
                    Event::Returned(from, to, span) => {
                        let given = known.given(&sets, set, sources, (from, to));
                        let how = (span, Leaves::Returned, self.carriers.get(&from).copied());
                        self.leaves(&known, None, &given, how, true, &mut lent);
                    }
                    Event::Stored(root, from, to, behind, span) => {
                        let places = known.read(&sets, set, root, behind, false);
                        let given = known.given(&sets, set, sources, (from, to));
                        let carrier = self.carriers.get(&from).copied();
                        let how = (span, Leaves::Stored(root), carrier);
                        self.leaves(&known, Some(&places), &given, how, true, &mut lent);
                    }
                    Event::Borrowed(borrow) => {
                        let borrow = &self.borrows[borrow as usize];
                        let reached =
                            known.resolved(&sets, set, borrow.root, &borrow.steps, borrow.at);
                        self.reaches(&known, &reached, (borrow.at, None), &mut lent);
                    }
                    Event::Left(from, to, span) => {
                        let given = known.given(&sets, set, sources, (from, to));
                        let how = (span, Leaves::Left, self.carriers.get(&from).copied());
                        self.leaves(&known, Some(&[]), &given, how, true, &mut lent);
                    }
                    Event::Called(call) => {
                        let record = &self.calls[call as usize];
                        lent.aliased
                            .extend(known.aliased(&sets, set, sources, record));
                        // A function that points into what a value owns
                        // apart is given no pointer that may be one of
                        // several to the value.
                        for (arg, pointee) in record.pointees.iter().enumerate() {
                            let says = sources.says(call);
                            let points = says.is_none_or(|lends| {
                                lends.borrows.get(arg).copied().unwrap_or(false)
                            });
                            let Some(range) = pointee.filter(|_| points && record.owns[arg]) else {
                                continue;
                            };
                            let run = run(&self.args, range);
                            let reached = known.given_by(&sets, set, sources, run, false);
                            let how = (record.at, Some(record.spans[arg]));
                            self.reaches(&known, &reached, how, &mut lent);
                        }
                        let sure = sources.says(call).is_some();
                        // What the function keeps of a function value
                        // that it is given; one that is not known may
                        // keep every one (CLO-4).
                        for (arg, &lends) in record.funcs.iter().enumerate() {
                            let says = sources.says(call);
                            let keeps = says.map_or(lends, |lends| {
                                lends.keeps.get(arg).copied().unwrap_or(false)
                            });
                            if !keeps {
                                continue;
                            }
                            let run = run(&self.args, record.args[arg]);
                            let given = known.given_by(&sets, set, sources, run, true);
                            let carrier = Some(Carrier::Expr(record.exprs[arg]));
                            let how = (record.at, Leaves::Kept, carrier);
                            self.leaves(&known, Some(&[]), &given, how, true, &mut lent);
                        }
                        for stored in known.stored_by(&sets, set, sources, call) {
                            for (from, given) in &stored.given {
                                let carrier = Some(Carrier::Expr(record.exprs[*from]));
                                let how = (record.at, Leaves::Called, carrier);
                                let places = Some(&stored.places[..]);
                                self.leaves(&known, places, given, how, sure, &mut lent);
                            }
                        }
                    }
                    _ => {}
                }
                set = known.apply(&mut sets, set, sources, event);
            }
        }
        lent.lends.stores.sort_unstable();
        lent.lends.stores.dedup();
        lent.steps = known.steps + sets.steps + reads.steps;
        lent
    }

    /// A pointer is taken into what the values at the places `reached`
    /// own apart from themselves: at `at`, or by a call there that is
    /// given what is written at the second place. A value behind a
    /// pointer that was read through another, or that came from
    /// outside the function, may have a second pointer to it, and none
    /// is taken there. One behind a parameter is the parameter's:
    /// those who call the function are told.
    fn reaches(
        &self,
        known: &Known,
        reached: &[u32],
        (at, given): (Span, Option<Span>),
        lent: &mut Lent,
    ) {
        for &place in reached {
            let (of, way, _) = &known.loans[place as usize];
            let pointers = way.iter().filter(|step| **step == Step::Target).count();
            let param = self.params.iter().position(|param| *param == Some(*of));
            match (pointers, param) {
                (0, _) => {}
                (1, Some(param)) => lent.lends.borrows[param] = true,
                _ => {
                    lent.reached.push(Reached { at, given });
                    return;
                }
            }
        }
    }

    /// The loans `given` leave the function as `how` says: as results,
    /// or into the places `places`, if these are behind a pointer that
    /// leads out of the function. What ends with the function does not
    /// leave it; what a parameter points to leaves only as a result or
    /// to behind a parameter, which those who call the function are
    /// told. Without `sure`, a function that is not known may store
    /// it: that it puts what a parameter points to somewhere else is
    /// not held against the call.
    fn leaves(
        &self,
        known: &Known,
        places: Option<&[u32]>,
        given: &[u32],
        (at, how, carrier): (Span, Leaves, Option<Carrier>),
        sure: bool,
        lent: &mut Lent,
    ) {
        let param = |local: LocalId| self.params.iter().position(|param| *param == Some(local));
        // The parameters that the places are behind, and whether one
        // is somewhere else outside the function. A pointer that is
        // stored through and of which nothing is known leads out of
        // the function too; an argument that reaches nothing has
        // nothing stored behind it.
        let mut behind = Vec::new();
        let stored = matches!(how, Leaves::Stored(_));
        let nowhere = stored && places.is_some_and(|places| places.is_empty());
        let mut unknown = matches!(how, Leaves::Left | Leaves::Kept);
        for &place in places.unwrap_or(&[]) {
            let (of, way, _) = &known.loans[place as usize];
            if !way.contains(&Step::Target) {
                continue;
            }
            match param(*of) {
                Some(at) => behind.push(at as u32),
                None => unknown = true,
            }
        }
        if places.is_some() && behind.is_empty() && !unknown && !nowhere {
            return;
        }
        // Where the loans go is followed by those who call the
        // function: as a result, or behind a parameter.
        let followed = places.is_none() || !(unknown || nowhere);
        let captured =
            |loan: &u32| matches!(known.loans[*loan as usize].1[..], [Step::Captured(true, _)]);
        let uses = given.iter().any(captured);
        for &loan in given {
            let (of, way, made) = &known.loans[loan as usize];
            // A variable that a function literal uses is kept as long
            // as the literal: what it holds is what counts, where the
            // literal is followed. Where it is not, the literal uses
            // what it may not, and that is all that is said.
            // A variable that cannot lend is kept with the literal.
            if matches!(way[..], [Step::Captured(false, _)]) {
                continue;
            }
            let capture = matches!(way[..], [Step::Captured(..)]);
            if uses && capture == followed {
                continue;
            }
            let escaped = |given: bool| Escaped {
                at,
                of: *of,
                made: *made,
                how,
                given,
                captured: capture,
                // The variable that a literal uses says whether what
                // is found depends on a type parameter.
                carrier: match capture {
                    true => Some(Carrier::Local(*of)),
                    false => carrier,
                },
            };
            // What a pointer points to is not the function's, and
            // outlives it.
            if of.0 < self.locals && !way.contains(&Step::Target) {
                lent.escaped.push(escaped(false));
                continue;
            }
            let Some(from) = param(*of) else {
                // A result that points to what no parameter leads to.
                lent.lends.outside |= places.is_none();
                continue;
            };
            match places {
                None => {
                    lent.lends.gives[from] = true;
                    // Into what the parameter points to, or into what
                    // is read through that.
                    let pointers = way.iter().filter(|step| **step == Step::Target).count();
                    lent.lends.deep[from] |= pointers > 1;
                }
                Some(_) => {
                    let pairs = behind.iter().map(|&into| (into, from as u32));
                    lent.lends.stores.extend(pairs);
                    // A function value may be kept, which those who
                    // call the function are told; a pointer may not.
                    match self.funcs.get(from) {
                        Some(true) if unknown => lent.lends.keeps[from] = true,
                        _ if unknown && sure => lent.escaped.push(escaped(true)),
                        _ => {}
                    }
                }
            }
        }
    }
}

/// The step with any index, for an element.
fn any_index(step: Step) -> Step {
    match step {
        Step::At(_) => Step::At(None),
        other => other,
    }
}

/// What a call may store behind one of its arguments.
struct StoredBy {
    /// The places that the argument reaches.
    places: Vec<u32>,
    /// The loans in what may be stored there, by the argument that
    /// they come from.
    given: Vec<(usize, Vec<u32>)>,
    /// Those of the argument itself, where the function is not known:
    /// they are taken to stay inside the function.
    own: Vec<u32>,
}

/// The sources of a problem, with what is known of the functions that
/// its calls run.
struct Sources<'p> {
    problem: &'p Problem,
    lends: &'p [Option<Arc<Lends>>],
}

impl Sources<'_> {
    /// What the function of the call says that it stores, if it is
    /// known. A function that says nothing of its parameters stores
    /// nothing; one that is given another number of arguments than it
    /// has parameters is taken as not known.
    fn says(&self, call: u32) -> Option<&Lends> {
        let count = self.problem.calls[call as usize].args.len();
        let known = self.lends[call as usize].as_deref();
        known.filter(|lends| lends.gives.is_empty() || lends.gives.len() == count)
    }
}

/// The sources from the first number to the second.
fn run(all: &[Source], (from, to): (u32, u32)) -> &[Source] {
    &all[from as usize..to as usize]
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
    /// The loans of each variable, and for each loan the facts of
    /// those that hold it.
    lent: FxHashMap<LocalId, Vec<u32>>,
    holders: Vec<Vec<u32>>,
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
            lent: FxHashMap::default(),
            holders: Vec::new(),
            steps: 0,
        };
        known.outside = known.loan(OUTSIDE, vec![Step::Target], function);
        known
    }

    /// The facts of the loans of places of the variable.
    fn rooted(&self, root: LocalId) -> &[u32] {
        self.of_root.get(&root).map_or(&[], |all| &all[..])
    }

    /// The loans that a change of the place may break: those of the
    /// place, of what it is in and of its parts. A loan of another
    /// part of the variable is none.
    fn under(&self, root: LocalId, way: &[Step]) -> &[u32] {
        match way.first() {
            None => self.lent.get(&root).map_or(&[], |all| &all[..]),
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
        self.holders.push(Vec::new());
        self.lent.entry(key.0).or_default().push(next);
        if let Some(&first) = key.1.first() {
            let part = (key.0, any_index(first));
            self.of_part.entry(part).or_default().push(next);
        }
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
                let root = self.loans[loan as usize].0;
                self.of_root.entry(root).or_default().push(next);
                self.holders[loan as usize].push(next);
                next
            }
        };
        sets.put(set, fact, true)
    }

    /// The loans in a value that is read from the variable through as
    /// many pointers as `hops` says: what the variables hold that its
    /// loans are places of. With `beyond`, also those in all that is
    /// reached from the value through more pointers. What is read
    /// through a pointer from outside the function is from outside too.
    fn read(&mut self, sets: &Sets, set: u32, holder: LocalId, hops: u8, beyond: bool) -> Vec<u32> {
        let mut now = Vec::new();
        self.held(sets, set, holder, &mut now);
        let mut all = Vec::new();
        let mut seen: FxHashSet<LocalId> = FxHashSet::default();
        let mut left = hops;
        while !now.is_empty() {
            if left == 0 {
                if !beyond {
                    return now;
                }
                all.extend(&now);
            }
            let mut next = Vec::new();
            for loan in now {
                let (of, way, made) = &self.loans[loan as usize];
                let (of, made) = (*of, *made);
                // What is read through a pointer from outside is
                // behind that pointer: of the same parameter, if it is
                // one.
                if way.contains(&Step::Target) {
                    let deep = way.ends_with(&[Step::Target, Step::Target]);
                    if !deep {
                        let mut behind = way.clone();
                        behind.push(Step::Target);
                        next.push(self.loan(of, behind, made));
                    }
                } else if left > 0 || seen.insert(of) {
                    self.held(sets, set, of, &mut next);
                }
            }
            next.sort_unstable();
            next.dedup();
            now = next;
            left = left.saturating_sub(1);
        }
        all.sort_unstable();
        all.dedup();
        all
    }

    /// The loans that the sources from the first number to the second
    /// give, where the variables hold what the set says.
    fn given(&mut self, sets: &Sets, set: u32, of: &Sources, range: (u32, u32)) -> Vec<u32> {
        self.given_by(sets, set, of, run(&of.problem.sources, range), false)
    }

    /// The loans that the sources give. A place behind a pointer of a
    /// variable is a place in what the variable points into; a call
    /// gives what the arguments give that the function may give back,
    /// and what is outside.
    ///
    /// With `reach`, the sources are those of an argument with all that
    /// it reaches, and a call among them gives all that its arguments
    /// reach. Without, a call gives what its function says: what is
    /// outside only if a result may point there, and of an argument
    /// only what it points to, unless a result may point deeper.
    fn given_by(
        &mut self,
        sets: &Sets,
        set: u32,
        of: &Sources,
        first: &[Source],
        reach: bool,
    ) -> Vec<u32> {
        let mut given: Vec<u32> = Vec::new();
        let mut rest = vec![(first, reach)];
        while let Some((sources, reach)) = rest.pop() {
            for source in sources {
                self.steps += 1;
                match source {
                    Source::Outside => given.push(self.outside),
                    Source::Holder(holder, hops, beyond) => {
                        given.extend(self.read(sets, set, *holder, *hops, *beyond));
                    }
                    Source::Call(call) => {
                        let record = &of.problem.calls[*call as usize];
                        let args = &record.args;
                        let known = of.lends[*call as usize].as_deref();
                        let says = of.says(*call).filter(|_| !reach);
                        if says.is_none_or(|lends| lends.outside) {
                            given.push(self.outside);
                        }
                        for (position, &arg) in args.iter().enumerate() {
                            let counts =
                                known.is_none_or(|lends| lends.gives_back(position, args.len()));
                            if !counts {
                                continue;
                            }
                            // What the argument points to, where the
                            // function says that no result points
                            // deeper.
                            let direct = says.is_some_and(|lends| {
                                !lends.deep.get(position).copied().unwrap_or(true)
                            });
                            match record.pointees[position].filter(|_| direct) {
                                Some(pointee) => rest.push((run(&of.problem.args, pointee), false)),
                                None => rest.push((run(&of.problem.args, arg), true)),
                            }
                        }
                    }
                    Source::Place(root, way, made) => {
                        given.extend(self.resolved(sets, set, *root, way, *made));
                    }
                }
            }
        }
        given
    }

    /// The loans of the place at the way from the variable, made at
    /// `made`: each pointer on the way leads into what the variable
    /// before it holds.
    fn resolved(
        &mut self,
        sets: &Sets,
        set: u32,
        root: LocalId,
        way: &[Step],
        made: Span,
    ) -> Vec<u32> {
        let mut parts = way.split(|step| *step == Step::Target);
        let first = parts.next().unwrap_or(&[]);
        let mut places = vec![(root, first.to_vec())];
        for part in parts {
            let mut next = Vec::new();
            for (of, mut way) in places {
                let mut held = Vec::new();
                if !way.contains(&Step::Target) {
                    self.held(sets, set, of, &mut held);
                }
                // A pointer that is not known, or comes from outside,
                // leads behind its variable.
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
        let loans = places.into_iter().map(|(of, way)| self.loan(of, way, made));
        loans.collect()
    }

    /// The set after the event: what a variable holds from then on.
    fn apply(&mut self, sets: &mut Sets, mut set: u32, sources: &Sources, event: Event) -> u32 {
        match event {
            Event::Holds(holder, from, to, only) => {
                let given = self.given(sets, set, sources, (from, to));
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
                let given = self.given(sets, set, sources, (from, to));
                for place in self.read(sets, set, root, behind, false) {
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
            Event::Called(call) => {
                for stored in self.stored_by(sets, set, sources, call) {
                    let given = stored.given.iter().flat_map(|(_, loans)| loans);
                    let given: Vec<u32> = given.chain(&stored.own).copied().collect();
                    for place in stored.places {
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
            }
            _ => {}
        }
        set
    }

    /// The places that the call may change, each with whether it is
    /// changed through a pointer: all that an argument reaches which
    /// may hold a pointer, unless the function says that it changes
    /// nothing there; and the variables that a function value among
    /// the arguments, or the one that is called, may change.
    fn changed_by(
        &mut self,
        sets: &Sets,
        set: u32,
        of: &Sources,
        call: u32,
    ) -> Vec<(LocalId, Vec<Step>, bool)> {
        let record = &of.problem.calls[call as usize];
        let says = of.says(call);
        let mut written = Vec::new();
        for (position, &may) in record.changes.iter().enumerate() {
            let said = says.map(|lends| lends.changes.get(position).copied().unwrap_or(false));
            if !may || said == Some(false) {
                continue;
            }
            let reached = self.given_by(
                sets,
                set,
                of,
                run(&of.problem.args, record.args[position]),
                true,
            );
            for loan in reached {
                if loan == self.outside {
                    continue;
                }
                let (root, way, _) = &self.loans[loan as usize];
                match way[..] {
                    [Step::Captured(_, true)] => written.push((*root, Vec::new(), false)),
                    [Step::Captured(..)] => {}
                    _ => written.push((*root, way.clone(), true)),
                }
            }
        }
        let mut seen = FxHashSet::default();
        written.retain(|place| seen.insert(place.clone()));
        written
    }

    /// The first two pointers that the call is given and that may point
    /// to one place: to the same place of a variable, or to a place and
    /// a part of it. What is read through a pointer, and what comes
    /// from outside the function, may be one value by its nature
    /// (OWN-B10) and is not held to this.
    fn aliased(&mut self, sets: &Sets, set: u32, of: &Sources, call: &Call) -> Option<Aliased> {
        let mut pointers: Vec<(usize, Vec<u32>)> = Vec::new();
        for (position, pointee) in call.pointees.iter().enumerate() {
            let Some(range) = *pointee else {
                continue;
            };
            let mut loans = self.given_by(sets, set, of, run(&of.problem.args, range), false);
            loans.retain(|&loan| {
                let (root, way, _) = &self.loans[loan as usize];
                let through = way.iter().filter(|step| **step == Step::Target).count() > 1;
                let captured = matches!(way[..], [Step::Captured(..)]);
                *root != OUTSIDE && !through && !captured
            });
            for (other, others) in &pointers {
                for (&a, &b) in loans
                    .iter()
                    .flat_map(|a| others.iter().map(move |b| (a, b)))
                {
                    self.steps += 1;
                    let ((root, way, _), (other_root, other_way, _)) =
                        (&self.loans[a as usize], &self.loans[b as usize]);
                    let apart = matches!(lies(way, other_way), Lies::Apart)
                        && matches!(lies(other_way, way), Lies::Apart);
                    if root == other_root && !apart {
                        return Some(Aliased {
                            at: call.at,
                            both: (call.spans[*other], call.spans[position]),
                            of: *root,
                        });
                    }
                }
            }
            pointers.push((position, loans));
        }
        None
    }

    /// What the call may store: for each argument that the function
    /// may store behind, the places that the argument reaches, and the
    /// loans in what it may store there, by the argument that they
    /// come from. A function that is not known
    /// may store all that it is given behind every argument that leads
    /// to a place for a pointer; what it stores of an argument behind
    /// the same argument comes last, apart: it is taken to stay inside
    /// the function.
    fn stored_by(&mut self, sets: &Sets, set: u32, of: &Sources, call: u32) -> Vec<StoredBy> {
        let record = &of.problem.calls[call as usize];
        let count = record.args.len();
        let known = of.says(call);
        let mut into: Vec<Vec<usize>> = vec![Vec::new(); count];
        match known {
            Some(lends) => {
                for &(sink, from) in &lends.stores {
                    into[sink as usize].push(from as usize);
                }
            }
            None => {
                for sink in (0..count).filter(|&sink| record.sinks[sink]) {
                    into[sink] = (0..count).collect();
                }
            }
        }
        let mut reached: Vec<Option<Vec<u32>>> = vec![None; count];
        let mut all = Vec::new();
        for (sink, from) in into.into_iter().enumerate() {
            if from.is_empty() {
                continue;
            }
            for &arg in std::iter::once(&sink).chain(&from) {
                if reached[arg].is_none() {
                    let sources = run(&of.problem.args, record.args[arg]);
                    reached[arg] = Some(self.given_by(sets, set, of, sources, true));
                }
            }
            let places = reached[sink].clone().unwrap_or_default();
            let loans = |arg: usize| reached[arg].clone().unwrap_or_default();
            let apart = known.is_none() && from.contains(&sink);
            let own = Some(sink).filter(|_| apart).map(loans).unwrap_or_default();
            let others = from.iter().filter(|&&arg| !(apart && arg == sink));
            all.push(StoredBy {
                places,
                given: others.map(|&arg| (arg, loans(arg))).collect(),
                own,
            });
        }
        all
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
