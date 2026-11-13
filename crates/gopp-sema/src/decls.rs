//! The order in which the variables of a package are initialized, and the
//! cycles among their initial values, which are errors.
//!
//! What a declaration needs is what its checked expressions use as values.

use std::cmp::Reverse;
use std::collections::BinaryHeap;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::Span;
use gopp_syntax::ast::*;
use lasso::ThreadedRodeo;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::object::{ObjId, ObjKind};
use crate::package::Package;
use crate::resolve::Unit;

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
    /// The constants, variables and functions that it uses as values; for
    /// a function, those that its body uses.
    refs: Vec<usize>,
    /// The value that initializes a variable, with its file.
    value: Option<(u32, ExprId)>,
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
    /// The objects of the package, then its methods.
    nodes: Vec<Node>,
    /// The names of the methods, in the order of their nodes.
    methods: &'a [Ident],
    /// The object that each declaring identifier declares.
    by_span: FxHashMap<Span, usize>,
}

/// Finds the cycles among the package-level declarations of `package` and
/// the order in which its variables are initialized. `needs` has, for each
/// object and then for each of `methods`, the objects and the methods that
/// its declaration uses as values.
pub fn order(
    package: &Package,
    units: &[Unit<'_>],
    needs: &[Vec<usize>],
    methods: &[Ident],
    names: &ThreadedRodeo,
    sink: &Sink,
) -> Vec<Initializer> {
    let by_span = package.objects.iter().enumerate();
    let node = |refs: &Vec<usize>| Node {
        refs: refs.clone(),
        ..Node::default()
    };
    let mut orderer = Orderer {
        package,
        names,
        sink,
        nodes: needs.iter().map(node).collect(),
        methods,
        by_span: by_span
            .map(|(index, object)| (object.span, index))
            .collect(),
    };
    for (file, unit) in units.iter().enumerate() {
        orderer.collect(file, unit);
    }
    orderer.init_order()
}

impl Orderer<'_> {
    /// The method that is the node `node`, if it is one.
    fn method(&self, node: usize) -> Option<Ident> {
        let at = node.checked_sub(self.package.objects.len())?;
        self.methods.get(at).copied()
    }

    fn name(&self, node: usize) -> &str {
        match self.method(node) {
            Some(method) => self.names.resolve(&method.name),
            None => self.names.resolve(&self.package.objects[node].name),
        }
    }

    fn span(&self, node: usize) -> Span {
        match self.method(node) {
            Some(method) => method.span,
            None => self.package.objects[node].span,
        }
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
        for &decl in &tree.decls {
            match &tree.decl_nodes[decl].kind {
                DeclKind::Var { names, values, .. } => {
                    for (position, &name) in names.iter().enumerate() {
                        let Some(object) = self.declared(name) else {
                            continue;
                        };
                        // `a, b = f()` gives both the one value.
                        let value = match values.len() {
                            1 => values.first(),
                            _ => values.get(position),
                        };
                        self.nodes[object].value = value.map(|&value| (file as u32, value));
                    }
                }
                DeclKind::Func {
                    is_macro: false,
                    receiver: None,
                    name,
                    type_params,
                    ty,
                    body,
                } => self.entry_point(tree, *name, type_params, *ty, body.is_some()),
                _ => {}
            }
        }
    }

    /// `init`, and `main` in package main, are functions and nothing else.
    fn is_entry_point(&self, name: Ident) -> bool {
        let text = self.names.resolve(&name.name);
        text == "init" || text == "main" && self.package.name == "main"
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

    fn is_value(&self, node: usize) -> bool {
        let kind = self.package.objects.get(node).map(|object| &object.kind);
        matches!(kind, Some(ObjKind::Const | ObjKind::Var))
    }

    /// Go's order of initialization: again and again the first variable in
    /// the source that needs no variable that is not yet initialized.
    /// What a variable needs includes what the functions it names need.
    /// Constants come first.
    fn init_order(&self) -> Vec<Initializer> {
        // A method is what a function is.
        let relevant = |node: usize| match self.package.objects.get(node) {
            Some(object) => self.is_value(node) || matches!(object.kind, ObjKind::Func),
            None => true,
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
