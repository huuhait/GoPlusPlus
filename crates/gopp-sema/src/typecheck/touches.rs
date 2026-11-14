//! Which variables of packages each function names, what it calls and
//! what it starts with `go`; and with that, the variables that a
//! goroutine may use: no plain write changes one of those (CON-18).
//!
//! A write to a variable of a package is noted where it is found. When
//! every body of the package is walked, what each `go` statement starts
//! is followed through the calls, also into the packages that are
//! imported. A variable that is reached so, and that is changed
//! outside `init`, is refused where it is changed.

use std::collections::hash_map::Entry;

use super::borrows::Callee;
use super::place::Fixed;
use super::*;
use crate::package::Program;
use crate::types::{Global, Touch, TouchKey};

/// What a call runs or a `go` statement starts.
#[derive(Clone, Copy)]
pub(super) enum Target {
    /// A function literal of the same body, by its number there.
    Unit(usize),
    Known(Callee),
    /// A function of a header: it names no variable of a package.
    Nothing,
    /// A function value: which function runs is not known.
    Value,
    /// A method of an interface, or one that a child may replace, by
    /// its name.
    Named(Spur),
}

/// A call that runs while a pointer into what a variable of a package
/// owns apart is used later, or while a loop ranges over the variable:
/// if what it runs may free or move that, it is refused.
#[derive(Clone)]
pub(super) struct Run {
    pub ran: super::borrows::Ran,
    /// What is called, as it is written.
    pub called: String,
    pub global: Global,
    /// The variable that holds the pointer; none for a loop.
    pub holder: Option<String>,
    pub made: Span,
}

/// A function, a function literal, `init` or the value of a variable
/// of the package, with what it does itself.
#[derive(Clone)]
pub(super) struct Unit {
    pub owner: Option<Callee>,
    /// Where it is written; and whether it is `init` or the value of
    /// a variable, which run before the program has started.
    pub at: Span,
    pub starting: bool,
    /// The unit that a function literal is written in.
    pub parent: Option<usize>,
    pub uses: Vec<Global>,
    /// The variables that it changes, each where, and whether the
    /// write may free or move what the variable owns apart. With no
    /// variable: through a pointer that may point into one.
    pub writes: Vec<(Option<Global>, Span, bool)>,
    pub addressed: Vec<Global>,
    /// What it calls, each with where.
    pub calls: Vec<(Target, u32)>,
    pub spawns: Vec<(Target, Span)>,
    /// The functions that it uses as values.
    pub values: Vec<Target>,
}

impl Unit {
    fn new(at: Span, parent: Option<usize>) -> Unit {
        Unit {
            owner: None,
            at,
            starting: false,
            parent,
            uses: Vec::new(),
            writes: Vec::new(),
            addressed: Vec::new(),
            calls: Vec::new(),
            spawns: Vec::new(),
            values: Vec::new(),
        }
    }
}

impl<'a> Checker<'a> {
    /// The body of the declaration at `at` is walked from here on.
    pub(super) fn touch_start(&mut self, at: Span) {
        self.touched = vec![Unit::new(at, None)];
        self.touching = 0;
        self.literal_units.clear();
    }

    /// A function literal is entered. The unit that was walked comes
    /// back, for `touch_leave`.
    pub(super) fn touch_enter(&mut self, literal: ExprId, at: Span) -> usize {
        let outer = self.touching;
        self.literal_units.insert(literal, self.touched.len());
        self.touched.push(Unit::new(at, Some(outer)));
        self.touching = self.touched.len() - 1;
        outer
    }

    pub(super) fn touch_leave(&mut self, outer: usize) {
        self.touching = outer;
    }

    fn path_of(&self, package: &Package) -> Spur {
        self.names.get_or_intern(&package.path)
    }

    /// The variable of a package that the expression names.
    pub(super) fn global(&self, cx: &Cx<'a>, expr: ExprId) -> std::option::Option<Global> {
        let variable = |package: &Package, id: ObjId| {
            let object = package.objects.get(id.0 as usize);
            object.is_some_and(|object| matches!(object.kind, ObjKind::Var))
        };
        match cx.body.uses.get(&expr) {
            Some(&Ref::Object(object)) if variable(self.package, object) => {
                Some((self.path_of(self.package), object))
            }
            Some(&Ref::Member { import, member }) => match &self.package.object(import).kind {
                ObjKind::Import { package, .. } if variable(package, member) => {
                    Some((self.path_of(package), member))
                }
                _ => None,
            },
            _ => None,
        }
    }

