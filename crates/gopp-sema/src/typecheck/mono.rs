//! Instantiation cycles, after `go/types` (mono.go): a generic type or
//! function that instantiates itself, directly or through others, with a
//! type argument built around its own type parameter would need
//! instances without end (GEN-5).

use super::*;

struct Vertex {
    /// The weight of the heaviest known way to the vertex, the edge that
    /// ends it, and how many edges it has.
    weight: i32,
    pre: usize,
    len: usize,
    param: TypeId,
}

struct Edge {
    dst: usize,
    src: usize,
    weight: i32,
    span: Span,
    typ: TypeId,
}

/// The type parameters of the package, with an edge for each type
/// argument that is written with one: from the parameter in the argument
/// to the parameter that gets the argument, of weight 1 if the argument
/// is more than the parameter itself.
#[derive(Default)]
pub(super) struct Mono {
    vertices: Vec<Vertex>,
    edges: Vec<Edge>,
    index: FxHashMap<TypeId, usize>,
    /// For a type parameter that a receiver names, that of the type.
    canon: FxHashMap<TypeId, TypeId>,
    /// The declared type that each type parameter belongs to.
    owners: FxHashMap<TypeId, crate::types::NamedId>,
    /// By the name of a method with type parameters of its own: what it
    /// names the type parameters of its receiver, and its own.
    methods: FxHashMap<Span, (Vec<TypeId>, Vec<TypeId>)>,
    /// The vertices of the cycle that was found, the last one first.
    found: Vec<usize>,
}

impl<'a> Checker<'a> {
    /// The type parameters `params` are those of the declared type
    /// `named`.
    pub(super) fn mono_owner(&mut self, params: &[TypeId], named: crate::types::NamedId) {
        for &param in params {
            self.mono.owners.insert(param, named);
        }
    }

    /// The receiver of a method names the type parameters of `named`
    /// `params`: each is the parameter of the type at its place.
    pub(super) fn mono_canon(&mut self, params: &[TypeId], named: crate::types::NamedId) {
        let of_type = self.types.type_params(named);
        for (&param, &canon) in params.iter().zip(&of_type) {
            self.mono.canon.insert(param, canon);
        }
    }

    /// The type parameters `params` stand for `canon`, each at its place.
    pub(super) fn mono_same(&mut self, params: &[TypeId], canon: &[TypeId]) {
        for (&param, &canon) in params.iter().zip(canon) {
            self.mono.canon.insert(param, canon);
        }
    }

    /// The method named at `name` has type parameters of its own `own`,
    /// and names those of its receiver `receiver`. Its instances are made
    /// where it is called, so the type parameters of its receiver are
    /// its own too, not those of the type.
    pub(super) fn mono_method(&mut self, name: Span, receiver: &[TypeId], own: &[TypeId]) {
        match self.mono.methods.get(&name).cloned() {
            Some((of_receiver, of_method)) => {
                self.mono_same(receiver, &of_receiver);
                self.mono_same(own, &of_method);
            }
            None => {
                let declared = (receiver.to_vec(), own.to_vec());
                self.mono.methods.insert(name, declared);
            }
        }
    }

    fn mono_vertex(&mut self, param: TypeId) -> std::option::Option<usize> {
        let param = self.types.param_origin(param);
        let param = self.mono.canon.get(&param).copied().unwrap_or(param);
        // A type parameter of another package is in no cycle of this one.
        if !self.param_spans.contains_key(&param) {
            return None;
        }
        if let Some(&at) = self.mono.index.get(&param) {
            return Some(at);
        }
        let at = self.mono.vertices.len();
        self.mono.vertices.push(Vertex {
            weight: 0,
            pre: 0,
            len: 0,
            param,
        });
        self.mono.index.insert(param, at);
        Some(at)
    }

    /// The generic type or function with the type parameters `params` is
    /// given the type arguments `args`, each written at its span.
    pub(super) fn mono_record(
        &mut self,
        params: &[TypeId],
        args: &[TypeId],
        spans: &[Span],
        at: Span,
    ) {
        // A method with type parameters of its own is instantiated for
        // the type arguments of its receiver too.
        let receiver = params
            .first()
            .and_then(|&first| self.types.method_receiver(first));
        if let Some((of_receiver, given)) = receiver {
            self.mono_record(&of_receiver, &given, &[], at);
        }
        for (position, (&param, &arg)) in params.iter().zip(args).enumerate() {
            let Some(dst) = self.mono_vertex(param) else {
                continue;
            };
            let span = spans.get(position).copied().unwrap_or(at);
            let mut rest = vec![arg];
            let mut seen = FxHashSet::default();
            while let Some(next) = rest.pop() {
                self.steps += 1;
                if !seen.insert(next) {
                    continue;
                }
                let node = self.types.get(self.types.unalias(next));
                match &*node {
                    Type::Param(_) => {
                        if let Some(src) = self.mono_vertex(next) {
                            let weight = i32::from(next != arg);
                            self.mono.edges.push(Edge {
                                dst,
                                src,
                                weight,
                                span,
                                typ: arg,
                            });
                        }
                    }
                    other => other.each_part(&mut |part| rest.push(part)),
                }
            }
        }
    }

