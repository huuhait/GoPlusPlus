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
//! A parameter of an interface type that the function calls methods
//! through, and does no more to, is lent too, and **worked on**: a
//! method may change the value that the interface value holds, so the
//! caller lends a value of its own for the call, which nothing else
//! reads (TYP-7).
//!
//! A function literal whose value cannot outlive the variables that it
//! uses is **framed**: its environment is in the frame of the function
//! and has the addresses of those variables, which stay where they are
//! (CLO-1). That is a literal that is called where it is written, one
//! that a `defer` statement calls and that uses variables of the whole
//! function only, one that a parameter is lent which its function only
//! calls, and one that a variable holds which is only called. A variable
//! that any other literal uses lives in a cell (CLO-2).
//!
//! A variable is **reached** if something else than the function may
//! change it while a call runs: its address is taken. A value in a
//! variable that is not reached cannot change during a call, so it can
//! be lent.

use gopp_mangle::{Entity, Namer, symbol_of};
use gopp_sema::types::Type as CheckedType;
use gopp_sema::{Body, LocalId, LocalKind, MethodValue, Predeclared, Ref, SelectionKind, Transfer};
use gopp_syntax::ast::{
    AssignOp, Block, DeclKind, ExprId, ExprKind, File, Param, StmtId, StmtKind, UnaryOp,
};
use rustc_hash::{FxHashMap, FxHashSet};

use super::{Job, Shared};

#[derive(Default)]
pub(crate) struct Facts {
    /// For each parameter, the receiver first: the function does not
    /// own it. It only reads it, or calls methods through it.
    pub lent: Vec<bool>,
    /// For each parameter: it is lent, and the function calls methods
    /// through it, which may change the value that it holds.
    pub worked: Vec<bool>,
    /// The function calls `recover` itself, and the function literals
    /// in it that do: only a function that a panic calls stops the
    /// panic so (PAN-1).
    pub recovers: bool,
    pub recovering: FxHashSet<ExprId>,
    /// The function calls through a function value or an interface,
    /// and the function literals in it that do: what such a call runs
    /// is not known where it is written.
    pub calls_values: bool,
    pub calling_values: FxHashSet<ExprId>,
    /// The parameters that a call is given as the last that is read of
    /// them: the variable, what is called, and which argument it is,
    /// none for the receiver. And the same with the number of the
    /// parameter, the symbol of the function and the number of its
    /// parameter, for the calls of functions that are known.
    passed: Vec<(LocalId, ExprId, Option<usize>)>,
    pub given_on: Vec<(usize, String, usize)>,
    /// The variables whose address is taken.
    reached: FxHashSet<LocalId>,
    /// The variables that a function literal which is not framed uses
    /// and does not declare, and those whose address a method value
    /// has: each lives in a cell, which the function values that use it
    /// share.
    pub celled: FxHashSet<LocalId>,
    /// For a variable that a function literal which is not framed
    /// uses: the first such literal, which a report names.
    pub later: FxHashMap<LocalId, ExprId>,
    /// The variables of blocks that a `defer` statement borrows: a
    /// deferred function literal uses them, or the statement has their
    /// address as the receiver of its call or as `&x` among its
    /// arguments. Each lives in a cell, and as long as the function if
    /// the statement runs (PAN-3).
    pub extended: FxHashSet<LocalId>,
    /// The function literals whose environment is in the frame.
    pub framed: FxHashSet<ExprId>,
    /// The variables whose address a method value has.
    bound: FxHashSet<LocalId>,
    /// The function literals that are arguments of a call, each with
    /// what is called and its number among the arguments: framed if the
    /// function that is called is lent the parameter, which is known
    /// when every function has its facts.
    asked: Vec<(ExprId, ExprId, usize)>,
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

    /// The variables that live in cells, from the literals that are
    /// framed.
    fn settle(&mut self, body: &Body) {
        self.celled = self.bound.clone();
        self.celled.extend(self.extended.iter().copied());
        self.later.clear();
        for (&literal, captures) in &body.captures {
            if !self.framed.contains(&literal) {
                for capture in captures {
                    self.celled.insert(capture.local);
                    let first = self.later.entry(capture.local).or_insert(literal);
                    *first = literal.min(*first);
                }
            }
        }
    }

