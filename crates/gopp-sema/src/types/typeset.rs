//! The types that a constraint allows: its type set.
//!
//! An interface allows the types that have its methods; the type terms of
//! a constraint (`~int | string`), `comparable` and the constraints of
//! Go++ narrow that. The set of an interface is built once, after the sets
//! of the interfaces that it embeds, and shared with those that embed it.

use std::sync::atomic::Ordering;

use rustc_hash::FxHashSet;

use super::{Constraint, Marker, MethodSet, Term, Type, TypeId, Types};

/// What a constraint asks of a type.
#[derive(Clone, Debug, Default)]
pub struct TypeSet {
    /// The methods that the type has.
    pub methods: MethodSet,
    /// The type terms, one of which the type is in; `None` if any type
    /// may be. No term holds another.
    pub terms: Option<Vec<Term>>,
    /// The values of the type compare. Never with `terms`: those are the
    /// terms whose values compare then.
    pub comparable: bool,
    /// The constraints of Go++ that the type meets, each once.
    pub markers: Vec<Marker>,
}

impl TypeSet {
    /// Whether no type is allowed.
    pub fn is_empty(&self) -> bool {
        self.terms.as_ref().is_some_and(|terms| terms.is_empty())
    }

    /// What makes this the set of a constraint; `None` if methods alone
    /// say what is in it.
    pub fn constraint(&self) -> Option<Constraint> {
        match (&self.terms, self.comparable, self.markers.is_empty()) {
            (Some(_), ..) => Some(Constraint::Terms),
            (None, true, _) => Some(Constraint::Comparable),
            (None, false, false) => Some(Constraint::Marker),
            (None, false, true) => None,
        }
    }
}

/// What is known of the set of an interface while sets are built.
enum Dep {
    Set(TypeSet),
    /// A declared type in it is not checked yet.
    Unknown,
    /// It is being built: an interface that embeds itself, which is an
    /// error.
    Entered,
}

impl Types {
    /// Whether no type is in both terms.
    pub fn disjoint(&self, x: Term, y: Term) -> bool {
        let under = |term: Term, other: Term| match other.tilde {
            true => self.underlying(term.ty),
            false => term.ty,
        };
        !self.identical(under(x, y), under(y, x))
    }

    /// Whether the type is in the term.
    pub fn in_term(&self, term: Term, ty: TypeId) -> bool {
        match term.tilde {
            true => self.identical(term.ty, self.underlying(ty)),
            false => self.identical(term.ty, ty),
        }
    }

    /// The terms without those that another term holds.
    fn normalized(&self, terms: Vec<Term>) -> Vec<Term> {
        let mut used = vec![false; terms.len()];
        let mut out = Vec::new();
        for i in 0..terms.len() {
            if used[i] {
                continue;
            }
            let mut kept = terms[i];
            for j in i + 1..terms.len() {
                if !used[j] && !self.disjoint(kept, terms[j]) {
                    // Of two terms that meet, the one with `~` holds the
                    // other.
                    if !kept.tilde && terms[j].tilde {
                        kept = terms[j];
                    }
                    used[j] = true;
                }
            }
            out.push(kept);
        }
        out
    }

    /// The types that are in both lists of terms; `None` is every type.
    fn both(&self, a: Option<Vec<Term>>, b: Option<Vec<Term>>) -> Option<Vec<Term>> {
        let (a, b) = match (a, b) {
            (None, other) | (other, None) => return other,
            (Some(a), Some(b)) => (a, b),
        };
        let mut out = Vec::new();
        for &x in &a {
            for &y in &b {
                if !self.disjoint(x, y) {
                    out.push(if !x.tilde || y.tilde { x } else { y });
                }
            }
        }
        Some(self.normalized(out))
    }

    /// The set of the types that are in both sets.
    fn meet(&self, a: TypeSet, b: &TypeSet, steps: &mut u64) -> TypeSet {
        let mut terms = self.both(a.terms, b.terms.clone());
        let mut comparable = a.comparable || b.comparable;
        if let (true, Some(listed)) = (comparable, &mut terms) {
            listed.retain(|term| self.strictly_comparable(term.ty));
            comparable = false;
        }
        let mut markers = a.markers;
        for &marker in &b.markers {
            if !markers.contains(&marker) {
                markers.push(marker);
            }
        }
        TypeSet {
            methods: a.methods.union(&b.methods, &mut |_, _| {}, steps),
            terms,
            comparable,
            markers,
        }
    }

    /// The interfaces whose sets the set of the element `elem` of an
    /// interface is made of: itself, or the interfaces among the terms of
    /// a union.
    fn interfaces_in(&self, elem: TypeId) -> Vec<TypeId> {
        let under = self.underlying(elem);
        match &*self.get(under) {
            Type::Interface { .. } => vec![under],
            Type::Union(terms) => {
                let unders = terms.iter().map(|term| self.underlying(term.ty));
                unders
                    .filter(|&under| matches!(&*self.get(under), Type::Interface { .. }))
                    .collect()
            }
            _ => Vec::new(),
        }
    }

