//! The order among the declarations of a package, and the cycles that are
//! errors: a type that contains itself, a declaration that needs itself,
//! variables whose initial values need each other.
//!
//! All of it is read from what the names denote; no type is computed.

use std::cmp::Reverse;
use std::collections::BinaryHeap;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::Span;
use gopp_syntax::ast::*;
use lasso::ThreadedRodeo;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::object::{Body, ObjId, ObjKind, Ref};
use crate::package::Package;
use crate::resolve::Unit;
use crate::universe::Predeclared;

/// One step of the start of a program: the variables on the left get the
/// value on the right.
#[derive(Debug, PartialEq, Eq)]
pub struct Initializer {
    pub lhs: Vec<ObjId>,
    /// Index of the file in [`Package::files`].
    pub file: u32,
    pub value: ExprId,
}

/// What a declaration refers to.
#[derive(Default)]
struct Node {
    /// The objects that its type and its value name, functions aside for
    /// a function; for a function, what its body names.
    refs: Vec<usize>,
    /// For a type: the types that it holds by value, so that a value of
    /// it would hold a value of them.
    held: Vec<usize>,
    /// The value that initializes a variable, with its file.
    value: Option<(u32, ExprId)>,
    is_alias: bool,
}

/// The strongly connected components of a graph, each after all that it
/// leads to. No recursion: a chain of declarations is as deep as it is
/// long.
fn components(count: usize, succ: impl Fn(usize) -> Vec<usize>) -> Vec<Vec<usize>> {
    const UNSEEN: usize = usize::MAX;
    let mut index = vec![UNSEEN; count];
    let mut low = vec![0; count];
    let mut on_stack = vec![false; count];
    let mut stack = Vec::new();
    let mut out = Vec::new();
    let mut next = 0;
    // Each frame: a node, its successors, and how many were followed.
    let mut frames: Vec<(usize, Vec<usize>, usize)> = Vec::new();
    for root in 0..count {
        if index[root] != UNSEEN {
            continue;
        }
        let mut entering = Some(root);
        loop {
            if let Some(node) = entering.take() {
                index[node] = next;
                low[node] = next;
                next += 1;
                stack.push(node);
                on_stack[node] = true;
                frames.push((node, succ(node), 0));
            }
            let Some((node, successors, followed)) = frames.last_mut() else {
                break;
            };
            let node = *node;
            if let Some(&to) = successors.get(*followed) {
                *followed += 1;
                if index[to] == UNSEEN {
                    entering = Some(to);
                } else if on_stack[to] {
                    low[node] = low[node].min(index[to]);
                }
                continue;
            }
            frames.pop();
            if let Some((parent, _, _)) = frames.last() {
                low[*parent] = low[*parent].min(low[node]);
            }
            if low[node] == index[node] {
                let mut component = Vec::new();
                while let Some(member) = stack.pop() {
                    on_stack[member] = false;
                    component.push(member);
                    if member == node {
                        break;
                    }
                }
                component.sort_unstable();
                out.push(component);
            }
        }
    }
    out
}

/// A path from `start` back to `start` through `within`, in the order of
/// the references; empty if there is none.
fn cycle_through(start: usize, within: &[usize], succ: impl Fn(usize) -> Vec<usize>) -> Vec<usize> {
    let inside: FxHashSet<usize> = within.iter().copied().collect();
    let mut seen = FxHashSet::default();
    let mut path: Vec<(usize, Vec<usize>, usize)> = vec![(start, succ(start), 0)];
    while let Some((_, successors, followed)) = path.last_mut() {
        let Some(&to) = successors.get(*followed) else {
            path.pop();
            continue;
        };
        *followed += 1;
        if to == start {
            return path.iter().map(|&(node, _, _)| node).collect();
        }
        if inside.contains(&to) && seen.insert(to) {
            path.push((to, succ(to), 0));
        }
    }
    Vec::new()
}

struct Orderer<'a> {
    package: &'a Package,
    names: &'a ThreadedRodeo,
    sink: &'a Sink,
    nodes: Vec<Node>,
    /// The object that each declaring identifier declares.
    by_span: FxHashMap<Span, usize>,
}