    fn touching(&mut self) -> std::option::Option<&mut Unit> {
        self.touched.get_mut(self.touching)
    }

    /// The expression names a variable of a package or a function: the
    /// unit uses it.
    pub(super) fn touch_named(&mut self, cx: &Cx<'a>, expr: ExprId) {
        if self.memo.is_none() {
            return;
        }
        if let Some(global) = self.global(cx, expr) {
            if let Some(unit) = self.touching() {
                unit.uses.push(global);
            }
            return;
        }
        // A function that is not called here is a value.
        if self.callee == Some(expr) {
            return;
        }
        let function = match cx.body.uses.get(&expr) {
            Some(&Ref::Object(object)) => matches!(self.package.object(object).kind, ObjKind::Func)
                .then_some(Callee::Func(object)),
            Some(&Ref::Member { import, member }) => Some(Callee::Imported(import, member)),
            _ => None,
        };
        if let (Some(function), Some(unit)) = (function, self.touching()) {
            unit.values.push(Target::Known(function));
        }
    }

    /// A variable of a package is changed at `at`; with none, through
    /// a pointer that may point into one. What `init` and the value of
    /// a variable do themselves is done before the program starts.
    pub(super) fn touch_write(
        &mut self,
        global: std::option::Option<Global>,
        at: Span,
        replaces: bool,
    ) {
        if self.memo.is_none() {
            let within = self.late_within.unwrap_or(at);
            self.late_writes.push((global, at, within, replaces));
            return;
        }
        if self.starting && self.literals.is_empty() {
            return;
        }
        if let Some(unit) = self.touching() {
            unit.writes.push((global, at, replaces));
        }
    }

    /// A write to what is only read is found at `at`. Of a variable of
    /// a package it is noted, for the end of the package; whether it
    /// is that.
    pub(super) fn noted(&mut self, fixed: Fixed, at: Span) -> bool {
        self.noted_as(fixed, at, true)
    }

    /// `noted`, of a write that frees or moves what the variable owns
    /// apart from itself only with `replaces`.
    pub(super) fn noted_as(&mut self, fixed: Fixed, at: Span, replaces: bool) -> bool {
        match fixed {
            Fixed::Shared => false,
            Fixed::Package(global) => {
                self.touch_write(global, at, replaces);
                true
            }
        }
    }

    /// A pointer into a variable of a package is taken.
    pub(super) fn touch_address(&mut self, cx: &Cx<'a>, place: ExprId) {
        let mut root = self.unparen(cx, place);
        while let ExprKind::Selector { x, .. }
        | ExprKind::Index { x, .. }
        | ExprKind::Slice { x, .. } = cx.tree.exprs[root].kind
        {
            if cx.body.uses.contains_key(&root) {
                break;
            }
            root = self.unparen(cx, x);
        }
        if let (Some(global), Some(unit)) = (self.global(cx, root), self.touching()) {
            unit.addressed.push(global);
        }
    }

    /// The function `fun` is called.
    pub(super) fn touch_call(&mut self, cx: &Cx<'a>, fun: ExprId) {
        let target = self.target(cx, fun);
        let at = cx.tree.exprs[fun].span.lo;
        if let Some(unit) = self.touching() {
            unit.calls.push((target, at));
        }
    }

    /// A `go` statement starts the call.
    pub(super) fn touch_spawn(&mut self, cx: &Cx<'a>, call: ExprId) {
        let ExprKind::Call { fun, .. } = cx.tree.exprs[call].kind else {
            return;
        };
        let target = self.target(cx, fun);
        let at = cx.tree.exprs[call].span;
        if let Some(unit) = self.touching() {
            unit.spawns.push((target, at));
        }
    }

    /// A method is taken as a value.
    pub(super) fn touch_method_value(&mut self, cx: &Cx<'a>, selector: ExprId) {
        let target = self.target(cx, selector);
        if let Some(unit) = self.touching() {
            unit.values.push(target);
        }
    }

