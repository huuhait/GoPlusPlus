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

/// A function, a function literal, `init` or the value of a variable
/// of the package, with what it does itself.
#[derive(Clone, Default)]
pub(super) struct Unit {
    pub owner: Option<Callee>,
    /// The unit that a function literal is written in.
    pub parent: Option<usize>,
    pub uses: Vec<Global>,
    /// The variables that it changes, each where. With no variable:
    /// through a pointer that may point into one.
    pub writes: Vec<(Option<Global>, Span)>,
    pub addressed: Vec<Global>,
    pub calls: Vec<Target>,
    pub spawns: Vec<(Target, Span)>,
    /// The functions that it uses as values.
    pub values: Vec<Target>,
}

impl<'a> Checker<'a> {
    /// The body of a declaration is walked from here on.
    pub(super) fn touch_start(&mut self) {
        self.touched = vec![Unit::default()];
        self.touching = 0;
        self.literal_units.clear();
    }

    /// A function literal is entered. The unit that was walked comes
    /// back, for `touch_leave`.
    pub(super) fn touch_enter(&mut self, literal: ExprId) -> usize {
        let outer = self.touching;
        self.literal_units.insert(literal, self.touched.len());
        self.touched.push(Unit {
            parent: Some(outer),
            ..Unit::default()
        });
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
    pub(super) fn touch_write(&mut self, global: std::option::Option<Global>, at: Span) {
        if self.memo.is_none() {
            self.late_writes.push((global, at));
            return;
        }
        if self.starting && self.literals.is_empty() {
            return;
        }
        if let Some(unit) = self.touching() {
            unit.writes.push((global, at));
        }
    }

    /// A write to what is only read is found at `at`. Of a variable of
    /// a package it is noted, for the end of the package; whether it
    /// is that.
    pub(super) fn noted(&mut self, fixed: Fixed, at: Span) -> bool {
        match fixed {
            Fixed::Shared => false,
            Fixed::Package(global) => {
                self.touch_write(global, at);
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
        if let Some(unit) = self.touching() {
            unit.calls.push(target);
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
        if let Some(unit) = self.touched.first_mut() {
            unit.owner = owner;
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
            for call in &mut unit.calls {
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

    /// The variables that a goroutine may use: all that is named in
    /// what the `go` statements start, through the calls. A call of a
    /// function value may run every function that is used as a value,
    /// and a call of a method of an interface every method of its
    /// name. Each variable comes with the `go` statement of this
    /// package that leads to it first, if one does.
    fn used_by_goroutines(
        &mut self,
        program: &Program,
        roots: &[(TouchKey, std::option::Option<Span>)],
    ) -> FxHashMap<Global, std::option::Option<Span>> {
        let mut found: FxHashMap<Global, std::option::Option<Span>> = FxHashMap::default();
        let mut seen: FxHashSet<TouchKey> = FxHashSet::default();
        let mut named: FxHashSet<Spur> = FxHashSet::default();
        let mut valued = false;
        let mut rest: Vec<_> = roots.iter().rev().copied().collect();
        while let Some((key, by)) = rest.pop() {
            self.steps += 1;
            if !seen.insert(key) {
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
                if let Entry::Vacant(free) = found.entry(global) {
                    free.insert(by);
                }
            }
            self.steps += (touch.calls.len() + touch.methods.len()) as u64;
            rest.extend(touch.calls.iter().map(|&key| (key, by)));
            for name in touch.methods.iter().filter(|name| named.insert(**name)) {
                let methods = program.methods.get(name).into_iter().flatten();
                rest.extend(methods.map(|&key| (key, by)));
            }
        }
        found
    }

    fn global_name(&self, (path, object): Global) -> String {
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
    /// may use. `first` is where the package starts, for what is found
    /// between two packages that it imports.
    pub(super) fn races(&mut self, first: std::option::Option<Span>) -> Program {
        let path = self.path_of(self.package);
        let units = std::mem::take(&mut self.units);
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
        let mut program = Program::default();
        let mut roots: Vec<(TouchKey, std::option::Option<Span>)> = Vec::new();
        let mut writes: Vec<(std::option::Option<Global>, Span)> = Vec::new();
        for (at, unit) in units.iter().enumerate() {
            self.steps += 1;
            let mut touch = Touch {
                uses: unit.uses.clone(),
                calls: std::mem::take(&mut inner[at]),
                ..Touch::default()
            };
            touch.uses.sort_unstable();
            touch.uses.dedup();
            for &call in &unit.calls {
                match (call, self.touch_key(&keys, call)) {
                    (Target::Nothing, _) => {}
                    (Target::Named(name), _) => touch.methods.push(name),
                    (_, Some(key)) => touch.calls.push(key),
                    (_, None) => touch.values = true,
                }
            }
            let mut seen = FxHashSet::default();
            touch.calls.retain(|key| seen.insert(*key));
            touch.methods.sort_unstable();
            touch.methods.dedup();
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
            writes.extend(unit.writes.iter().copied());
            program.addressed.extend(unit.addressed.iter().copied());
        }
        // What a `go` statement starts. Of a function value or of a
        // method of an interface, a function that calls it stands in.
        for unit in &units {
            for &(target, span) in &unit.spawns {
                self.steps += 1;
                let key = match (target, self.touch_key(&keys, target)) {
                    (Target::Nothing, _) => continue,
                    (_, Some(key)) => key,
                    (target, None) => {
                        let stands = Touch {
                            values: !matches!(target, Target::Named(_)),
                            methods: match target {
                                Target::Named(name) => vec![name],
                                _ => Vec::new(),
                            },
                            ..Touch::default()
                        };
                        let key = TouchKey::Body(path, keys.len() as u32);
                        keys.push(key);
                        self.types.set_touch(key, Arc::new(stands));
                        key
                    }
                };
                roots.push((key, Some(span)));
                program.roots.insert(key);
            }
        }
        writes.append(&mut self.late_writes);
        writes.sort_unstable_by_key(|&(_, at)| (at.file, at.lo, at.hi));
        writes.dedup();
        for &(global, _) in &writes {
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
            refused.extend(&of.refused);
        }
        let used = match roots.is_empty() {
            true => FxHashMap::default(),
            false => self.used_by_goroutines(&program, &roots),
        };
        let written = |global: &Global| {
            program.written.contains(global)
                || (program.written_any && program.addressed.contains(global))
        };
        let mut raced: Vec<Global> = (used.keys())
            .filter(|global| written(global) && !refused.contains(global))
            .copied()
            .collect();
        raced.sort_unstable();
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
        refused.extend(raced);
        program.refused = refused;
        program
    }
}