/// Finds the cycles among the package-level declarations of `package` and
/// the order in which its variables are initialized.
pub fn order(
    package: &Package,
    units: &[Unit<'_>],
    names: &ThreadedRodeo,
    sink: &Sink,
) -> Vec<Initializer> {
    let by_span = package.objects.iter().enumerate();
    let mut orderer = Orderer {
        package,
        names,
        sink,
        nodes: package.objects.iter().map(|_| Node::default()).collect(),
        by_span: by_span
            .map(|(index, object)| (object.span, index))
            .collect(),
    };
    for (file, unit) in units.iter().enumerate() {
        orderer.collect(file, unit);
    }
    orderer.recursive_types();
    orderer.init_order()
}

impl Orderer<'_> {
    fn name(&self, object: usize) -> &str {
        self.names.resolve(&self.package.objects[object].name)
    }

    fn span(&self, object: usize) -> Span {
        self.package.objects[object].span
    }

    fn error(&self, code: Code, span: Span, message: impl Into<String>) {
        self.sink.push(Diagnostic::new(code, span, message));
    }

    /// The object that the top-level identifier `name` declares; none if
    /// it is a second declaration of the name.
    fn declared(&self, name: Ident) -> Option<usize> {
        self.by_span.get(&name.span).copied()
    }

    fn collect(&mut self, file: usize, unit: &Unit<'_>) {
        let tree = unit.file;
        let bodies = &self.package.files[file].bodies;
        // The values that a constant without one repeats, per group.
        let mut repeated: Option<(Option<u32>, &[ExprId], usize)> = None;
        for (index, &decl) in tree.decls.iter().enumerate() {
            let node = &tree.decl_nodes[decl];
            let refs = Refs::new(tree, &bodies[index]);
            match &node.kind {
                DeclKind::Const { names, ty, values } | DeclKind::Var { names, ty, values } => {
                    let is_const = matches!(node.kind, DeclKind::Const { .. });
                    let mut values = values.as_slice();
                    // The names in a repeated value were resolved with the
                    // declaration that wrote it.
                    let mut value_refs = Refs::new(tree, &bodies[index]);
                    if is_const {
                        match repeated {
                            Some((group, last, origin))
                                if values.is_empty() && group == node.group =>
                            {
                                values = last;
                                value_refs = Refs::new(tree, &bodies[origin]);
                            }
                            _ => repeated = Some((node.group, values, index)),
                        }
                    }
                    let typed: Vec<usize> = ty.map_or(Vec::new(), |ty| refs.within(tree, ty));
                    for (position, &name) in names.iter().enumerate() {
                        self.only_a_func(name);
                        let Some(object) = self.declared(name) else {
                            continue;
                        };
                        // `a, b = f()` gives both the one value.
                        let value = match values.len() {
                            1 => values.first(),
                            _ => values.get(position),
                        };
                        let mut all = typed.clone();
                        if let Some(&value) = value {
                            all.extend(value_refs.within(tree, value));
                        }
                        let node = &mut self.nodes[object];
                        node.refs = all;
                        if !is_const {
                            node.value = value.map(|&value| (file as u32, value));
                        }
                    }
                }
                DeclKind::Type {
                    name, alias, ty, ..
                } => {
                    self.only_a_func(*name);
                    let Some(object) = self.declared(*name) else {
                        continue;
                    };
                    let mut held = Vec::new();
                    refs.held(tree, *ty, true, &mut held);
                    let node = &mut self.nodes[object];
                    node.is_alias = *alias;
                    node.refs = refs.within(tree, *ty);
                    node.held = held;
                }
                DeclKind::Func {
                    is_macro: false,
                    receiver: None,
                    name,
                    type_params,
                    ty,
                    body,
                } => {
                    self.entry_point(tree, *name, type_params, *ty, body.is_some());
                    if let Some(object) = self.declared(*name) {
                        self.nodes[object].refs = refs.all();
                    }
                }
                _ => {}
            }
        }
    }

    /// `init`, and `main` in package main, are functions and nothing else.
    fn is_entry_point(&self, name: Ident) -> bool {
        let text = self.names.resolve(&name.name);
        text == "init" || text == "main" && self.package.name == "main"
    }

    fn only_a_func(&self, name: Ident) {
        if self.is_entry_point(name) {
            let text = self.names.resolve(&name.name);
            let message = format!("cannot declare {text} - must be func");
            self.error(Code::InitSignature, name.span, message);
        }
    }

    /// `init` and `main` take nothing and return nothing.
    fn entry_point(
        &self,
        tree: &File,
        name: Ident,
        type_params: &[TypeParam],
        ty: ExprId,
        has_body: bool,
    ) {
        if !self.is_entry_point(name) {
            return;
        }
        let text = self.names.resolve(&name.name);
        if let Some(first) = type_params.first() {
            let message = format!("func {text} must have no type parameters");
            self.error(Code::InitTypeParams, first.name.span, message);
        }
        let plain = matches!(
            &tree.exprs[ty].kind,
            ExprKind::FuncType { params, results, .. } if params.is_empty() && results.is_empty()
        );
        if !plain {
            let message = format!("func {text} must have no arguments and no return values");
            self.error(Code::InitSignature, name.span, message);
        }
        if text == "init" && !has_body {
            self.error(Code::InitWithBody, name.span, "func init must have a body");
        }
    }

    /// `x refers to y` for each step of `cycle`, which leads back to its
    /// start.
    fn report_cycle(&self, cycle: &[usize], whole: Code, own: Code, what: [&str; 2]) {
        let Some(&start) = cycle.first() else { return };
        let name = self.name(start);
        if cycle.len() == 1 {
            let message = format!("{}: {name} refers to itself", what[1]);
            self.error(own, self.span(start), message);
            return;
        }
        let message = format!("{} {name}", what[0]);
        let mut diagnostic = Diagnostic::new(whole, self.span(start), message);
        for (step, &from) in cycle.iter().enumerate() {
            let to = cycle[(step + 1) % cycle.len()];
            let note = format!("{} refers to {}", self.name(from), self.name(to));
            diagnostic = diagnostic.with_note(Code::NoteRefersTo, Some(self.span(from)), note);
        }
        self.sink.push(diagnostic);
    }

    fn is_value(&self, object: usize) -> bool {
        matches!(
            self.package.objects[object].kind,
            ObjKind::Const | ObjKind::Var
        )
    }

    /// A type may not hold a value of itself, and aliases may not name
    /// each other in a circle.
    fn recursive_types(&self) {
        let count = self.nodes.len();
        let held = |node: usize| self.nodes[node].held.clone();
        let aliases = |node: usize| {
            let named = self.nodes[node].refs.iter().copied();
            let is_alias = |&to: &usize| self.nodes[to].is_alias;
            match self.nodes[node].is_alias {
                true => named.filter(is_alias).collect(),
                false => Vec::new(),
            }
        };
        let mut reported = FxHashSet::default();
        for component in components(count, held)
            .into_iter()
            .chain(components(count, aliases))
        {
            let start = component[0];
            let by_value = cycle_through(start, &component, held);
            let cycle = match by_value.is_empty() {
                true => cycle_through(start, &component, aliases),
                false => by_value,
            };
            if !cycle.is_empty() && reported.insert(start) {
                let what = ["invalid recursive type", "invalid recursive type"];
                self.report_cycle(
                    &cycle,
                    Code::InvalidRecursiveType,
                    Code::InvalidRecursiveTypeSelf,
                    what,
                );
            }
        }
    }

    /// Go's order of initialization: again and again the first variable in
    /// the source that needs no variable that is not yet initialized.
    /// What a variable needs includes what the functions it names need.
    /// Constants come first.
    fn init_order(&self) -> Vec<Initializer> {
        let relevant = |object: usize| {
            self.is_value(object) || matches!(self.package.objects[object].kind, ObjKind::Func)
        };
        let refs = |node: usize| {
            let named = self.nodes[node].refs.iter().copied();
            match relevant(node) {
                true => named.filter(|&to| relevant(to)).collect::<Vec<_>>(),
                false => Vec::new(),
            }
        };
        // Functions that call each other are one node. Among the nodes
        // there is then no circle, but for the errors.
        let groups = components(self.nodes.len(), refs);
        let mut group_of = vec![0; self.nodes.len()];
        for (group, members) in groups.iter().enumerate() {
            for &member in members {
                group_of[member] = group;
            }
        }
        let mut waiting = vec![0usize; groups.len()];
        let mut needed_by: Vec<Vec<usize>> = vec![Vec::new(); groups.len()];
        for (group, members) in groups.iter().enumerate() {
            let mut needs: Vec<usize> = members
                .iter()
                .flat_map(|&m| refs(m))
                .map(|to| group_of[to])
                .collect();
            needs.sort_unstable();
            needs.dedup();
            for need in needs.into_iter().filter(|&need| need != group) {
                waiting[group] += 1;
                needed_by[need].push(group);
            }
        }

        let key = |group: usize| {
            let first = groups[group][0];
            let is_const = matches!(self.package.objects[first].kind, ObjKind::Const);
            Reverse((!is_const, first))
        };
        let has_value = |group: usize| groups[group].iter().any(|&member| self.is_value(member));
        let mut ready = BinaryHeap::new();
        let mut functions = Vec::new();
        let release =
            |group: usize, ready: &mut BinaryHeap<_>, functions: &mut Vec<usize>| match has_value(
                group,
            ) {
                true => ready.push((key(group), group)),
                false => functions.push(group),
            };
        for (group, &needs) in waiting.iter().enumerate() {
            if needs == 0 {
                release(group, &mut ready, &mut functions);
            }
        }
        // The variables that each value initializes: `a, b = f()` is one
        // step for both.
        let mut initialized: FxHashMap<(u32, ExprId), Vec<ObjId>> = FxHashMap::default();
        for (object, node) in self.nodes.iter().enumerate() {
            if let Some(value) = node.value {
                initialized
                    .entry(value)
                    .or_default()
                    .push(ObjId(object as u32));
            }
        }
        let mut order = Vec::new();
        loop {
            // A function is no step of its own: it is done when what it
            // needs is.
            let group = match functions.pop() {
                Some(group) => group,
                None => match ready.pop() {
                    Some((_, group)) => group,
                    None => break,
                },
            };
            let members = &groups[group];
            let own_cycle = members.len() > 1 || refs(members[0]).contains(&members[0]);
            if own_cycle && has_value(group) {
                let start = members
                    .iter()
                    .copied()
                    .find(|&m| self.is_value(m))
                    .unwrap_or(members[0]);
                let cycle = cycle_through(start, members, refs);
                let what = ["initialization cycle for", "initialization cycle"];
                self.report_cycle(&cycle, Code::InitCycle, Code::InitCycleSelf, what);
            }
            for &member in members {
                if let Some((file, value)) = self.nodes[member].value
                    && let Some(lhs) = initialized.remove(&(file, value))
                {
                    order.push(Initializer { lhs, file, value });
                }
            }
            for &dependent in &needed_by[group] {
                waiting[dependent] -= 1;
                if waiting[dependent] == 0 {
                    release(dependent, &mut ready, &mut functions);
                }
            }
        }
        order
    }
}