    /// What a call of `fun` runs.
    fn target(&self, cx: &Cx<'a>, fun: ExprId) -> Target {
        let mut fun = self.unparen(cx, fun);
        if let ExprKind::Index { x, .. } = cx.tree.exprs[fun].kind
            && self.instantiated.contains_key(&fun)
        {
            fun = self.unparen(cx, x);
        }
        if let Some(&unit) = self.literal_units.get(&fun) {
            return Target::Unit(unit);
        }
        let function = |object: std::option::Option<&crate::Object>| {
            object.is_some_and(|object| matches!(object.kind, ObjKind::Func))
        };
        match cx.body.uses.get(&fun) {
            Some(&Ref::Object(object)) => {
                return match function(Some(self.package.object(object))) {
                    true => Target::Known(Callee::Func(object)),
                    false => Target::Value,
                };
            }
            Some(&Ref::Member { import, member }) => {
                return match &self.package.object(import).kind {
                    ObjKind::Import { package, .. } if package.foreign.is_some() => Target::Nothing,
                    ObjKind::Import { package, .. }
                        if function(package.objects.get(member.0 as usize)) =>
                    {
                        Target::Known(Callee::Imported(import, member))
                    }
                    _ => Target::Value,
                };
            }
            Some(_) => return Target::Value,
            None => {}
        }
        let ExprKind::Selector { sel, .. } = &cx.tree.exprs[fun].kind else {
            return Target::Value;
        };
        let Some(selection) = self.selections.get(&fun) else {
            return Target::Value;
        };
        let method = matches!(
            selection.kind,
            SelectionKind::BaseMethod | SelectionKind::Method | SelectionKind::MethodExpr
        );
        if !method {
            return Target::Value;
        }
        let Type::Named { id, .. } = &*self.types.get(selection.holder) else {
            return Target::Named(sel.name);
        };
        let interface = matches!(
            &*self.types.get(self.types.underlying(selection.holder)),
            Type::Interface { .. }
        );
        // A child may replace the method of a type with a table.
        let replaced = interface || self.types.has_table(selection.holder);
        match (selection.kind, replaced) {
            (SelectionKind::BaseMethod, _) | (_, false) => {
                Target::Known(Callee::Method(*id, sel.name))
            }
            _ => Target::Named(sel.name),
        }
    }

    /// The units of the body that was walked, the first of them the
    /// function that others call by the name `owner`.
    pub(super) fn touch_end(&mut self, owner: std::option::Option<Callee>) -> Vec<Unit> {
        self.literal_units.clear();
        let starting = self.starting;
        if let Some(unit) = self.touched.first_mut() {
            (unit.owner, unit.starting) = (owner, starting);
        }
        std::mem::take(&mut self.touched)
    }

    /// The units of a body join those of the package.
    pub(super) fn touch_merge(&mut self, units: Vec<Unit>) {
        let base = self.units.len();
        let moved = |target: Target| match target {
            Target::Unit(unit) => Target::Unit(unit + base),
            other => other,
        };
        for mut unit in units {
            unit.parent = unit.parent.map(|parent| parent + base);
            for (call, _) in &mut unit.calls {
                *call = moved(*call);
            }
            for value in &mut unit.values {
                *value = moved(*value);
            }
            for (target, _) in &mut unit.spawns {
                *target = moved(*target);
            }
            self.units.push(unit);
        }
    }

    fn touch_key(&self, keys: &[TouchKey], target: Target) -> std::option::Option<TouchKey> {
        let path = self.path_of(self.package);
        match target {
            Target::Unit(unit) => keys.get(unit).copied(),
            Target::Known(Callee::Func(object)) => Some(TouchKey::Func(path, object)),
            Target::Known(Callee::Imported(import, member)) => {
                match &self.package.object(import).kind {
                    ObjKind::Import { package, .. } => {
                        Some(TouchKey::Func(self.path_of(package), member))
                    }
                    _ => None,
                }
            }
            Target::Known(Callee::Method(named, name)) => Some(TouchKey::Method(named, name)),
            Target::Known(Callee::Unknown) => None,
            Target::Nothing | Target::Value | Target::Named(_) => None,
        }
    }