    /// The set of one element of an interface: an embedded interface, a
    /// union, a type, or a predeclared constraint. `set_of` has what is
    /// known of the interfaces in it.
    fn element(&self, elem: TypeId, set_of: &impl Fn(TypeId) -> Dep) -> Dep {
        let under = self.underlying(elem);
        let terms = match &*self.get(under) {
            Type::Interface { .. } => return set_of(under),
            Type::Invalid | Type::Param(_) => return Dep::Unknown,
            Type::Marker(Marker::Comparable) => {
                return Dep::Set(TypeSet {
                    comparable: true,
                    ..TypeSet::default()
                });
            }
            Type::Marker(marker) => {
                return Dep::Set(TypeSet {
                    markers: vec![*marker],
                    ..TypeSet::default()
                });
            }
            Type::Union(terms) => {
                let mut all = Vec::new();
                for term in terms.iter() {
                    let under = self.underlying(term.ty);
                    match &*self.get(under) {
                        Type::Invalid => return Dep::Unknown,
                        Type::Interface { .. } => match set_of(under) {
                            Dep::Set(set) => match set.terms {
                                Some(terms) => all.extend(terms),
                                // One term allows every type.
                                None => return Dep::Set(TypeSet::default()),
                            },
                            Dep::Entered => {}
                            Dep::Unknown => return Dep::Unknown,
                        },
                        // No type has an underlying type that is a
                        // declared type.
                        _ if term.tilde && under != term.ty => {}
                        _ => all.push(*term),
                    }
                }
                self.normalized(all)
            }
            _ => vec![Term {
                tilde: false,
                ty: elem,
            }],
        };
        Dep::Set(TypeSet {
            terms: Some(terms),
            ..TypeSet::default()
        })
    }

    /// The set of the interface type `start`, which is no declared type.
    /// `None` while a declared type in it is not checked yet.
    fn interface_set(&self, start: TypeId) -> Option<TypeSet> {
        let cached = |ty: TypeId| self.read(|inner| inner.type_sets.get(&ty).cloned());
        if let Some(set) = cached(start) {
            return Some(set);
        }
        // The interfaces whose sets are being built, each after those it
        // is made of: a chain of embedded interfaces is as long as a
        // program makes it.
        let mut work = vec![(start, false)];
        let mut entered: FxHashSet<TypeId> = [start].into_iter().collect();
        let mut unknown: FxHashSet<TypeId> = FxHashSet::default();
        let mut steps = 0;
        while let Some((current, parts_done)) = work.pop() {
            let ty = self.get(current);
            let Type::Interface { methods, embedded } = &*ty else {
                continue;
            };
            if !parts_done {
                work.push((current, true));
                for &elem in embedded.iter() {
                    for part in self.interfaces_in(elem) {
                        if cached(part).is_none() && entered.insert(part) {
                            work.push((part, false));
                        }
                    }
                }
                continue;
            }
            let set_of = |ty: TypeId| match cached(ty) {
                Some(set) => Dep::Set(set),
                None if unknown.contains(&ty) => Dep::Unknown,
                None => Dep::Entered,
            };
            let mut set = TypeSet::default();
            for method in methods.iter() {
                set.methods = set.methods.with(method.clone(), &mut |_, _| {}, &mut steps);
            }
            let mut complete = true;
            for &elem in embedded.iter() {
                match self.element(elem, &set_of) {
                    Dep::Set(part) => set = self.meet(set, &part, &mut steps),
                    Dep::Unknown => complete = false,
                    Dep::Entered => {}
                }
            }
            match complete {
                true => drop(self.write(|inner| inner.type_sets.insert(current, set))),
                false => drop(unknown.insert(current)),
            }
        }
        self.steps.fetch_add(steps, Ordering::Relaxed);
        cached(start)
    }

    /// The type set of the constraint `constraint`: an interface, or what
    /// stands for one after a type parameter (`~int | string`, `int`).
    /// `None` while a declared type in it is not checked yet.
    pub fn type_set(&self, constraint: TypeId) -> Option<TypeSet> {
        for part in self.interfaces_in(constraint) {
            self.interface_set(part)?;
        }
        let set_of = |ty: TypeId| match self.interface_set(ty) {
            Some(set) => Dep::Set(set),
            None => Dep::Unknown,
        };
        match self.element(constraint, &set_of) {
            Dep::Set(set) => Some(set),
            Dep::Unknown | Dep::Entered => None,
        }
    }

    /// The type set of the type parameter `param`; `None` also while its
    /// constraint is not known.
    pub fn param_set(&self, param: TypeId) -> Option<TypeSet> {
        self.type_set(self.constraint(param)?)
    }

    /// The methods of an interface with those of the interfaces it
    /// embeds, by name; of two methods of one name, the one that the
    /// interface writes itself. `None` if `id` is no interface, or is a
    /// constraint, or embeds a type that is not checked yet.
    pub fn method_set(&self, id: TypeId) -> Option<MethodSet> {
        let set = self.interface_set(self.underlying(id))?;
        set.constraint().is_none().then_some(set.methods)
    }

    /// What makes the interface `id` a constraint, which only a type
    /// parameter can be held to; `None` if it is none.
    pub fn constraint_kind(&self, id: TypeId) -> Option<Constraint> {
        self.interface_set(self.underlying(id))?.constraint()
    }

    /// Whether the type is an interface that only a type parameter can be
    /// held to, or one that embeds a type that is not checked yet.
    pub fn is_constraint(&self, id: TypeId) -> bool {
        self.is_interface(id) && self.method_set(id).is_none()
    }
}
