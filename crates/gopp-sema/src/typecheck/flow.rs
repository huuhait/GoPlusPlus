//! What is known of a local variable at a point of its function: that
//! an option was tested and is set, that the `ok` of a lookup is true,
//! that the `err` of a call is nil (OWN-P10). The facts follow the
//! statements forward: a condition gives them to the branch it leads to,
//! an early exit to what comes after, and an assignment takes them away.

use super::*;

/// What is known of a variable.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) enum Known {
    /// It is not nil: `x != nil`, `!o.IsNil()`.
    Set,
    /// It is nil: `err == nil`.
    Nil,
    /// It is true: `ok`.
    True,
}

/// A fact, with the state of its variable when it was found.
#[derive(Clone, Copy, Debug)]
struct Fact {
    /// How often the variable was assigned before.
    version: u32,
    /// The function literal around, which no fact leaves or enters.
    epoch: u32,
    /// Where the condition is.
    at: u32,
}

/// A loop that is being walked, with the uses in it of facts from before
/// it: each holds only if the loop does not assign the variable.
struct Loop {
    span: Span,
    uses: Vec<(LocalId, Span)>,
}

#[derive(Default)]
pub(super) struct Flow {
    facts: FxHashMap<(LocalId, Known), Fact>,
    /// What each fact that was added replaced, the last one last.
    log: Vec<((LocalId, Known), std::option::Option<Fact>)>,
    versions: FxHashMap<LocalId, u32>,
    /// Where each variable is assigned, in the order of the source.
    assigned: FxHashMap<LocalId, Vec<u32>>,
    epoch: u32,
    epochs: u32,
    loops: Vec<Loop>,
    /// The variables that a function literal writes or takes the address
    /// of: it may run at any time, so nothing is known of them.
    unstable: FxHashSet<LocalId>,
    /// The variables that a function literal uses at all.
    captured: FxHashSet<LocalId>,
    /// The variables that are set only where another one says so: the
    /// value of `v, ok := m[k]` where `ok` is true, of `v, err := f()`
    /// where `err` is nil.
    guards: FxHashMap<LocalId, (LocalId, Known)>,
    /// The statements that were walked after which control does not go
    /// on.
    leaves: FxHashSet<StmtId>,
}

impl<'a> Checker<'a> {
    /// A function body starts with nothing known.
    pub(super) fn flow_enter(&mut self) -> (u32, usize) {
        self.flow.epochs += 1;
        let outer = std::mem::replace(&mut self.flow.epoch, self.flow.epochs);
        (outer, self.flow.log.len())
    }

    pub(super) fn flow_leave(&mut self, (epoch, mark): (u32, usize)) {
        self.flow_back(mark);
        self.flow.epoch = epoch;
    }

    /// A label may be jumped to from anywhere: nothing is known after it.
    pub(super) fn flow_label(&mut self) {
        self.flow.epochs += 1;
        self.flow.epoch = self.flow.epochs;
    }

    pub(super) fn flow_mark(&self) -> usize {
        self.flow.log.len()
    }

    /// Forgets the facts that were added after `mark`.
    pub(super) fn flow_back(&mut self, mark: usize) {
        while self.flow.log.len() > mark {
            self.steps += 1;
            if let Some((key, old)) = self.flow.log.pop() {
                match old {
                    Some(old) => self.flow.facts.insert(key, old),
                    None => self.flow.facts.remove(&key),
                };
            }
        }
    }

    pub(super) fn flow_add(&mut self, facts: &[(LocalId, Known, u32)]) {
        for &(local, known, at) in facts {
            self.steps += 1;
            let fact = Fact {
                version: self.flow.versions.get(&local).copied().unwrap_or(0),
                epoch: self.flow.epoch,
                at,
            };
            let old = self.flow.facts.insert((local, known), fact);
            self.flow.log.push(((local, known), old));
        }
    }

    /// The variable is assigned at `at`, or its address is taken: what
    /// was known of it is not known any more.
    pub(super) fn flow_assigned(&mut self, local: LocalId, at: u32) {
        *self.flow.versions.entry(local).or_insert(0) += 1;
        self.flow.assigned.entry(local).or_default().push(at);
        self.flow.guards.remove(&local);
    }

    pub(super) fn flow_unstable(&mut self, local: LocalId) {
        self.flow.unstable.insert(local);
    }