    /// All that is reached from the functions, through the calls. A
    /// call of a function value may run every function that is used
    /// as a value, and a call of a method of an interface every
    /// method of its name.
    fn reach(
        &mut self,
        program: &Program,
        from: &[(TouchKey, std::option::Option<Span>)],
    ) -> Reached {
        let mut reached = Reached::default();
        let mut named: FxHashSet<Spur> = FxHashSet::default();
        let mut valued = false;
        let mut rest: Vec<_> = from.iter().rev().copied().collect();
        while let Some((key, by)) = rest.pop() {
            self.steps += 1;
            if !reached.seen.insert(key) {
                continue;
            }
            // What nothing is known of may run what a function value
            // may.
            let touch = self.types.touch(key);
            let values = touch.as_ref().is_none_or(|touch| touch.values);
            if values && !valued {
                valued = true;
                self.steps += program.valued.len() as u64;
                rest.extend(program.valued.iter().map(|&key| (key, by)));
            }
            let Some(touch) = touch else {
                continue;
            };
            for &global in &touch.uses {
                if let Entry::Vacant(free) = reached.uses.entry(global) {
                    free.insert(by);
                }
            }
            reached.writes.extend(&touch.writes);
            reached.writes_any |= touch.writes_any;
            reached.replaces.extend(&touch.replaces);
            self.steps += (touch.calls.len() + touch.methods.len() + touch.starts.len()) as u64;
            rest.extend(touch.calls.iter().map(|&key| (key, by)));
            // What a goroutine starts is reached too.
            reached.starts.extend(&touch.starts);
            rest.extend(touch.starts.iter().map(|&key| (key, by)));
            for name in touch.methods.iter().filter(|name| named.insert(**name)) {
                let methods = program.methods.get(name).into_iter().flatten();
                rest.extend(methods.map(|&key| (key, by)));
            }
        }
        reached
    }

    /// Whether the function may start a goroutine, itself or in what
    /// it calls; what is not known may. `calm` has the functions that
    /// are known not to, and gets those that are found.
    fn may_spawn(&mut self, key: TouchKey, calm: &mut FxHashSet<TouchKey>) -> bool {
        let mut seen: FxHashSet<TouchKey> = FxHashSet::default();
        let mut rest = vec![key];
        while let Some(key) = rest.pop() {
            self.steps += 1;
            if calm.contains(&key) || !seen.insert(key) {
                continue;
            }
            let Some(touch) = self.types.touch(key) else {
                return true;
            };
            if !touch.starts.is_empty() || touch.values || !touch.methods.is_empty() {
                return true;
            }
            self.steps += touch.calls.len() as u64;
            rest.extend(&touch.calls);
        }
        calm.extend(seen);
        false
    }

    /// What a call adds to what its function says.
    fn touch_called(&self, keys: &[TouchKey], call: Target, touch: &mut Touch) {
        match (call, self.touch_key(keys, call)) {
            (Target::Nothing, _) => {}
            (Target::Named(name), _) => touch.methods.push(name),
            (_, Some(key)) => touch.calls.push(key),
            (_, None) => touch.values = true,
        }
    }