    /// Marks the types whose parameters are on a cycle of positive
    /// weight, so that no walk follows their instances.
    pub(super) fn monomorph(&mut self) {
        self.mono_cyclic();
    }

    /// Whether an edge of positive weight joins two type parameters that
    /// reach each other: the weights are never negative, so only then is
    /// there a cycle of positive weight. The parameters that reach each
    /// other are found in one walk (Tarjan), and the types of those on a
    /// cycle are marked.
    fn mono_cyclic(&mut self) -> bool {
        const UNSEEN: usize = usize::MAX;
        let count = self.mono.vertices.len();
        let mut out = vec![Vec::new(); count];
        for edge in &self.mono.edges {
            out[edge.src].push(edge.dst);
        }
        let mut order = vec![UNSEEN; count];
        let mut low = vec![0; count];
        let mut group = vec![UNSEEN; count];
        let mut open = Vec::new();
        let mut groups = 0;
        let mut next = 0;
        for root in 0..count {
            if order[root] != UNSEEN {
                continue;
            }
            let mut work = vec![(root, 0)];
            while let Some(top) = work.last_mut() {
                self.steps += 1;
                let (v, at) = *top;
                if order[v] == UNSEEN {
                    order[v] = next;
                    low[v] = next;
                    next += 1;
                    open.push(v);
                }
                if let Some(&to) = out[v].get(at) {
                    top.1 += 1;
                    if order[to] == UNSEEN {
                        work.push((to, 0));
                    } else if group[to] == UNSEEN {
                        low[v] = low[v].min(order[to]);
                    }
                    continue;
                }
                work.pop();
                if let Some(&(from, _)) = work.last() {
                    low[from] = low[from].min(low[v]);
                }
                if low[v] == order[v] {
                    while let Some(member) = open.pop() {
                        group[member] = groups;
                        if member == v {
                            break;
                        }
                    }
                    groups += 1;
                }
            }
        }
        let mut cyclic = vec![false; groups];
        for edge in &self.mono.edges {
            if edge.weight > 0 && group[edge.src] == group[edge.dst] {
                cyclic[group[edge.src]] = true;
            }
        }
        // Every type on a cycle is marked, not only those on the cycle
        // that is reported.
        for (vertex, &of) in self.mono.vertices.iter().zip(&group) {
            if let (true, Some(&named)) = (cyclic[of], self.mono.owners.get(&vertex.param)) {
                self.types.set_expanding(named);
            }
        }
        cyclic.contains(&true)
    }

    /// Finds the cycle that Go reports (Bellman-Ford over the edges in
    /// the order they were recorded). Only a graph that has a cycle is
    /// searched: the search takes the vertices times the edges.
    fn mono_search(&mut self) {
        for vertex in &mut self.mono.vertices {
            vertex.weight = 0;
            vertex.len = 0;
        }
        let count = self.mono.vertices.len();
        let mut again = true;
        while again {
            again = false;
            for at in 0..self.mono.edges.len() {
                self.steps += 1;
                let (dst, src, weight) = {
                    let edge = &self.mono.edges[at];
                    (edge.dst, edge.src, edge.weight)
                };
                let through = self.mono.vertices[src].weight + weight;
                if through <= self.mono.vertices[dst].weight {
                    continue;
                }
                let len = self.mono.vertices[src].len + 1;
                let vertex = &mut self.mono.vertices[dst];
                vertex.pre = at;
                vertex.len = len;
                if len == count {
                    return self.mono_cycle(dst);
                }
                vertex.weight = through;
                again = true;
            }
        }
    }

    /// The cycle that the way to the vertex `v` ends in.
    fn mono_cycle(&mut self, mut v: usize) {
        let mut stack = Vec::new();
        let mut seen = vec![false; self.mono.vertices.len()];
        while !seen[v] {
            stack.push(v);
            seen[v] = true;
            v = self.mono.edges[self.mono.vertices[v].pre].src;
        }
        let from = stack.iter().position(|&on| on == v).unwrap_or(0);
        self.mono.found = stack.split_off(from);
    }

    /// Reports the cycle, as Go does: at the first type parameter, with
    /// what each parameter on it is instantiated as. Only a package with
    /// no other error is told.
    pub(super) fn mono_report(&mut self) {
        if !self.sink.is_empty() || !self.mono_cyclic() {
            return;
        }
        // With the instantiations of the bodies, as Go searches.
        self.mono_search();
        let first = self.mono.vertices[self.mono.found[0]].param;
        let Some(&span) = self.param_spans.get(&first) else {
            return;
        };
        let mut diagnostic =
            Diagnostic::new(Code::InstantiationCycle, span, "instantiation cycle:");
        for &on in &self.mono.found {
            let edge = &self.mono.edges[self.mono.vertices[on].pre];
            let param = self.mono.vertices[edge.dst].param;
            let note = format!(
                "{} instantiated as {}",
                self.type_string(param),
                self.type_string(edge.typ)
            );
            diagnostic = diagnostic.with_note(Code::NoteRefersTo, Some(edge.span), note);
        }
        self.sink.push(diagnostic);
    }
}