    pub(super) fn flow_captured(&mut self, local: LocalId) {
        self.flow.captured.insert(local);
    }

    /// The local variables `names` are declared without a value: each is
    /// nil, if its type has nil.
    pub(super) fn flow_declared_nil(&mut self, cx: &Cx<'a>, names: &[Ident]) {
        for name in names {
            if let Some(&local) = cx.body.defs.get(&name.span.lo) {
                self.flow_add(&[(local, Known::Nil, name.span.lo)]);
            }
        }
    }

    /// `place = value`: a variable that is assigned `nil` is nil.
    pub(super) fn flow_assigned_nil(&mut self, cx: &Cx<'a>, place: ExprId, value: ExprId) {
        if let (Some(local), true) = (self.flow_local(cx, place), self.is_nil_name(cx, value)) {
            self.flow_add(&[(local, Known::Nil, cx.tree.exprs[value].span.lo)]);
        }
    }

    /// `close(ch)` at `at`: not of a variable of the function that is nil
    /// on every way here (CON-6a). Nothing is said in a loop that started
    /// after the variable was last known, where a later turn may differ,
    /// nor of a variable that a function literal uses.
    pub(super) fn close_of_nil(&mut self, cx: &Cx<'a>, at: ExprId) {
        if !self.own_rules(cx) || self.memo.is_none() {
            return;
        }
        let Some(local) = self.flow_local(cx, at) else {
            return;
        };
        let is_variable = cx.body.locals[local.0 as usize].kind == LocalKind::Var;
        let Some(&fact) = self.flow.facts.get(&(local, Known::Nil)) else {
            return;
        };
        let version = self.flow.versions.get(&local).copied().unwrap_or(0);
        let in_loop = self
            .flow
            .loops
            .iter()
            .any(|around| around.span.lo > fact.at);
        let tested = self
            .flow
            .facts
            .get(&(local, Known::Set))
            .is_some_and(|set| set.version == version && set.epoch == self.flow.epoch);
        if !is_variable
            || fact.version != version
            || fact.epoch != self.flow.epoch
            || in_loop
            || tested
            || self.flow.captured.contains(&local)
        {
            return;
        }
        let text = self.expr_string(cx, at);
        let message = format!("cannot close {text}: {text} is nil on every way here");
        let fix = (
            Code::NoteOwnFixOption,
            format!("make the channel first: {text} = make(...)"),
        );
        self.error_with_fix(Code::CloseNilChan, cx.tree.exprs[at].span, message, fix);
    }

    /// An element is written in what `held` is, at `place`: not in a map
    /// that no variable holds, nor in the copy that a type assertion
    /// gives, where the write would be lost (ADR-0018).
    pub(super) fn lost_entry(&mut self, cx: &Cx<'a>, place: ExprId, held: ExprId) {
        if !self.own_rules(cx) || self.memo.is_none() {
            return;
        }
        let held = self.unparen(cx, held);
        let Some(ty) = self
            .memo
            .as_ref()
            .and_then(|memo| memo.get(&held))
            .map(|x| x.ty)
        else {
            return;
        };
        let (is_map, is_slice) = match &*self.types.get(self.types.underlying(ty)) {
            Type::Map { .. } => (true, false),
            Type::Slice(_) => (false, true),
            _ => return,
        };
        let asserted = matches!(cx.tree.exprs[held].kind, ExprKind::Assert { .. });
        let temporary = is_map && self.place_of(cx, held) == place::Place::Temporary;
        let text = self.expr_string(cx, place);
        let span = cx.tree.exprs[place].span;
        let fix = (
            Code::NoteOwnFixOption,
            "put it in a variable first".to_owned(),
        );
        if asserted && (is_map || is_slice) {
            let message = format!(
                "cannot change {text}: the assertion gives a copy, and the change would be lost"
            );
            self.error_with_fix(Code::BoxWrite, span, message, fix);
        } else if temporary {
            let message = format!(
                "cannot change {text}: no variable holds the map, and the change would be lost"
            );
            self.error_with_fix(Code::MapTempWrite, span, message, fix);
        }
    }

    pub(super) fn flow_guard(&mut self, value: LocalId, by: LocalId, known: Known) {
        self.flow.guards.insert(value, (by, known));
    }

