//! Labels and the statements that jump, after `go/types` (labels.go and
//! the branch statements of stmt.go).

use super::*;

/// What a statement may do: `break`, `continue`, `fallthrough`; and where
/// it is, for the message when `fallthrough` is not allowed.
#[derive(Clone, Copy, Default)]
struct Allowed {
    breaks: bool,
    continues: bool,
    falls_through: bool,
    final_case: bool,
    type_switch: bool,
}

/// A statement with a label: the label, and the statement after it.
#[derive(Clone, Copy)]
struct Labeled {
    name: Spur,
    stmt: StmtId,
}

/// The labels and the jumps of one function body. Statements are counted
/// as they are met: a count says when a block was entered, a `goto`
/// written or a variable declared.
#[derive(Default)]
struct Labels {
    /// Every label, with whether it is used.
    all: Vec<(Ident, bool)>,
    index: FxHashMap<Spur, usize>,
    /// How many of the blocks that are open declare the label: a `goto`
    /// reaches it.
    open: FxHashMap<Spur, u32>,
    /// The statements with the label that are being walked, the
    /// innermost last.
    enclosing: FxHashMap<Spur, Vec<StmtId>>,
    /// For a label that is not declared yet: each `goto` to it, with the
    /// count when it was written.
    waiting: FxHashMap<Spur, Vec<(Ident, u64)>>,
    count: u64,
}

/// One block that is open.
#[derive(Default)]
struct Block {
    entered: u64,
    labels: Vec<Spur>,
    /// Where the last variable of the block was declared, and the count
    /// then.
    declared: Option<(u32, u64)>,
}

impl<'a> Checker<'a> {
    /// Checks the labels and the jumps of the body of a function.
    pub(super) fn branches(&mut self, cx: &Cx<'a>, body: &[StmtId]) {
        self.jumps(cx, Allowed::default(), body);
        let mut labels = Labels::default();
        self.block_branches(cx, &mut labels, None, body);
        // A label that was not found is not declared, or it is inside a
        // block that the jump is outside of.
        let mut waiting: Vec<Ident> = labels
            .waiting
            .into_values()
            .flatten()
            .map(|(jump, _)| jump)
            .collect();
        waiting.sort_by_key(|jump| jump.span.lo);
        for jump in waiting {
            let name = self.names.resolve(&jump.name);
            match labels.index.get(&jump.name) {
                Some(&label) => {
                    labels.all[label].1 = true;
                    let message = format!("goto {name} jumps into block");
                    self.error(Code::GotoJumpsInto, jump.span, message);
                }
                None => {
                    let message = format!("label {name} not declared");
                    self.error(Code::LabelUndeclared, jump.span, message);
                }
            }
        }
        for (label, used) in labels.all {
            if !used {
                let name = self.names.resolve(&label.name);
                let message = format!("label {name} declared and not used");
                self.error(Code::LabelNotUsed, label.span, message);
            }
        }
    }

    // ---- break, continue and fallthrough without a label

    fn jumps(&mut self, cx: &Cx<'a>, allowed: Allowed, list: &[StmtId]) {
        // Empty statements at the end do not count for `fallthrough`.
        let is_empty = |&stmt: &StmtId| matches!(cx.tree.stmts[stmt].kind, StmtKind::Empty);
        let end = list.iter().rposition(|stmt| !is_empty(stmt));
        for (at, &stmt) in list.iter().enumerate() {
            let last = Some(at) == end;
            let allowed = Allowed {
                falls_through: allowed.falls_through && last,
                ..allowed
            };
            self.steps += 1;
            gopp_syntax::with_stack(|| self.jump(cx, allowed, stmt));
        }
    }