/// The package-level objects that one top-level declaration names, by the
/// place of the name.
struct Refs<'a> {
    body: &'a Body,
    /// The start of each name that denotes an object of the package, and
    /// the object, in the order of the source.
    sorted: Vec<(u32, usize)>,
}

impl<'a> Refs<'a> {
    fn new(tree: &File, body: &'a Body) -> Self {
        let object = |(&expr, denotes): (&ExprId, &Ref)| match *denotes {
            Ref::Object(ObjId(object)) => Some((tree.exprs[expr].span.lo, object as usize)),
            _ => None,
        };
        // A key of a composite literal may be a field; whether it names an
        // object is known only with the type of the literal.
        let mut sorted: Vec<(u32, usize)> = body.uses.iter().filter_map(object).collect();
        sorted.sort_unstable();
        Self { body, sorted }
    }

    fn all(&self) -> Vec<usize> {
        self.sorted.iter().map(|&(_, object)| object).collect()
    }

    /// The objects named inside the expression `expr`.
    fn within(&self, tree: &File, expr: ExprId) -> Vec<usize> {
        let span = tree.exprs[expr].span;
        let from = self.sorted.partition_point(|&(lo, _)| lo < span.lo);
        let named = self.sorted[from..]
            .iter()
            .take_while(|&&(lo, _)| lo < span.hi);
        named.map(|&(_, object)| object).collect()
    }