    /// Where `main` may start its first goroutine: before the
    /// statement there, no goroutine runs beside it. With the `defer`
    /// statements before that place, whose calls run at the end.
    /// `main` is the unit with the number `at`.
    fn first_spawn(
        &mut self,
        files: &[crate::resolve::Unit<'a>],
        (units, keys): (&[Unit], &[TouchKey]),
        at: usize,
        calm: &mut FxHashSet<TouchKey>,
    ) -> (u32, Vec<Span>) {
        let unit = &units[at];
        let body = files.iter().find_map(|file| {
            let declared = |decl: &ast::DeclId| {
                let node = &file.file.decl_nodes[*decl];
                match &node.kind {
                    DeclKind::Func {
                        body: Some(body), ..
                    } if node.span == unit.at => Some((file.file, body)),
                    _ => None,
                }
            };
            file.file.decls.iter().find_map(declared)
        });
        let Some((file, body)) = body else {
            return (unit.at.lo, Vec::new());
        };
        // What may start a goroutine, in the order of the source: a
        // `go` statement, a call, a function literal.
        let mut events: Vec<(u32, std::option::Option<TouchKey>, bool)> = Vec::new();
        events.extend(unit.spawns.iter().map(|(_, span)| (span.lo, None, true)));
        for &(call, lo) in &unit.calls {
            match (call, self.touch_key(keys, call)) {
                (Target::Nothing, _) => {}
                (_, Some(key)) => events.push((lo, Some(key), false)),
                (_, None) => events.push((lo, None, true)),
            }
        }
        for (child, inner) in units.iter().enumerate() {
            self.steps += 1;
            if inner.parent == Some(at) {
                events.push((inner.at.lo, Some(keys[child]), false));
            }
        }
        events.sort_unstable_by_key(|event| event.0);
        let mut next = 0;
        let mut deferred = Vec::new();
        for &stmt in &body.stmts {
            let node = &file.stmts[stmt];
            // A jump back to a label would run what is before it again.
            if matches!(node.kind, StmtKind::Labeled { .. }) {
                return (node.span.lo, deferred);
            }
            let waits = matches!(node.kind, StmtKind::Defer(_));
            if waits {
                deferred.push(node.span);
            }
            while let Some(&(lo, key, spawns)) = events.get(next).filter(|e| e.0 < node.span.hi) {
                next += 1;
                self.steps += 1;
                if lo < node.span.lo || waits {
                    continue;
                }
                if spawns || key.is_some_and(|key| self.may_spawn(key, calm)) {
                    return (node.span.lo, deferred);
                }
            }
        }
        (body.span.hi, deferred)
    }

    pub(super) fn global_name(&self, (path, object): Global) -> String {
        if path == self.path_of(self.package) {
            return self
                .names
                .resolve(&self.package.object(object).name)
                .to_owned();
        }
        let imported = self
            .package
            .objects
            .iter()
            .find_map(|import| match &import.kind {
                ObjKind::Import { package, .. } if self.path_of(package) == path => Some(package),
                _ => None,
            });
        match imported.and_then(|package| package.objects.get(object.0 as usize)) {
            Some(variable) => {
                let package = imported.map_or("", |package| &package.name);
                format!("{package}.{}", self.names.resolve(&variable.name))
            }
            None => format!("a variable of {}", self.names.resolve(&path)),
        }
    }

    /// When every body of the package is walked: says what each of its
    /// functions names and calls, follows what the `go` statements
    /// start, and refuses the writes to a variable that a goroutine
    /// may use. In the package `main`, a write that runs before the
    /// first goroutine may be started is none of them. `first` is
    /// where the package starts, for what is found between two
    /// packages that it imports.
    pub(super) fn races(
        &mut self,
        files: &[crate::resolve::Unit<'a>],
        first: std::option::Option<Span>,
    ) -> Program {
        let path = self.path_of(self.package);
        let mut units = std::mem::take(&mut self.units);
        // The writes that were found when every body was walked go to
        // the function that they are in.
        let within = |at: Span| (at.file, at.lo, at.hi);
        let by_span: FxHashMap<_, usize> = (units.iter().enumerate())
            .map(|(at, unit)| (within(unit.at), at))
            .collect();
        let mut loose: Vec<(std::option::Option<Global>, Span, bool)> = Vec::new();
        for (global, at, inside, replaces) in std::mem::take(&mut self.late_writes) {
            self.steps += 1;
            match by_span.get(&within(inside)) {
                Some(&unit) => units[unit].writes.push((global, at, replaces)),
                None => loose.push((global, at, replaces)),
            }
        }
        let mut keys: Vec<TouchKey> = (units.iter().enumerate())
            .map(|(at, unit)| match unit.owner {
                Some(Callee::Func(object)) => TouchKey::Func(path, object),
                Some(Callee::Method(named, name)) => TouchKey::Method(named, name),
                _ => TouchKey::Body(path, at as u32),
            })
            .collect();
        // A function literal that a `go` statement starts itself runs
        // nowhere else; another one runs where its function may.
        let mut started: FxHashSet<usize> = FxHashSet::default();
        for unit in &units {
            for (target, _) in &unit.spawns {
                if let Target::Unit(literal) = target {
                    started.insert(*literal);
                }
            }
        }
        let mut inner: Vec<Vec<TouchKey>> = vec![Vec::new(); units.len()];
        for (at, unit) in units.iter().enumerate() {
            if let Some(parent) = unit.parent.filter(|_| !started.contains(&at)) {
                inner[parent].push(keys[at]);
            }
        }
        // What a `go` statement starts. Of a function value or of a
        // method of an interface, a function that calls it stands in.
        let mut program = Program::default();
        let mut roots: Vec<(TouchKey, std::option::Option<Span>)> = Vec::new();
        let mut starts: Vec<Vec<TouchKey>> = vec![Vec::new(); units.len()];
        let mut started_at: Vec<(usize, TouchKey, Span)> = Vec::new();
        for (at, unit) in units.iter().enumerate() {
            for &(target, span) in &unit.spawns {
                self.steps += 1;
                let key = match (target, self.touch_key(&keys, target)) {
                    (Target::Nothing, _) => continue,
                    (_, Some(key)) => key,
                    (target, None) => {
                        let mut stands = Touch::default();
                        self.touch_called(&keys, target, &mut stands);
                        let key = TouchKey::Body(path, keys.len() as u32);
                        keys.push(key);
                        self.types.set_touch(key, Arc::new(stands));
                        key
                    }
                };
                starts[at].push(key);
                started_at.push((at, key, span));
                roots.push((key, Some(span)));
                program.roots.insert(key);
            }
        }
        for (at, unit) in units.iter().enumerate() {
            self.steps += 1;
            let mut touch = Touch {
                uses: unit.uses.clone(),
                calls: std::mem::take(&mut inner[at]),
                starts: std::mem::take(&mut starts[at]),
                ..Touch::default()
            };
            touch.uses.sort_unstable();
            touch.uses.dedup();
            for &(call, _) in &unit.calls {
                self.touch_called(&keys, call, &mut touch);
            }
            for &(global, _, replaces) in &unit.writes {
                match global {
                    Some(global) => {
                        touch.writes.push(global);
                        if replaces {
                            touch.replaces.push(global);
                        }
                    }
                    None => touch.writes_any = true,
                }
            }
            touch.writes.sort_unstable();
            touch.writes.dedup();
            touch.replaces.sort_unstable();
            touch.replaces.dedup();
            let mut seen = FxHashSet::default();
            touch.calls.retain(|key| seen.insert(*key));
            touch.methods.sort_unstable();
            touch.methods.dedup();
            program.written.extend(&touch.writes);
            program.written_any |= touch.writes_any;
            self.types.set_touch(keys[at], Arc::new(touch));
            // What is not known may run a function literal, or a
            // function or a method that is used as a value.
            if unit.parent.is_some() {
                program.valued.insert(keys[at]);
            }
            for &value in &unit.values {
                program.valued.extend(self.touch_key(&keys, value));
            }
            if let Some(Callee::Method(_, name)) = unit.owner {
                program.methods.entry(name).or_default().insert(keys[at]);
            }
            program.addressed.extend(unit.addressed.iter().copied());
        }
        for &(global, _, _) in &loose {
            match global {
                Some(global) => drop(program.written.insert(global)),
                None => program.written_any = true,
            }
        }
        // With what the imported packages do.
        let mut refused: FxHashSet<Global> = FxHashSet::default();
        for object in &self.package.objects {
            let ObjKind::Import { package, .. } = &object.kind else {
                continue;
            };
            let of = &package.program;
            self.steps += 1;
            let mut others: Vec<TouchKey> = (of.roots.iter().copied())
                .filter(|root| program.roots.insert(*root))
                .collect();
            others.sort_unstable_by_key(|key| format!("{key:?}"));
            roots.extend(others.into_iter().map(|root| (root, None)));
            program.valued.extend(&of.valued);
            for (name, methods) in &of.methods {
                program.methods.entry(*name).or_default().extend(methods);
            }
            program.written.extend(&of.written);
            program.written_any |= of.written_any;
            program.addressed.extend(&of.addressed);
            program.early |= of.early;
            refused.extend(&of.refused);
        }
        program.refused = refused;
        if roots.is_empty() {
            return program;
        }
        let mut calm: FxHashSet<TouchKey> = FxHashSet::default();
        for (at, unit) in units.iter().enumerate() {
            if unit.starting && unit.parent.is_none() && !program.early {
                program.early = self.may_spawn(keys[at], &mut calm);
            }
        }
        // In the package `main` the program starts at `main`, after
        // `init` and the values of the variables: only what is reached
        // from there runs, and only its `go` statements start
        // goroutines. A write may run beside a goroutine if a
        // goroutine reaches it, or `main` once it may have started
        // one. In another package nothing says who calls its
        // functions when: every write and every `go` statement count.
        let main = units.iter().position(|unit| match unit.owner {
            Some(Callee::Func(object)) => {
                self.names.resolve(&self.package.object(object).name) == "main"
            }
            _ => false,
        });
        let main = main.filter(|_| self.package.name == "main");
        let mut later: std::option::Option<(usize, u32, Vec<Span>, FxHashSet<TouchKey>)> = None;
        let (used, written, written_any) = match main {
            Some(at) => {
                let before: Vec<_> = (units.iter().enumerate())
                    .filter(|(_, unit)| unit.starting && unit.parent.is_none())
                    .map(|(at, _)| (keys[at], None))
                    .collect();
                let mut all = before.clone();
                all.push((keys[at], None));
                let live = self.reach(&program, &all);
                // Each with a `go` statement that runs.
                let mut spans: FxHashMap<TouchKey, Span> = FxHashMap::default();
                for &(unit, key, span) in &started_at {
                    if live.seen.contains(&keys[unit]) {
                        spans.entry(key).or_insert(span);
                    }
                }
                let roots: Vec<_> = (live.starts.iter())
                    .map(|key| (*key, spans.get(key).copied()))
                    .collect();
                let used = self.reach(&program, &roots).uses;
                let (cut, deferred) = match program.early {
                    true => (units[at].at.lo, Vec::new()),
                    false => self.first_spawn(files, (&units, &keys), at, &mut calm),
                };
                let after = |lo: u32| {
                    lo >= cut || deferred.iter().any(|wait| wait.lo <= lo && lo < wait.hi)
                };
                let mut rest = Touch::default();
                for &(call, _) in units[at].calls.iter().filter(|(_, lo)| after(*lo)) {
                    self.touch_called(&keys, call, &mut rest);
                }
                // A function literal of `main` may run at any time.
                for (child, unit) in units.iter().enumerate() {
                    if unit.parent == Some(at) {
                        rest.calls.push(keys[child]);
                    }
                }
                for &(global, _, _) in units[at].writes.iter().filter(|(_, at, _)| after(at.lo)) {
                    match global {
                        Some(global) => rest.writes.push(global),
                        None => rest.writes_any = true,
                    }
                }
                let key = TouchKey::Body(path, keys.len() as u32);
                keys.push(key);
                self.types.set_touch(key, Arc::new(rest));
                let mut from = roots;
                from.push((key, None));
                // A goroutine that `init` starts runs beside all that
                // `init` does.
                if program.early {
                    from.extend(before);
                }
                let reached = self.reach(&program, &from);
                later = Some((at, cut, deferred, reached.seen));
                (used, reached.writes, reached.writes_any)
            }
            None => {
                let used = self.reach(&program, &roots).uses;
                (used, program.written.clone(), program.written_any)
            }
        };
        let is_written = |global: &Global| {
            written.contains(global) || (written_any && program.addressed.contains(global))
        };
        let mut raced: Vec<Global> = (used.keys())
            .filter(|global| is_written(global) && !program.refused.contains(global))
            .copied()
            .collect();
        raced.sort_unstable();
        // The writes of this package that may run then.
        let mut writes: Vec<(std::option::Option<Global>, Span)> =
            loose.iter().map(|&(global, at, _)| (global, at)).collect();
        for (at, unit) in units.iter().enumerate() {
            self.steps += 1;
            let runs = |span: Span| match &later {
                None => true,
                Some((main, cut, deferred, seen)) => {
                    let after = span.lo >= *cut
                        || (deferred.iter()).any(|wait| wait.lo <= span.lo && span.lo < wait.hi);
                    seen.contains(&keys[at]) || (at == *main && after)
                }
            };
            let sites = unit.writes.iter().map(|&(global, at, _)| (global, at));
            writes.extend(sites.filter(|&(_, span)| runs(span)));
        }
        writes.sort_unstable_by_key(|&(_, at)| (at.file, at.lo, at.hi));
        writes.dedup();
        // What a pointer that is not followed to one variable may
        // point into: the first of these are named.
        let through: Vec<Global> = (raced.iter())
            .filter(|global| program.addressed.contains(global))
            .copied()
            .collect();
        let pointed: Vec<String> = (through.iter().take(3))
            .map(|&global| self.global_name(global))
            .collect();
        let pointed = pointed.join(", ");
        let fix = "declare it as a sync.Atomic or a sync.Mutex, or set it in init";
        let mut said: FxHashSet<Global> = FxHashSet::default();
        for &(global, at) in &writes {
            let (of, message) = match (global, through.first()) {
                (Some(global), _) if raced.binary_search(&global).is_ok() => {
                    let name = self.global_name(global);
                    let message = format!(
                        "cannot change {name} here: a goroutine may use it at the same time"
                    );
                    (global, message)
                }
                (None, Some(&global)) => {
                    let message = format!(
                        "cannot write through this pointer: it may point into a variable of a \
                         package that a goroutine may use at the same time ({pointed})"
                    );
                    (global, message)
                }
                _ => continue,
            };
            said.insert(of);
            if !self.report_once((at, Code::ConGlobalWrite)) {
                continue;
            }
            let mut diagnostic = Diagnostic::new(Code::ConGlobalWrite, at, message);
            if let Some(&Some(root)) = used.get(&of) {
                let note = "a goroutine that may use it is started here".to_owned();
                diagnostic = diagnostic.with_note(Code::NoteGoFixShare, Some(root), note);
            }
            self.emit(diagnostic.with_note(Code::NoteGoFixShare, None, fix.to_owned()));
        }
        // What is changed in another package is said where the
        // goroutine is started, or where the two packages meet.
        for &global in raced.iter().filter(|global| !said.contains(global)) {
            let name = self.global_name(global);
            let (at, message) = match (used.get(&global).copied().flatten(), first) {
                (Some(root), _) => (
                    root,
                    format!(
                        "cannot start this goroutine: it may use {name}, which is changed \
                         outside init"
                    ),
                ),
                (None, Some(first)) => (
                    first,
                    format!(
                        "a goroutine of a package that is imported may use {name}, which is \
                         changed outside init"
                    ),
                ),
                (None, None) => continue,
            };
            let diagnostic = Diagnostic::new(Code::ConGlobalWrite, at, message);
            self.emit(diagnostic.with_note(Code::NoteGoFixShare, None, fix.to_owned()));
        }
        program.refused.extend(raced);
        program
    }
}

impl<'a> Checker<'a> {
    /// The calls that run while a pointer into what a variable of a
    /// package owns apart is used, or a loop ranges over it: one whose
    /// function may free or move that, itself or in what it calls, is
    /// refused. Asked when what every function changes is known.
    pub(super) fn replaced_under_pointers(&mut self) {
        let program = std::mem::take(&mut self.program);
        let mut known: FxHashMap<Vec<TouchKey>, Arc<Reached>> = FxHashMap::default();
        for run in std::mem::take(&mut self.pending_runs) {
            self.steps += 1;
            // A function that is not known: every method of the name,
            // or every function that is used as a value.
            let from: Vec<TouchKey> = match (run.ran.callee, run.ran.method) {
                (Callee::Unknown, Some(name)) => {
                    let mut all: Vec<TouchKey> = program
                        .methods
                        .get(&name)
                        .into_iter()
                        .flatten()
                        .copied()
                        .collect();
                    all.sort_unstable_by_key(|key| format!("{key:?}"));
                    all
                }
                (Callee::Unknown, None) => Vec::new(),
                (callee, _) => (self.touch_key(&[], Target::Known(callee)).into_iter()).collect(),
            };
            let reached = match known.get(&from) {
                Some(reached) => reached.clone(),
                None => {
                    let reached = match from.is_empty() {
                        // All that a function value may run.
                        true => {
                            let stands = TouchKey::Body(self.path_of(self.package), u32::MAX);
                            self.reach(&program, &[(stands, None)])
                        }
                        false => {
                            let from: Vec<_> = from.iter().map(|&key| (key, None)).collect();
                            self.reach(&program, &from)
                        }
                    };
                    let reached = Arc::new(reached);
                    known.insert(from, reached.clone());
                    reached
                }
            };
            let replaced = reached.replaces.contains(&run.global)
                || (reached.writes_any && program.addressed.contains(&run.global));
            if !replaced || !self.report_once((run.ran.at, Code::OwnBorrowedAssign)) {
                continue;
            }
            let name = self.global_name(run.global);
            let message = match &run.holder {
                Some(holder) => format!(
                    "cannot call {}: it may change {name}, and {holder} points into {name} and \
                     is used later",
                    run.called
                ),
                None => format!(
                    "cannot call {}: it may change {name}, which the loop ranges over",
                    run.called
                ),
            };
            let code = match run.holder {
                Some(_) => Code::OwnBorrowedAssign,
                None => Code::OwnRangeMutation,
            };
            let note = match run.holder {
                Some(_) => "the pointer is made here",
                None => "the loop is here",
            };
            let diagnostic = Diagnostic::new(code, run.ran.at, message);
            self.emit(diagnostic.with_note(Code::NoteGoFixShare, Some(run.made), note.to_owned()));
        }
        self.program = program;
    }
}

/// What is reached from some functions.
#[derive(Default)]
struct Reached {
    seen: FxHashSet<TouchKey>,
    /// The variables that are named, each with the `go` statement of
    /// this package that leads to it first, if one does.
    uses: FxHashMap<Global, std::option::Option<Span>>,
    writes: FxHashSet<Global>,
    writes_any: bool,
    replaces: FxHashSet<Global>,
    /// What the `go` statements on the way start.
    starts: Vec<TouchKey>,
}