    /// Whether `known` holds of the variable where `span` uses it. Inside
    /// a loop, a fact from before the loop holds if the loop assigns the
    /// variable nowhere, which is seen when the loop ends.
    pub(super) fn flow_holds(&mut self, local: LocalId, known: Known, span: Span) -> bool {
        let Some(&fact) = self.flow.facts.get(&(local, known)) else {
            return false;
        };
        let version = self.flow.versions.get(&local).copied().unwrap_or(0);
        if fact.version != version
            || fact.epoch != self.flow.epoch
            || self.flow.unstable.contains(&local)
        {
            return false;
        }
        // The outermost loop that started after the fact was found.
        let outermost = self
            .flow
            .loops
            .partition_point(|around| around.span.lo <= fact.at);
        if let Some(around) = self.flow.loops.get_mut(outermost) {
            around.uses.push((local, span));
        }
        true
    }

    pub(super) fn flow_loop(&mut self, span: Span) {
        self.flow.loops.push(Loop {
            span,
            uses: Vec::new(),
        });
    }

    /// The loop ends: a variable that it assigns was not known to hold
    /// what was found before the loop.
    pub(super) fn flow_loop_end(&mut self, cx: &Cx<'a>) {
        let Some(ended) = self.flow.loops.pop() else {
            return;
        };
        for (local, span) in ended.uses {
            self.steps += 1;
            let assigned = self.flow.assigned.get(&local).is_some_and(|at| {
                let from = at.partition_point(|&at| at < ended.span.lo);
                at.get(from).is_some_and(|&at| at < ended.span.hi)
            });
            if assigned {
                let name = self.names.resolve(&cx.body.locals[local.0 as usize].name);
                let message = format!(
                    "cannot rely on the test of {name} from before the loop: the loop assigns \
                     {name}"
                );
                let fix = (
                    Code::NoteOwnFixOption,
                    format!("test {name} inside the loop"),
                );
                self.error_with_fix(Code::OptionMaybeNil, span, message, fix);
            }
        }
    }

    /// The two operands of `op`. The right side of `&&` is worked out
    /// where the left side holds, that of `||` where it does not; and
    /// what is compared with nil is the variable itself.
    pub(super) fn operands(
        &mut self,
        cx: &Cx<'a>,
        op: BinaryOp,
        left: ExprId,
        right: ExprId,
    ) -> (Operand, Operand) {
        let with_nil = matches!(op, BinaryOp::Eq | BinaryOp::Ne)
            && (self.is_nil_name(cx, left) || self.is_nil_name(cx, right));
        if with_nil {
            let outer = self.unnarrowed;
            self.unnarrowed = Some(self.unparen(cx, left));
            let x = self.value(cx, left);
            self.unnarrowed = Some(self.unparen(cx, right));
            let y = self.value(cx, right);
            self.unnarrowed = outer;
            return (x, y);
        }
        let x = self.value(cx, left);
        let when = match op {
            BinaryOp::AndAnd => true,
            BinaryOp::OrOr => false,
            _ => return (x, self.value(cx, right)),
        };
        let mut facts = Vec::new();
        self.flow_facts(cx, left, when, &mut facts);
        let mark = self.flow_mark();
        self.flow_add(&facts);
        let y = self.value(cx, right);
        self.flow_back(mark);
        (x, y)
    }

    /// The place `expr` is assigned to, or its address is taken: if it
    /// is a variable, what was known of it is not known any more.
    pub(super) fn flow_written(&mut self, cx: &Cx<'a>, expr: ExprId) {
        if let Some(local) = self.flow_local(cx, expr) {
            let at = cx.tree.exprs[expr].span.lo;
            self.flow_assigned(local, at);
        }
    }