    /// With the facts of every function of the package: the literals
    /// that are arguments for parameters which their functions are
    /// lent are framed too.
    pub(super) fn lend(&mut self, shared: &Shared<'_>, job: &Job) {
        if self.asked.is_empty() {
            return;
        }
        let (tree, _) = shared.files[job.file as usize];
        let body = shared.body(job);
        let mut namer = Namer::new(shared.types, shared.names);
        for (literal, callee, at) in std::mem::take(&mut self.asked) {
            // The function of the package or the method of one of its
            // types that is called, and where its parameters start.
            let called = match (&tree.exprs[callee].kind, body.uses.get(&callee)) {
                (ExprKind::Name(_), Some(Ref::Object(object))) => {
                    Some((Entity::Object(shared.home(job.file), *object), 0))
                }
                (ExprKind::Selector { sel, .. }, _) => match body.selections.get(&callee) {
                    Some(selection) if selection.kind == SelectionKind::Method => {
                        let method = Entity::Method {
                            receiver: selection.holder,
                            name: sel.name,
                        };
                        Some((method, 1))
                    }
                    _ => None,
                },
                _ => None,
            };
            let Some((entity, first)) = called else {
                continue;
            };
            let lent = symbol_of(&mut namer, entity)
                .ok()
                .and_then(|symbol| shared.lent.get(&symbol));
            if lent.and_then(|lent| lent.get(first + at)) == Some(&true) {
                self.framed.insert(literal);
            }
        }
        self.settle(body);
    }
}

struct Walk<'a> {
    shared: &'a Shared<'a>,
    tree: &'a File,
    body: &'a Body,
    /// The package of the function.
    home: &'a gopp_sema::Package,
    /// The variables that the function does more to than read.
    more: FxHashSet<LocalId>,
    /// The variables of an interface type that a method is called
    /// through.
    called: FxHashSet<LocalId>,
    /// The function literals that are called where they are written,
    /// and those that a `defer` statement calls.
    direct: Vec<ExprId>,
    deferred: Vec<ExprId>,
    /// The variables whose address a `defer` statement has.
    held: Vec<LocalId>,
    /// The variables that are declared with a function literal as
    /// their value.
    func_vars: FxHashMap<LocalId, ExprId>,
    /// How often each variable is named, and how often as what a call
    /// calls.
    named: FxHashMap<LocalId, (u32, u32)>,
    /// The variables that a `go` or a `defer` statement names.
    waiting: FxHashSet<LocalId>,
    /// A macro call is in the body: what it names is not known.
    opaque: bool,
    /// How many function literals are around what is walked, and which.
    inside: u32,
    within: Vec<ExprId>,
    facts: Facts,
}

