//! What is known of a function before any function is lowered
//! (ADR-0041 rule 4, PAR-1, PAR-10).
//!
//! A parameter by value that the function only reads is **lent**: the
//! caller passes its argument as it is and stays the owner, and the
//! function frees nothing. Anything more than a read makes the
//! parameter the function's own: a write to it or to a part of it, its
//! address, a move out of it, a `return` of it. A copy that the function
//! gives away is a read, since the copy is made of it.
//!
//! A variable is **reached** if something else than the function may
//! change it while a call runs: its address is taken. A value in a
//! variable that is not reached cannot change during a call, so it can
//! be lent.

use gopp_sema::types::Type as CheckedType;
use gopp_sema::{Body, LocalId, Predeclared, Ref, SelectionKind, Transfer};
use gopp_syntax::ast::{
    AssignOp, Block, DeclKind, ExprId, ExprKind, File, Param, StmtId, StmtKind, UnaryOp,
};
use rustc_hash::FxHashSet;

use super::{Job, Shared};

#[derive(Default)]
pub(crate) struct Facts {
    /// For each parameter, the receiver first: the function only reads
    /// it.
    pub lent: Vec<bool>,
    /// The variables whose address is taken.
    reached: FxHashSet<LocalId>,
    /// A function literal, a `go` or a `defer` statement is in the
    /// body: what they do to the variables is not looked at, so every
    /// variable counts as reached and every parameter as the function's
    /// own.
    all: bool,
    /// The steps that the walk over the body took.
    pub steps: u64,
}

impl Facts {
    /// Whether nothing but the function itself can change the variable
    /// while a call runs.
    pub fn quiet(&self, local: LocalId) -> bool {
        !self.all && !self.reached.contains(&local)
    }
}

struct Walk<'a> {
    shared: &'a Shared<'a>,
    tree: &'a File,
    body: &'a Body,
    /// The variables that the function does more to than read.
    more: FxHashSet<LocalId>,
    facts: Facts,
}

enum Node {
    Expr(ExprId),
    Stmt(StmtId),
}

/// The facts of the function of a job.
pub(super) fn of(shared: &Shared<'_>, job: &Job) -> Facts {
    let (tree, _) = shared.files[job.file as usize];
    let DeclKind::Func {
        receiver,
        ty,
        body: Some(block),
        ..
    } = &tree.decl_nodes[job.decl].kind
    else {
        return Facts::default();
    };
    let ExprKind::FuncType { params, .. } = &tree.exprs[*ty].kind else {
        return Facts::default();
    };
    let mut walk = Walk {
        shared,
        tree,
        body: &shared.package.files[job.file as usize].bodies[job.at],
        more: FxHashSet::default(),
        facts: Facts::default(),
    };
    walk.block(block);
    for (&expr, mode) in &walk.body.var_modes {
        if *mode == Transfer::Move {
            walk.more(expr);
        }
    }
    let lent = |param: &Param| {
        let local = (param.name).and_then(|name| walk.body.defs.get(&name.span.lo));
        // A parameter without a name is never used.
        !walk.facts.all && local.is_none_or(|local| !walk.more.contains(local))
    };
    walk.facts.lent = receiver.iter().chain(params).map(lent).collect();
    walk.facts
}

fn exprs(rest: &mut Vec<Node>, exprs: &[ExprId]) {
    rest.extend(exprs.iter().map(|&expr| Node::Expr(expr)));
}