    /// `lhs := rhs` gave the variables the types `types`: a value that
    /// comes with `ok` or `err` is set where that says so, and a variable
    /// that is made here is set.
    pub(super) fn flow_defined(
        &mut self,
        cx: &Cx<'a>,
        lhs: &[ExprId],
        rhs: &[ExprId],
        types: &[TypeId],
    ) {
        let locals: Vec<std::option::Option<LocalId>> = lhs
            .iter()
            .map(|&place| {
                self.declared_local(cx, place)
                    .or(self.flow_local(cx, place))
            })
            .collect();
        if let ([_], true) = (rhs, lhs.len() > 1) {
            return self.flow_guards(cx, &locals, types);
        }
        if lhs.len() != rhs.len() {
            return;
        }
        for (local, &value) in locals.iter().zip(rhs) {
            let value = self.unparen(cx, value);
            let made = match cx.tree.exprs[value].kind {
                ExprKind::CompositeLit { .. } => true,
                ExprKind::Call { fun, .. } => {
                    let callee = self.memo.as_ref().and_then(|memo| memo.get(&fun));
                    matches!(
                        callee.map(|callee| &callee.mode),
                        Some(Mode::Builtin(Predeclared::Make))
                    )
                }
                _ => false,
            };
            if let (Some(local), true) = (*local, made) {
                self.flow_add(&[(local, Known::Set, cx.tree.exprs[value].span.lo)]);
            }
        }
    }

    /// The local variable that the expression names, through parentheses.
    pub(super) fn flow_local(&self, cx: &Cx<'a>, expr: ExprId) -> std::option::Option<LocalId> {
        let expr = self.unparen(cx, expr);
        match (&cx.tree.exprs[expr].kind, cx.body.uses.get(&expr)) {
            (ExprKind::Name(_), Some(&Ref::Local(local))) => {
                use LocalKind::*;
                let kind = cx.body.locals[local.0 as usize].kind;
                matches!(kind, Var | Receiver | Param | Result).then_some(local)
            }
            _ => None,
        }
    }

    pub(super) fn is_nil_name(&self, cx: &Cx<'a>, expr: ExprId) -> bool {
        let expr = self.unparen(cx, expr);
        matches!(
            cx.body.uses.get(&expr),
            Some(Ref::Universe(Predeclared::Nil))
        )
    }

    /// What is known where the condition `cond` is `when`: of `x != nil`,
    /// `x == nil`, `o.IsNil()`, a variable `ok`, and of these under `!`,
    /// `&&` and `||`.
    pub(super) fn flow_facts(
        &mut self,
        cx: &Cx<'a>,
        cond: ExprId,
        when: bool,
        out: &mut Vec<(LocalId, Known, u32)>,
    ) {
        self.steps += 1;
        let cond = self.unparen(cx, cond);
        let at = cx.tree.exprs[cond].span.lo;
        match cx.tree.exprs[cond].kind {
            ExprKind::Unary {
                op: UnaryOp::Not,
                x,
            } => gopp_syntax::with_stack(|| self.flow_facts(cx, x, !when, out)),
            ExprKind::Binary {
                op: BinaryOp::AndAnd,
                x,
                y,
            } if when => {
                gopp_syntax::with_stack(|| self.flow_facts(cx, x, true, out));
                self.flow_facts(cx, y, true, out);
            }
            ExprKind::Binary {
                op: BinaryOp::OrOr,
                x,
                y,
            } if !when => {
                gopp_syntax::with_stack(|| self.flow_facts(cx, x, false, out));
                self.flow_facts(cx, y, false, out);
            }
            ExprKind::Binary {
                op: op @ (BinaryOp::Eq | BinaryOp::Ne),
                x,
                y,
            } => {
                let local = match (self.is_nil_name(cx, x), self.is_nil_name(cx, y)) {
                    (false, true) => self.flow_local(cx, x),
                    (true, false) => self.flow_local(cx, y),
                    _ => None,
                };
                if let Some(local) = local {
                    let is_nil = (op == BinaryOp::Eq) == when;
                    let known = match is_nil {
                        true => Known::Nil,
                        false => Known::Set,
                    };
                    out.push((local, known, at));
                }
            }
            // `o.IsNil()`.
            ExprKind::Call { fun, ref args, .. } if args.is_empty() => {
                let fun = self.unparen(cx, fun);
                if let ExprKind::Selector { x, sel } = cx.tree.exprs[fun].kind
                    && self.names.resolve(&sel.name) == "IsNil"
                    && let Some(local) = self.flow_local(cx, x)
                {
                    let known = match when {
                        true => Known::Nil,
                        false => Known::Set,
                    };
                    out.push((local, known, at));
                }
            }
            ExprKind::Name(_) if when => {
                if let Some(local) = self.flow_local(cx, cond) {
                    out.push((local, Known::True, at));
                }
            }
            _ => {}
        }
    }