    fn jump(&mut self, cx: &Cx<'a>, allowed: Allowed, stmt: StmtId) {
        let node = &cx.tree.stmts[stmt];
        // What a statement inside this one may do.
        let mut inner = Allowed {
            falls_through: false,
            final_case: false,
            type_switch: false,
            ..allowed
        };
        match &node.kind {
            StmtKind::Labeled { stmt, .. } => self.jump(cx, allowed, *stmt),
            StmtKind::Block(block) => self.jumps(cx, inner, &block.stmts),
            StmtKind::If {
                then, otherwise, ..
            } => {
                self.jumps(cx, inner, &then.stmts);
                if let Some(otherwise) = otherwise {
                    self.jump(cx, inner, *otherwise);
                }
            }
            StmtKind::Switch { tag, clauses, .. } => {
                inner.breaks = true;
                let type_switch = tag.is_some_and(|tag| {
                    matches!(cx.tree.exprs[tag].kind, ExprKind::TypeSwitchGuard { .. })
                });
                for (at, clause) in clauses.iter().enumerate() {
                    let last = at + 1 == clauses.len();
                    let inner = Allowed {
                        type_switch,
                        falls_through: !type_switch && !last,
                        final_case: !type_switch && last,
                        ..inner
                    };
                    self.jumps(cx, inner, &clause.body);
                }
            }
            StmtKind::Select { clauses } => {
                inner.breaks = true;
                for clause in clauses {
                    self.jumps(cx, inner, &clause.body);
                }
            }
            StmtKind::For { body, .. } | StmtKind::Range { body, .. } => {
                (inner.breaks, inner.continues) = (true, true);
                self.jumps(cx, inner, &body.stmts);
            }
            StmtKind::Branch { kind, label: None } => {
                let (code, message) = match kind {
                    BranchKind::Break if !allowed.breaks => (
                        Code::NotInLoopBreak,
                        "break not in for, switch, or select statement",
                    ),
                    BranchKind::Continue if !allowed.continues => {
                        (Code::NotInLoopContinue, "continue not in for statement")
                    }
                    BranchKind::Fallthrough if !allowed.falls_through => {
                        if allowed.final_case {
                            let message = "cannot fallthrough final case in switch";
                            (Code::FallthroughFinal, message)
                        } else if allowed.type_switch {
                            let message = "cannot fallthrough in type switch";
                            (Code::FallthroughTypeSwitch, message)
                        } else {
                            let message = "fallthrough statement out of place";
                            (Code::FallthroughMisplaced, message)
                        }
                    }
                    _ => return,
                };
                self.error(code, node.span, message.to_owned());
            }
            _ => {}
        }
    }

    // ---- labels

    /// The statements of a block, which is the body of the statement
    /// `labeled` if that has a label.
    fn block_branches(
        &mut self,
        cx: &Cx<'a>,
        labels: &mut Labels,
        labeled: Option<Labeled>,
        list: &[StmtId],
    ) {
        if let Some(labeled) = labeled {
            let enclosing = labels.enclosing.entry(labeled.name).or_default();
            enclosing.push(labeled.stmt);
        }
        let mut block = Block {
            entered: labels.count,
            ..Block::default()
        };
        for &stmt in list {
            gopp_syntax::with_stack(|| self.stmt_branches(cx, labels, &mut block, None, stmt));
        }
        for name in block.labels {
            if let Some(open) = labels.open.get_mut(&name) {
                *open -= 1;
            }
        }
        if let Some(enclosing) = labeled.and_then(|l| labels.enclosing.get_mut(&l.name)) {
            enclosing.pop();
        }
    }