impl Walk<'_> {
    /// The variable that the place of the expression is in, or is
    /// reached through.
    fn root(&self, mut expr: ExprId) -> Option<LocalId> {
        loop {
            expr = match &self.tree.exprs[expr].kind {
                ExprKind::Paren(x)
                | ExprKind::Selector { x, .. }
                | ExprKind::Index { x, .. }
                | ExprKind::Slice { x, .. }
                | ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => *x,
                ExprKind::Name(_) => {
                    return match self.body.uses.get(&expr) {
                        Some(Ref::Local(local)) => Some(*local),
                        _ => None,
                    };
                }
                _ => return None,
            };
        }
    }

    /// The function does more than read the variable of the place.
    fn more(&mut self, place: ExprId) {
        self.more.extend(self.root(place));
    }

    /// Something else may change the variable of the place.
    fn reached(&mut self, place: ExprId) {
        let root = self.root(place);
        self.facts.reached.extend(root);
        self.more.extend(root);
    }

    fn block(&mut self, block: &Block) {
        let mut rest: Vec<Node> = block.stmts.iter().map(|&stmt| Node::Stmt(stmt)).collect();
        while let Some(node) = rest.pop() {
            self.facts.steps += 1;
            match node {
                Node::Expr(expr) => self.expr(expr, &mut rest),
                Node::Stmt(stmt) => self.stmt(stmt, &mut rest),
            }
        }
    }

    fn expr(&mut self, expr: ExprId, rest: &mut Vec<Node>) {
        let mut next = |expr: &ExprId| rest.push(Node::Expr(*expr));
        match &self.tree.exprs[expr].kind {
            ExprKind::FuncLit { .. } | ExprKind::MacroCall { .. } => self.facts.all = true,
            ExprKind::CompositeLit { elems, .. } => elems.iter().for_each(next),
            ExprKind::KeyValue { key, value } => [key, value].into_iter().for_each(next),
            ExprKind::Paren(x)
            | ExprKind::Selector { x, .. }
            | ExprKind::Assert { x, .. }
            | ExprKind::TypeSwitchGuard { x, .. } => next(x),
            ExprKind::Index { x, indices } => {
                next(x);
                indices.iter().for_each(next);
            }
            ExprKind::Slice { x, lo, hi, max, .. } => {
                next(x);
                [lo, hi, max].into_iter().flatten().for_each(next);
            }
            ExprKind::Unary { op, x } => {
                next(x);
                if *op == UnaryOp::Addr {
                    self.reached(*x);
                }
            }
            ExprKind::Binary { x, y, .. } => [x, y].into_iter().for_each(next),
            ExprKind::Call { fun, args, .. } => {
                next(fun);
                args.iter().for_each(next);
                self.call(*fun, args);
            }
            _ => {}
        }
    }

    /// What a call does to the variables beyond reading them: a
    /// built-in function that writes its first argument, and a method
    /// that takes the address of its receiver.
    fn call(&mut self, fun: ExprId, args: &[ExprId]) {
        let mut callee = fun;
        while let ExprKind::Paren(inner) = self.tree.exprs[callee].kind {
            callee = inner;
        }
        if let Some(Ref::Universe(called)) = self.body.uses.get(&callee) {
            let writes = matches!(
                called,
                Predeclared::Clear | Predeclared::Delete | Predeclared::CopyFunc
            );
            if let (true, Some(&first)) = (writes, args.first()) {
                self.more(first);
            }
            return;
        }
        let ExprKind::Selector { x, sel } = &self.tree.exprs[callee].kind else {
            return;
        };
        let Some(selection) = self.body.selections.get(&callee) else {
            return;
        };
        if selection.kind == SelectionKind::Field {
            return;
        }
        // A method of this package says how it takes its receiver; of
        // any other it is not known, and may be by its address.
        let types = self.shared.types;
        let by_value = match &*types.get(selection.holder) {
            CheckedType::Named { id, .. } => {
                let method = self.shared.methods.get(&(*id, sel.name));
                method.is_some_and(|method| {
                    !matches!(&*types.get(method.receiver), CheckedType::Pointer(_))
                })
            }
            _ => false,
        };
        if !by_value {
            self.reached(*x);
        }
    }

    fn stmt(&mut self, stmt: StmtId, rest: &mut Vec<Node>) {
        let tree = self.tree;
        match &tree.stmts[stmt].kind {
            StmtKind::Bad | StmtKind::Empty | StmtKind::Branch { .. } => {}
            StmtKind::Go(_) | StmtKind::Defer(_) | StmtKind::Select { .. } => self.facts.all = true,
            StmtKind::Labeled { stmt, .. } => rest.push(Node::Stmt(*stmt)),
            StmtKind::Block(block) => rest.extend(block.stmts.iter().map(|&stmt| Node::Stmt(stmt))),
            StmtKind::Expr(expr) => exprs(rest, &[*expr]),
            StmtKind::Send { chan, value } => exprs(rest, &[*chan, *value]),
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    if let DeclKind::Var { values, .. } = &tree.decl_nodes[decl].kind {
                        exprs(rest, values);
                    }
                }
            }
            StmtKind::Assign { op, lhs, rhs } => {
                exprs(rest, lhs);
                exprs(rest, rhs);
                for &left in lhs {
                    // A name that the statement declares is a new
                    // variable.
                    let declares = *op == AssignOp::Define
                        && self.body.defs.contains_key(&tree.exprs[left].span.lo);
                    if !declares {
                        self.more(left);
                    }
                }
            }
            StmtKind::IncDec { x, .. } => {
                exprs(rest, &[*x]);
                self.more(*x);
            }
            StmtKind::Return(values) => {
                exprs(rest, values);
                // A variable that is returned is handed over.
                for &value in values {
                    let mut value = value;
                    while let ExprKind::Paren(inner) = tree.exprs[value].kind {
                        value = inner;
                    }
                    if let ExprKind::Name(_) = tree.exprs[value].kind {
                        self.more(value);
                    }
                }
            }
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => {
                exprs(rest, &[*cond]);
                rest.extend(init.iter().chain(otherwise).map(|&stmt| Node::Stmt(stmt)));
                rest.extend(then.stmts.iter().map(|&stmt| Node::Stmt(stmt)));
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                exprs(rest, cond.as_slice());
                rest.extend(init.iter().chain(post).map(|&stmt| Node::Stmt(stmt)));
                rest.extend(body.stmts.iter().map(|&stmt| Node::Stmt(stmt)));
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                exprs(rest, &[*x]);
                rest.extend(body.stmts.iter().map(|&stmt| Node::Stmt(stmt)));
                for &left in lhs {
                    let mut left = left;
                    while let ExprKind::Paren(inner) = tree.exprs[left].kind {
                        left = inner;
                    }
                    // `&v` is another name of an element of what is
                    // walked.
                    let alias = matches!(
                        tree.exprs[left].kind,
                        ExprKind::Unary {
                            op: UnaryOp::Addr,
                            ..
                        }
                    );
                    match (alias, define) {
                        (true, _) => self.reached(*x),
                        (false, false) => {
                            exprs(rest, &[left]);
                            self.more(left);
                        }
                        (false, true) => {}
                    }
                }
            }
            StmtKind::Switch { init, tag, clauses } => {
                exprs(rest, tag.as_slice());
                rest.extend(init.iter().map(|&stmt| Node::Stmt(stmt)));
                for clause in clauses {
                    exprs(rest, clause.cases.as_deref().unwrap_or_default());
                    rest.extend(clause.body.iter().map(|&stmt| Node::Stmt(stmt)));
                }
            }
        }
    }
}