    /// Notes whether control goes on after the statement `stmt`, which
    /// was walked: not after one that returns, jumps or panics, nor after
    /// a block or an `if` whose every way out is one of these.
    pub(super) fn flow_left(&mut self, cx: &Cx<'a>, stmt: StmtId) {
        let leaves = match &cx.tree.stmts[stmt].kind {
            StmtKind::Return(_) | StmtKind::Branch { .. } => true,
            StmtKind::Block(block) => self.flow_block_leaves(&block.stmts),
            StmtKind::Labeled { stmt, .. } => self.flow_leaves(*stmt),
            StmtKind::If {
                then,
                otherwise: Some(otherwise),
                ..
            } => self.flow_block_leaves(&then.stmts) && self.flow_leaves(*otherwise),
            StmtKind::Expr(x) => match cx.tree.exprs[self.unparen(cx, *x)].kind {
                ExprKind::Call { fun, .. } => matches!(
                    cx.body.uses.get(&self.unparen(cx, fun)),
                    Some(Ref::Universe(Predeclared::Panic))
                ),
                _ => false,
            },
            _ => false,
        };
        if leaves {
            self.flow.leaves.insert(stmt);
        }
    }

    pub(super) fn flow_leaves(&self, stmt: StmtId) -> bool {
        self.flow.leaves.contains(&stmt)
    }

    pub(super) fn flow_block_leaves(&self, stmts: &[StmtId]) -> bool {
        stmts.last().is_some_and(|&last| self.flow_leaves(last))
    }

    /// The name `expr` of the local variable `local` is read as a value
    /// of the type `ty`: an option that is known to be set is read as
    /// what it holds, and a value that another variable says is set is
    /// read where that is known.
    pub(super) fn flow_read(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        local: LocalId,
        x: Operand,
    ) -> Operand {
        if !self.own_rules(cx) || self.memo.is_none() || self.unnarrowed == Some(expr) {
            return x;
        }
        let span = cx.tree.exprs[expr].span;
        if let Some(&(by, known)) = self.flow.guards.get(&local)
            && !self.flow_holds(by, known, span)
        {
            let name = self.names.resolve(&cx.body.locals[local.0 as usize].name);
            let other = self.names.resolve(&cx.body.locals[by.0 as usize].name);
            let (holds, test) = match known {
                Known::Nil => (format!("{other} is nil"), format!("{other} != nil")),
                _ => (format!("{other} is true"), format!("!{other}")),
            };
            let message = format!(
                "cannot use {name} here: {} has no zero value, so {name} is set only where \
                 {holds}",
                self.type_string(x.ty)
            );
            let fix = format!("return or go on where {test}, and use {name} after that");
            self.error_with_fix(
                Code::ValueUnchecked,
                span,
                message,
                (Code::NoteOwnFixOption, fix),
            );
        }
        let elem = match &*self.types.get(self.types.underlying(x.ty)) {
            Type::Option(elem) => *elem,
            _ => return x,
        };
        if !self.flow_holds(local, Known::Set, span) {
            return x;
        }
        self.narrowed.insert(expr);
        Operand {
            mode: x.mode,
            ty: elem,
        }
    }

    /// The value of `v, ok := m[k]`, of `v, ok := <-ch` and of
    /// `v, err := f()` is set only where the other variable says so, if
    /// its type has no zero value to stand for it (OWN-P10).
    pub(super) fn flow_guards(
        &mut self,
        cx: &Cx<'a>,
        locals: &[std::option::Option<LocalId>],
        types: &[TypeId],
    ) {
        if !self.own_rules(cx) || locals.len() < 2 || locals.len() != types.len() {
            return;
        }
        let Some((&by, values)) = locals.split_last() else {
            return;
        };
        let last = types[types.len() - 1];
        let known = if self.basic_of(last) == Some(Basic::Bool) {
            Known::True
        } else if last == self.types.error() {
            Known::Nil
        } else {
            return;
        };
        for (value, &ty) in values.iter().zip(types) {
            let Some(value) = *value else { continue };
            if self.zero_lacking(ty).is_none() {
                continue;
            }
            match by {
                Some(by) => self.flow_guard(value, by, known),
                // Nothing says whether the value is set.
                None => self.flow_guard(value, value, Known::True),
            }
        }
    }
}