    fn stmt_branches(
        &mut self,
        cx: &Cx<'a>,
        labels: &mut Labels,
        block: &mut Block,
        labeled: Option<Labeled>,
        stmt: StmtId,
    ) {
        let node = &cx.tree.stmts[stmt];
        labels.count += 1;
        self.steps += 1;
        match &node.kind {
            StmtKind::Decl(decls) => {
                let is_var =
                    |&decl: &DeclId| matches!(cx.tree.decl_nodes[decl].kind, DeclKind::Var { .. });
                if decls.iter().any(is_var) {
                    block.declared = Some((node.span.lo, labels.count));
                }
            }
            StmtKind::Assign {
                op: AssignOp::Define,
                ..
            } => block.declared = Some((node.span.lo, labels.count)),
            StmtKind::Labeled { label, stmt: after } => {
                let mut labeled = labeled;
                if label.name != self.blank {
                    self.label(cx, labels, block, *label);
                    labeled = Some(Labeled {
                        name: label.name,
                        stmt: *after,
                    });
                }
                self.stmt_branches(cx, labels, block, labeled, *after);
            }
            StmtKind::Branch {
                kind,
                label: Some(label),
            } => {
                let name = self.names.resolve(&label.name);
                let target = labels.enclosing.get(&label.name).and_then(|at| at.last());
                let target = target.map(|&target| &cx.tree.stmts[target].kind);
                let is_loop = matches!(target, Some(StmtKind::For { .. } | StmtKind::Range { .. }));
                let breaks = is_loop
                    || matches!(
                        target,
                        Some(StmtKind::Switch { .. } | StmtKind::Select { .. })
                    );
                let declared = labels.open.get(&label.name).is_some_and(|&open| open > 0);
                match kind {
                    BranchKind::Break if !breaks => {
                        let message = format!("invalid break label {name}");
                        return self.error(Code::InvalidBreakLabel, label.span, message);
                    }
                    BranchKind::Continue if !is_loop => {
                        let message = format!("invalid continue label {name}");
                        return self.error(Code::InvalidContinueLabel, label.span, message);
                    }
                    BranchKind::Goto if !declared => {
                        let waiting = labels.waiting.entry(label.name).or_default();
                        return waiting.push((*label, labels.count));
                    }
                    BranchKind::Fallthrough => return,
                    _ => {}
                }
                if let Some(&known) = labels.index.get(&label.name) {
                    labels.all[known].1 = true;
                }
            }
            StmtKind::Block(inner) => self.block_branches(cx, labels, labeled, &inner.stmts),
            StmtKind::If {
                then, otherwise, ..
            } => {
                self.block_branches(cx, labels, labeled, &then.stmts);
                if let Some(otherwise) = otherwise {
                    self.stmt_branches(cx, labels, block, labeled, *otherwise);
                }
            }
            StmtKind::For { body, .. } | StmtKind::Range { body, .. } => {
                self.block_branches(cx, labels, labeled, &body.stmts);
            }
            // The clauses are blocks inside the block of the statement,
            // which has the label.
            StmtKind::Switch { clauses, .. } => {
                let bodies = clauses.iter().map(|clause| &clause.body[..]);
                self.clauses(cx, labels, labeled, bodies);
            }
            StmtKind::Select { clauses } => {
                let bodies = clauses.iter().map(|clause| &clause.body[..]);
                self.clauses(cx, labels, labeled, bodies);
            }
            _ => {}
        }
    }

    fn clauses<'b>(
        &mut self,
        cx: &Cx<'a>,
        labels: &mut Labels,
        labeled: Option<Labeled>,
        bodies: impl Iterator<Item = &'b [StmtId]>,
    ) {
        if let Some(labeled) = labeled {
            let enclosing = labels.enclosing.entry(labeled.name).or_default();
            enclosing.push(labeled.stmt);
        }
        for body in bodies {
            self.block_branches(cx, labels, None, body);
        }
        if let Some(enclosing) = labeled.and_then(|l| labels.enclosing.get_mut(&l.name)) {
            enclosing.pop();
        }
    }

    /// Declares the label in `block`. Every `goto` to it that was written
    /// since the block was entered has found its label.
    fn label(&mut self, cx: &Cx<'a>, labels: &mut Labels, block: &mut Block, label: Ident) {
        let name = self.names.resolve(&label.name);
        let known = labels.index.get(&label.name).copied();
        match known {
            Some(other) => {
                let message = format!("label {name} already declared");
                if self.reported.insert((label.span, Code::LabelRedeclared)) {
                    let diagnostic = Diagnostic::new(Code::LabelRedeclared, label.span, message);
                    self.sink.push(diagnostic.with_note(
                        Code::NoteOtherDeclaration,
                        Some(labels.all[other].0.span),
                        format!("other declaration of {name}"),
                    ));
                }
            }
            None => {
                labels.index.insert(label.name, labels.all.len());
                labels.all.push((label, false));
                *labels.open.entry(label.name).or_default() += 1;
                block.labels.push(label.name);
            }
        }
        let Some(waiting) = labels.waiting.get_mut(&label.name) else {
            return;
        };
        while let Some(&(jump, written)) = waiting.last().filter(|jump| jump.1 > block.entered) {
            waiting.pop();
            self.steps += 1;
            if known.is_none()
                && let Some(last) = labels.all.last_mut()
            {
                last.1 = true;
            }
            // A variable that is declared in the block between the jump
            // and the label is jumped over.
            if let Some((at, _)) = block.declared.filter(|&(_, declared)| declared > written) {
                let line = cx.text[..at as usize].matches('\n').count() + 1;
                let message = format!("goto {name} jumps over variable declaration at line {line}");
                self.error(Code::GotoJumpsOver, jump.span, message);
            }
        }
    }
}