    /// The types that a value of the type `expr` holds a value of. Behind
    /// a pointer, in a slice, a map, a channel, a func or an interface,
    /// nothing is held.
    fn held(&self, tree: &File, expr: ExprId, by_value: bool, out: &mut Vec<usize>) {
        if !by_value {
            return;
        }
        match &tree.exprs[expr].kind {
            ExprKind::Name(_) => {
                if let Some(&Ref::Object(ObjId(object))) = self.body.uses.get(&expr) {
                    out.push(object as usize);
                }
            }
            ExprKind::Paren(x) => self.held(tree, *x, true, out),
            ExprKind::ArrayType { elem, .. } => self.held(tree, *elem, true, out),
            ExprKind::StructType { fields } => {
                for field in fields {
                    self.held(tree, field.ty, true, out);
                }
            }
            ExprKind::Index { x, indices } => match self.body.uses.get(x) {
                // An option holds its value in place.
                Some(Ref::Universe(Predeclared::Option)) => {
                    for &index in indices {
                        self.held(tree, index, true, out);
                    }
                }
                Some(Ref::Universe(_)) => {}
                // A generic type: what it holds of its arguments is known
                // only when it is instantiated.
                _ => self.held(tree, *x, true, out),
            },
            _ => {}
        }
    }
}