enum Node {
    Expr(ExprId),
    /// An expression of a `go` or a `defer` statement, which runs
    /// later.
    Later(ExprId),
    Stmt(StmtId),
    /// The body of a function literal is walked.
    Leave,
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
        body: shared.body(job),
        home: shared.home(job.file),
        more: FxHashSet::default(),
        called: FxHashSet::default(),
        direct: Vec::new(),
        deferred: Vec::new(),
        held: Vec::new(),
        func_vars: FxHashMap::default(),
        named: FxHashMap::default(),
        waiting: FxHashSet::default(),
        opaque: false,
        inside: 0,
        within: Vec::new(),
        facts: Facts::default(),
    };
    walk.block(block);
    for (&expr, mode) in &walk.body.var_modes {
        if *mode == Transfer::Move {
            walk.more(expr);
        }
    }
    walk.frame(block);
    let lent = |param: &Param| {
        let local = (param.name).and_then(|name| walk.body.defs.get(&name.span.lo));
        // A parameter without a name is never used.
        !walk.facts.all && local.is_none_or(|local| !walk.more.contains(local))
    };
    let worked = |param: &Param| {
        let local = (param.name).and_then(|name| walk.body.defs.get(&name.span.lo));
        lent(param) && local.is_some_and(|local| walk.called.contains(local))
    };
    let all = || receiver.iter().chain(params);
    (walk.facts.lent, walk.facts.worked) = (all().map(lent).collect(), all().map(worked).collect());
    // The parameters that are given on, and to which parameter of which
    // function.
    let mut namer = Namer::new(shared.types, shared.names);
    for (local, callee, at) in std::mem::take(&mut walk.facts.passed) {
        let named = |param: &Param| {
            let found = (param.name).and_then(|name| walk.body.defs.get(&name.span.lo));
            found == Some(&local)
        };
        let Some(param) = all().position(named) else {
            continue;
        };
        let called = match (&tree.exprs[callee].kind, walk.body.uses.get(&callee)) {
            (ExprKind::Name(_), Some(Ref::Object(object))) => {
                at.map(|at| (Entity::Object(shared.home(job.file), *object), at))
            }
            (ExprKind::Selector { sel, .. }, _) => match walk.body.selections.get(&callee) {
                Some(selection) if selection.kind == SelectionKind::Method => {
                    let method = Entity::Method {
                        receiver: selection.holder,
                        name: sel.name,
                    };
                    Some((method, at.map_or(0, |at| at + 1)))
                }
                _ => None,
            },
            _ => None,
        };
        if let Some((entity, position)) = called
            && let Ok(symbol) = symbol_of(&mut namer, entity)
        {
            walk.facts.given_on.push((param, symbol, position));
        }
    }
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
                // `base` is the receiver of the method (TYP-20).
                ExprKind::Name(_) => {
                    return match self.body.uses.get(&expr) {
                        Some(Ref::Local(local)) => Some(*local),
                        _ => self.shared.base_of(self.tree, self.body, expr),
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
                Node::Expr(expr) => self.expr(expr, &mut rest, false),
                Node::Later(expr) => self.expr(expr, &mut rest, true),
                Node::Stmt(stmt) => self.stmt(stmt, &mut rest),
                Node::Leave => {
                    self.inside -= 1;
                    self.within.pop();
                }
            }
        }
    }

    /// The body of a function literal is walked too: a literal in it
    /// may be framed as one of the function itself is, with its
    /// environment in the frame of the literal around it. Nothing else
    /// is put on the list after this for the same expression.
    fn literal(&mut self, literal: ExprId, rest: &mut Vec<Node>) {
        if let ExprKind::FuncLit { body, .. } = &self.tree.exprs[literal].kind {
            self.inside += 1;
            self.within.push(literal);
            rest.push(Node::Leave);
            rest.extend(body.stmts.iter().map(|&stmt| Node::Stmt(stmt)));
        }
    }

    /// The expression without the parentheses around it.
    fn unparen(&self, mut expr: ExprId) -> ExprId {
        while let ExprKind::Paren(inner) = self.tree.exprs[expr].kind {
            expr = inner;
        }
        expr
    }

    /// The variable that a name names.
    fn local(&self, name: ExprId) -> Option<LocalId> {
        match (&self.tree.exprs[name].kind, self.body.uses.get(&name)) {
            (ExprKind::Name(_), Some(Ref::Local(local))) => Some(*local),
            _ => None,
        }
    }

    /// Which function literals are framed, once the body is walked.
    fn frame(&mut self, block: &Block) {
        let body = self.body;
        // The variables of the whole function: its parameters, its
        // results, and what its outermost statements declare.
        let mut whole: FxHashSet<LocalId> = FxHashSet::default();
        for (at, local) in body.locals.iter().enumerate() {
            if matches!(
                local.kind,
                LocalKind::Param | LocalKind::Result | LocalKind::Receiver
            ) {
                whole.insert(LocalId(at as u32));
            }
        }
        for &stmt in &block.stmts {
            let mut stmt = stmt;
            while let StmtKind::Labeled { stmt: inner, .. } = &self.tree.stmts[stmt].kind {
                stmt = *inner;
            }
            match &self.tree.stmts[stmt].kind {
                StmtKind::Decl(decls) => {
                    for &decl in decls {
                        if let DeclKind::Var { names, .. } = &self.tree.decl_nodes[decl].kind {
                            let declared =
                                names.iter().filter_map(|name| body.defs.get(&name.span.lo));
                            whole.extend(declared.copied());
                        }
                    }
                }
                StmtKind::Assign {
                    op: AssignOp::Define,
                    lhs,
                    ..
                } => {
                    let declared = (lhs.iter())
                        .filter_map(|&left| body.defs.get(&self.tree.exprs[left].span.lo));
                    whole.extend(declared.copied());
                }
                _ => {}
            }
        }
        let captures = |literal: &ExprId| body.captures.get(literal).map(Vec::as_slice);
        let mut framed: FxHashSet<ExprId> = self.direct.iter().copied().collect();
        let mut extended: FxHashSet<LocalId> = self.held.iter().copied().collect();
        for literal in &self.deferred {
            let used = captures(literal).unwrap_or_default();
            if used.iter().all(|capture| whole.contains(&capture.local)) {
                framed.insert(*literal);
            }
            extended.extend(used.iter().map(|capture| capture.local));
        }
        // A variable of the whole function lives as long as it anyway.
        extended.retain(|local| !whole.contains(local));
        self.facts.extended = extended;
        // A variable that holds a literal and is only called: nothing
        // else names it, and no other literal uses it.
        let captured: FxHashSet<LocalId> = (body.captures.values().flatten())
            .map(|capture| capture.local)
            .collect();
        for (local, literal) in &self.func_vars {
            let only_called = self
                .named
                .get(local)
                .is_none_or(|&(all, called)| all == called);
            let kept = captured.contains(local)
                || self.waiting.contains(local)
                || self.more.contains(local);
            if only_called && !kept {
                framed.insert(*literal);
            }
        }
        if !self.opaque {
            self.facts.framed = framed;
        }
        self.facts.settle(body);
    }

    /// `later` says that the expression is of a `go` or a `defer`
    /// statement: what it names is used when the statement runs its
    /// call.
    fn expr(&mut self, expr: ExprId, rest: &mut Vec<Node>, later: bool) {
        let mut next = |expr: &ExprId| {
            rest.push(match later {
                true => Node::Later(*expr),
                false => Node::Expr(*expr),
            });
        };
        match &self.tree.exprs[expr].kind {
            ExprKind::MacroCall { .. } => {
                self.facts.all = true;
                self.opaque = true;
            }
            ExprKind::FuncLit { .. } => {
                self.facts.all = true;
                self.literal(expr, rest);
            }
            ExprKind::Name(_) => {
                if let Some(local) = self.local(expr) {
                    self.named.entry(local).or_default().0 += 1;
                    if later {
                        self.waiting.insert(local);
                    }
                }
            }
            ExprKind::CompositeLit { elems, .. } => elems.iter().for_each(next),
            ExprKind::KeyValue { key, value } => [key, value].into_iter().for_each(next),
            ExprKind::Selector { x, .. } => {
                next(x);
                // A method value with a pointer receiver has the
                // address of its variable, which then lives in a cell
                // that the value shares (CLO-5).
                if self.body.method_values.get(&expr) == Some(&MethodValue::Address) {
                    self.reached(*x);
                    let root = self.root(*x);
                    self.facts.bound.extend(root);
                }
            }
            ExprKind::Paren(x)
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
                if !later {
                    self.called_literals(*fun, args);
                }
                self.call(*fun, args);
            }
            _ => {}
        }
    }

    /// What a call says of function literals: one that is called where
    /// it is written, a variable that is called, and the literals among
    /// the arguments.
    fn called_literals(&mut self, fun: ExprId, args: &[ExprId]) {
        let callee = self.unparen(fun);
        if let ExprKind::FuncLit { .. } = self.tree.exprs[callee].kind {
            self.direct.push(callee);
        }
        if let Some(local) = self.local(callee) {
            self.named.entry(local).or_default().1 += 1;
        }
        for (at, &arg) in args.iter().enumerate() {
            let arg = self.unparen(arg);
            if let ExprKind::FuncLit { .. } = self.tree.exprs[arg].kind {
                self.facts.asked.push((arg, callee, at));
            }
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
            if *called == Predeclared::Recover {
                match self.within.last() {
                    Some(&literal) => drop(self.facts.recovering.insert(literal)),
                    None => self.facts.recovers = true,
                }
            }
            let writes = matches!(
                called,
                Predeclared::Clear | Predeclared::Delete | Predeclared::CopyFunc
            );
            if let (true, Some(&first)) = (writes, args.first()) {
                self.more(first);
            }
            return;
        }
        if self.inside == 0 {
            let receiver = match self.tree.exprs[callee].kind {
                ExprKind::Selector { x, .. } => Some((x, None)),
                _ => None,
            };
            let given = args.iter().enumerate().map(|(at, &arg)| (arg, Some(at)));
            for (arg, at) in receiver.into_iter().chain(given) {
                let arg = self.unparen(arg);
                if let (true, Some(Ref::Local(local))) =
                    (self.body.last_uses.contains(&arg), self.body.uses.get(&arg))
                {
                    self.facts.passed.push((*local, callee, at));
                }
            }
        }
        if self.through_value(callee) {
            match self.within.last() {
                Some(&literal) => drop(self.facts.calling_values.insert(literal)),
                None => self.facts.calls_values = true,
            }
        }
        let ExprKind::Selector { x, .. } = &self.tree.exprs[callee].kind else {
            return;
        };
        let Some(selection) = self.body.selections.get(&callee) else {
            return;
        };
        if selection.kind == SelectionKind::Field {
            return;
        }
        // A method of an interface value works on the value that the
        // variable holds, and gets no address that outlives the call.
        if self.shared.types.is_interface(selection.holder) {
            let mut receiver = *x;
            while let ExprKind::Paren(inner) = self.tree.exprs[receiver].kind {
                receiver = inner;
            }
            if let (ExprKind::Name(_), Some(Ref::Local(local)), true) = (
                &self.tree.exprs[receiver].kind,
                self.body.uses.get(&receiver),
                selection.path.is_empty(),
            ) {
                self.called.insert(*local);
                return;
            }
        }
        if !self.takes_value(callee) {
            self.reached(*x);
        }
    }

    /// Whether a call of `callee` runs a function that is not known
    /// where the call is written: `callee` is no function by its name
    /// and no method of a type that has it.
    fn through_value(&self, callee: ExprId) -> bool {
        // A conversion calls nothing.
        if !self.body.types.contains_key(&callee) {
            return false;
        }
        let function = |package: &gopp_sema::Package, id: gopp_sema::ObjId| {
            let object = package.objects.get(id.0 as usize);
            object.is_some_and(|object| matches!(object.kind, gopp_sema::ObjKind::Func))
        };
        match (&self.tree.exprs[callee].kind, self.body.uses.get(&callee)) {
            (ExprKind::Name(_), Some(Ref::Object(object))) => !function(self.home, *object),
            (ExprKind::Selector { .. }, Some(Ref::Member { import, member })) => {
                match &self.home.object(*import).kind {
                    gopp_sema::ObjKind::Import { package, .. } => !function(package, *member),
                    _ => true,
                }
            }
            (ExprKind::Selector { sel, .. }, _) => match self.body.selections.get(&callee) {
                Some(selection)
                    if matches!(
                        selection.kind,
                        SelectionKind::Method | SelectionKind::BaseMethod
                    ) =>
                {
                    self.shared.method(selection.holder, sel.name).is_none()
                }
                _ => true,
            },
            _ => true,
        }
    }

    /// Whether the selector names a method of this package that takes
    /// its receiver by value. Of any other method it is not known, and
    /// may be by its address.
    fn takes_value(&self, callee: ExprId) -> bool {
        let ExprKind::Selector { sel, .. } = &self.tree.exprs[callee].kind else {
            return false;
        };
        let Some(selection) = self.body.selections.get(&callee) else {
            return false;
        };
        let types = self.shared.types;
        match &*types.get(selection.holder) {
            CheckedType::Named { .. } => {
                let method = self.shared.method(selection.holder, sel.name);
                method.is_some_and(|method| {
                    !matches!(&*types.get(method.receiver), CheckedType::Pointer(_))
                })
            }
            _ => false,
        }
    }

    /// The receiver of a call of the selector, if the call may have
    /// its address: a method of a value that is no interface value and
    /// is not taken by value.
    fn by_address(&self, callee: ExprId) -> Option<ExprId> {
        let ExprKind::Selector { x, .. } = &self.tree.exprs[callee].kind else {
            return None;
        };
        let selection = self.body.selections.get(&callee)?;
        let method = selection.kind != SelectionKind::Field;
        let value = self.shared.types.is_interface(selection.holder) || self.takes_value(callee);
        (method && !value).then_some(*x)
    }

    /// A variable that is declared with the value: one that holds a
    /// function literal is noted.
    fn func_var(&mut self, local: Option<LocalId>, value: ExprId) {
        let value = self.unparen(value);
        if let (Some(local), ExprKind::FuncLit { .. }) = (local, &self.tree.exprs[value].kind) {
            self.func_vars.insert(local, value);
        }
    }

    fn stmt(&mut self, stmt: StmtId, rest: &mut Vec<Node>) {
        let tree = self.tree;
        match &tree.stmts[stmt].kind {
            StmtKind::Bad | StmtKind::Empty | StmtKind::Branch { .. } => {}
            StmtKind::Go(call) => {
                self.facts.all = true;
                rest.push(Node::Later(*call));
            }
            StmtKind::Defer(call) => {
                self.facts.all = true;
                // A literal that the statement calls runs where the
                // function is left; what else it names is kept.
                let inner = self.unparen(*call);
                match &tree.exprs[inner].kind {
                    ExprKind::Call { fun, args, .. } => {
                        let callee = self.unparen(*fun);
                        // The deferred calls of a function literal are
                        // those of the literal: what its `defer`
                        // statements call and hold is not that of the
                        // function.
                        let literal = matches!(tree.exprs[callee].kind, ExprKind::FuncLit { .. });
                        let own = literal && self.inside == 0;
                        match own {
                            true => self.deferred.push(callee),
                            false => rest.push(Node::Later(*fun)),
                        }
                        rest.extend(args.iter().map(|&arg| Node::Later(arg)));
                        // What the statement has the address of.
                        let receiver = self.by_address(callee);
                        // `delete` and `clear` work on the map where
                        // it is held.
                        let written = match self.body.uses.get(&callee) {
                            Some(Ref::Universe(Predeclared::Delete | Predeclared::Clear)) => {
                                args.first().copied()
                            }
                            _ => None,
                        };
                        let taken = args.iter().filter_map(|&arg| {
                            match self.tree.exprs[self.unparen(arg)].kind {
                                ExprKind::Unary {
                                    op: UnaryOp::Addr,
                                    x,
                                } => Some(x),
                                _ => None,
                            }
                        });
                        let roots: Vec<LocalId> = (receiver.into_iter().chain(written))
                            .chain(taken)
                            .filter_map(|place| self.root(place))
                            .collect();
                        if self.inside == 0 {
                            self.held.extend(roots);
                        }
                        if own {
                            self.literal(callee, rest);
                        }
                    }
                    _ => rest.push(Node::Later(*call)),
                }
            }
            StmtKind::Select { clauses } => {
                self.facts.all = true;
                for clause in clauses {
                    rest.extend(clause.comm.iter().map(|&stmt| Node::Stmt(stmt)));
                    rest.extend(clause.body.iter().map(|&stmt| Node::Stmt(stmt)));
                }
            }
            StmtKind::Labeled { stmt, .. } => rest.push(Node::Stmt(*stmt)),
            StmtKind::Block(block) => rest.extend(block.stmts.iter().map(|&stmt| Node::Stmt(stmt))),
            StmtKind::Expr(expr) => exprs(rest, &[*expr]),
            StmtKind::Send { chan, value } => exprs(rest, &[*chan, *value]),
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    if let DeclKind::Var { names, values, .. } = &tree.decl_nodes[decl].kind {
                        exprs(rest, values);
                        if let ([name], [value]) = (&names[..], &values[..]) {
                            let local = self.body.defs.get(&name.span.lo).copied();
                            self.func_var(local, *value);
                        }
                    }
                }
            }
            StmtKind::Assign { op, lhs, rhs } => {
                exprs(rest, lhs);
                exprs(rest, rhs);
                if let (AssignOp::Define, [left], [value]) = (op, &lhs[..], &rhs[..]) {
                    let local = self.body.defs.get(&tree.exprs[*left].span.lo).copied();
                    self.func_var(local, *value);
                }
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
