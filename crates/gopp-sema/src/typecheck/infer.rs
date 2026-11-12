//! Type inference: the type arguments of a generic function that a call
//! does not write, after `go/types` (infer.go, unify.go), with Go's
//! messages.

use super::constraints::Instance;
use super::*;
use crate::types::MethodSet;

/// How deep unification goes into types before it gives up, as in Go.
const DEPTH_LIMIT: u32 = 50;

/// How alike two types must be to unify.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct Alike {
    /// As for an assignment: a declared type and what it is declared as
    /// unify at the top; inside them the types are identical.
    assign: bool,
    /// The types are identical but for the type parameters.
    exact: bool,
}

const INEXACT: Alike = Alike {
    assign: false,
    exact: false,
};
const ASSIGN: Alike = Alike {
    assign: true,
    exact: false,
};
const EXACT: Alike = Alike {
    assign: false,
    exact: true,
};

/// A generic function as it is called: its type, its type parameters, and
/// the type arguments that the call writes, which are the first ones.
pub(super) struct Generic {
    pub sig: TypeId,
    pub params: Vec<TypeId>,
    pub written: Vec<(TypeId, Span)>,
}

/// How type arguments are looked for.
#[derive(Clone, Copy, Default)]
struct How<'d> {
    /// Nothing is reported.
    quiet: bool,
    /// The arguments are read as what they point to.
    read: bool,
    /// The generic function is a value that is given to a place: what
    /// the place is, for the messages.
    place: Option<Place<'d>>,
}

/// What a generic function that is used as a value is given to.
#[derive(Clone, Copy)]
pub(super) enum Place<'d> {
    /// A variable or a result, as a message names it.
    Named(&'d str),
    /// A parameter: the function as a message shows it, which is with
    /// its type parameters.
    Parameter(&'d str),
}

/// What is found for type parameters while types are unified.
#[derive(Clone)]
struct Unifier<'t> {
    types: &'t Types,
    names: &'t ThreadedRodeo,
    params: Vec<TypeId>,
    /// Which of the type parameters each is.
    index: FxHashMap<TypeId, usize>,
    /// For each type parameter, where its type is kept: parameters that
    /// are one type share the place.
    place: Vec<usize>,
    found: Vec<Option<TypeId>>,
    depth: u32,
    /// The pairs of types, neither of them a type parameter that is
    /// looked for, that unified, with how alike they had to be: a type
    /// that stands in several places is unified once.
    unified: FxHashSet<(TypeId, TypeId, Alike)>,
    /// How many pairs of types were looked at.
    steps: u64,
}

impl<'t> Unifier<'t> {
    fn new(types: &'t Types, names: &'t ThreadedRodeo, params: &[TypeId]) -> Self {
        Self {
            types,
            names,
            params: params.to_vec(),
            index: params
                .iter()
                .enumerate()
                .map(|(at, &ty)| (ty, at))
                .collect(),
            place: (0..params.len()).collect(),
            found: vec![None; params.len()],
            depth: 0,
            unified: FxHashSet::default(),
            steps: 0,
        }
    }

    /// Which of the type parameters that are looked for `ty` is.
    fn index(&self, ty: TypeId) -> Option<usize> {
        self.index.get(&ty).copied()
    }

    fn at(&self, param: usize) -> Option<TypeId> {
        self.found[self.place[param]]
    }

    fn set(&mut self, param: usize, ty: TypeId) {
        self.found[self.place[param]] = Some(ty);
    }

    fn unknowns(&self) -> usize {
        (0..self.params.len())
            .filter(|&param| self.at(param).is_none())
            .count()
    }

    fn inferred(&self) -> Vec<Option<TypeId>> {
        (0..self.params.len()).map(|param| self.at(param)).collect()
    }

    /// Makes two type parameters one type. False if both have a type.
    fn join(&mut self, x: usize, y: usize) -> bool {
        let (px, py) = (self.place[x], self.place[y]);
        let (from, to) = match (px == py, self.found[px], self.found[py]) {
            (true, ..) => return true,
            (false, Some(_), Some(_)) => return false,
            (false, Some(_), None) => (py, px),
            _ => (px, py),
        };
        for place in &mut self.place {
            if *place == from {
                *place = to;
            }
        }
        true
    }

    fn is_named(&self, ty: TypeId) -> bool {
        matches!(&*self.types.get(ty), Type::Named { .. })
    }

    fn is_param(&self, ty: TypeId) -> bool {
        matches!(&*self.types.get(ty), Type::Param(_))
    }

    /// Whether the type is an interface and no type parameter.
    fn is_interface(&self, ty: TypeId) -> bool {
        !self.is_param(ty) && self.types.is_interface(ty)
    }

    fn unify(&mut self, x: TypeId, y: TypeId, alike: Alike) -> bool {
        self.nify(x, y, alike, &mut Vec::new())
    }

    /// Whether `x` and `y` are one type for some types of the type
    /// parameters, which are noted. `pairs` has the interfaces that are
    /// being compared: a pair that is met again unifies.
    fn nify(
        &mut self,
        x: TypeId,
        y: TypeId,
        alike: Alike,
        pairs: &mut Vec<(TypeId, TypeId)>,
    ) -> bool {
        self.steps += 1;
        if x == y {
            return true;
        }
        if self.unified.contains(&(x, y, alike)) {
            return true;
        }
        if self.depth == DEPTH_LIMIT {
            return false;
        }
        self.depth += 1;
        let unified = self.nify_unequal(x, y, alike, pairs);
        self.depth -= 1;
        if unified && self.index(x).is_none() && self.index(y).is_none() {
            self.unified.insert((x, y, alike));
        }
        unified
    }

    fn nify_unequal(
        &mut self,
        mut x: TypeId,
        mut y: TypeId,
        alike: Alike,
        pairs: &mut Vec<(TypeId, TypeId)>,
    ) -> bool {
        let types = self.types;
        // A declared type goes to `y`, a type parameter that is looked
        // for to `x`.
        if self.is_named(x) || self.index(y).is_some() {
            (x, y) = (y, x);
        }
        // A declared type unifies with a type that is written out
        // through what it is declared as, unless that is an interface.
        let written_out = !self.is_named(x) && !self.is_param(x);
        if !alike.exact && self.is_named(y) && written_out && !types.is_interface(x) {
            y = types.underlying(y);
            if x == y {
                return true;
            }
        }
        match (self.index(x), self.index(y)) {
            (Some(px), Some(py)) => {
                if self.join(px, py) {
                    return true;
                }
                return match (self.at(px), self.at(py)) {
                    (Some(a), Some(b)) => self.nify(a, b, alike, pairs),
                    _ => false,
                };
            }
            (Some(px), None) => {
                let Some(known) = self.at(px) else {
                    self.set(px, y);
                    return true;
                };
                if !self.nify(known, y, alike, pairs) {
                    return false;
                }
                let (xi, yi) = (self.is_interface(known), self.is_interface(y));
                let (xn, yn) = (self.is_named(known), self.is_named(y));
                if xi && yi {
                    // Of two declared interfaces neither is the better
                    // name; of others, the methods are the same ones.
                    if xn && yn {
                        return types.identical(known, y);
                    }
                    let count = |ty| types.type_set(ty).map(|set| set.methods.len());
                    if count(known) != count(y) {
                        return false;
                    }
                } else if xi || yi {
                    return false;
                }
                // Of the types that unify, the one with a name is kept;
                // without one, a channel with a direction.
                let directed = matches!(
                    &*types.get(types.underlying(y)),
                    Type::Chan { dir, .. } if *dir != ChanDir::Both
                );
                if !alike.exact && !xn && (yn || directed) {
                    self.set(px, y);
                }
                return true;
            }
            _ => {}
        }
        if !alike.exact {
            match (self.is_interface(x), self.is_interface(y)) {
                (true, true) => return self.interfaces(x, y, false, pairs),
                (true, false) => return self.has_methods_of(y, x, pairs),
                (false, true) => return self.has_methods_of(x, y, pairs),
                (false, false) => {}
            }
        }
        // A type parameter of the function around goes to `x`.
        if self.is_param(y) {
            (x, y) = (y, x);
        }
        // Inside the types of an assignment, the types are identical.
        let inner = Alike {
            exact: alike.exact || alike.assign,
            ..alike
        };
        match (&*types.get(x), &*types.get(y)) {
            (Type::Basic(a), Type::Basic(b)) => a == b,
            (Type::Array { len: m, elem: a }, Type::Array { len: n, elem: b }) => {
                m == n && self.nify(*a, *b, inner, pairs)
            }
            (Type::Slice(a), Type::Slice(b))
            | (Type::Pointer(a), Type::Pointer(b))
            | (Type::Unique(a), Type::Unique(b))
            | (Type::Shared(a), Type::Shared(b))
            | (Type::Weak(a), Type::Weak(b))
            | (Type::Option(a), Type::Option(b))
            | (Type::Local(a), Type::Local(b)) => self.nify(*a, *b, inner, pairs),
            (Type::Struct(a), Type::Struct(b)) => {
                a.len() == b.len()
                    && a.iter().zip(b.iter()).all(|(f, g)| {
                        f.embedded == g.embedded
                            && f.tag == g.tag
                            && f.required == g.required
                            && (f.name, f.package) == (g.name, g.package)
                            && self.nify(f.ty, g.ty, inner, pairs)
                    })
            }
            (Type::Tuple(a), Type::Tuple(b)) => self.all(a, b, alike, pairs),
            (
                Type::Func {
                    params: p,
                    results: r,
                    variadic: v,
                    ..
                },
                Type::Func {
                    params: q,
                    results: s,
                    variadic: w,
                    ..
                },
            ) => v == w && self.all(p, q, inner, pairs) && self.all(r, s, inner, pairs),
            (Type::Interface { .. }, Type::Interface { .. }) => self.interfaces(x, y, true, pairs),
            (Type::Map { key: k, value: v }, Type::Map { key: l, value: w }) => {
                self.nify(*k, *l, inner, pairs) && self.nify(*v, *w, inner, pairs)
            }
            (Type::Chan { dir: d, elem: a }, Type::Chan { dir: e, elem: b }) => {
                (!alike.exact || d == e) && self.nify(*a, *b, inner, pairs)
            }
            (Type::Named { id: a, args: p }, Type::Named { id: b, args: q }) => {
                a == b && self.all(p, q, alike, pairs)
            }
            // What all the types of a type parameter of the function
            // around share stands for it.
            (Type::Param(_), _) => match types.common_under(x) {
                Ok(shared) => self.nify(shared, y, ASSIGN, pairs),
                Err(_) => false,
            },
            _ => false,
        }
    }

    fn all(
        &mut self,
        a: &[TypeId],
        b: &[TypeId],
        alike: Alike,
        pairs: &mut Vec<(TypeId, TypeId)>,
    ) -> bool {
        a.len() == b.len()
            && a.iter()
                .zip(b)
                .all(|(&a, &b)| self.nify(a, b, alike, pairs))
    }

    /// Two interfaces unify if they allow the same types and the methods
    /// of the one with fewer are methods of the other, which has as many
    /// if `same_count`.
    fn interfaces(
        &mut self,
        x: TypeId,
        y: TypeId,
        same_count: bool,
        pairs: &mut Vec<(TypeId, TypeId)>,
    ) -> bool {
        let types = self.types;
        let (Some(a), Some(b)) = (types.type_set(x), types.type_set(y)) else {
            return false;
        };
        let same_types = types.subset(&a.terms, &b.terms) && types.subset(&b.terms, &a.terms);
        if a.comparable != b.comparable || !same_types {
            return false;
        }
        if same_count && a.methods.len() != b.methods.len() {
            return false;
        }
        if pairs.contains(&(x, y)) || pairs.contains(&(y, x)) {
            return true;
        }
        pairs.push((x, y));
        let (fewer, more) = match a.methods.len() <= b.methods.len() {
            true => (&a.methods, &b.methods),
            false => (&b.methods, &a.methods),
        };
        let unified = self.methods_unify(fewer, pairs, |method| {
            more.get((method.name, method.package))
                .map(|other| other.sig)
        });
        pairs.pop();
        unified
    }

    /// A type that is no interface unifies with an interface if it has
    /// the methods of the interface.
    fn has_methods_of(
        &mut self,
        ty: TypeId,
        iface: TypeId,
        pairs: &mut Vec<(TypeId, TypeId)>,
    ) -> bool {
        let (types, names) = (self.types, self.names);
        let Some(set) = types.type_set(iface) else {
            return false;
        };
        self.methods_unify(&set.methods, pairs, |method| {
            types.method_type(ty, method.name, method.package, names)
        })
    }

    /// Whether the type of each of the methods unifies with what `other`
    /// has for the method.
    fn methods_unify(
        &mut self,
        methods: &MethodSet,
        pairs: &mut Vec<(TypeId, TypeId)>,
        other: impl Fn(&Method) -> Option<TypeId>,
    ) -> bool {
        let mut list = Vec::new();
        methods.for_each(&mut |method| list.push(method.clone()));
        list.iter()
            .all(|method| other(method).is_some_and(|sig| self.nify(method.sig, sig, EXACT, pairs)))
    }
}

impl<'a> Checker<'a> {
    /// `P`, `P and Q`, `P, Q, and R`: type parameters as a message lists
    /// them.
    fn params_string(&self, params: &[TypeId]) -> String {
        let names: Vec<String> = params.iter().map(|&p| self.type_string(p)).collect();
        match &names[..] {
            [] => String::new(),
            [one] => one.clone(),
            [one, other] => format!("{one} and {other}"),
            [first @ .., last] => format!("{}, and {last}", first.join(", ")),
        }
    }

    /// The generic function with type parameters of its own, for a call
    /// whose arguments are written with its type parameters: a function
    /// that calls itself.
    fn renamed(&mut self, generic: Generic) -> Generic {
        let fresh: Vec<TypeId> = generic
            .params
            .iter()
            .map(|&param| {
                let name = self.types.param_name(param).unwrap_or(self.blank);
                self.types.param(name)
            })
            .collect();
        for (&param, &new) in generic.params.iter().zip(&fresh) {
            if let Some(constraint) = self.types.constraint(param) {
                let constraint = self.types.substitute(constraint, &generic.params, &fresh);
                self.types.set_constraint(new, constraint);
            }
            if let Some(&span) = self.param_spans.get(&param) {
                self.param_spans.insert(new, span);
            }
        }
        Generic {
            sig: self.types.substitute(generic.sig, &generic.params, &fresh),
            params: fresh,
            written: generic.written,
        }
    }

    /// The type of the generic function for the call at `call` with the
    /// arguments `args`, each given to a parameter of the type in
    /// `wanted`: the type arguments that the call does not write are
    /// found from the arguments and from the constraints. `None`, with
    /// the reason reported, if they are not found.
    pub(super) fn instantiated_for(
        &mut self,
        cx: &Cx<'a>,
        (call, fun): (Span, ExprId),
        mut generic: Generic,
        wanted: &[TypeId],
        args: &[(ExprId, Operand)],
    ) -> Option<TypeId> {
        let mut wanted = wanted.to_vec();
        let mentioned = |this: &Self, ty: TypeId| this.types.mentions(ty, &generic.params);
        let in_args = args.iter().any(|(_, x)| mentioned(self, x.ty));
        let in_written = generic.written.iter().any(|&(ty, _)| mentioned(self, ty));
        if in_args || in_written {
            let old = generic.params.clone();
            generic = self.renamed(generic);
            for ty in &mut wanted {
                *ty = self.types.substitute(*ty, &old, &generic.params);
            }
        }
        // A generic function among the arguments is found together with
        // the function that is called: its type parameters, as new ones,
        // are looked for too.
        let own = generic.params.len();
        let mut args = args.to_vec();
        for (_, x) in &mut args {
            if let Some(params) = self.types.generic_func(x.ty) {
                let argument = self.renamed(Generic {
                    sig: self.types.underlying(x.ty),
                    params,
                    written: Vec::new(),
                });
                x.ty = argument.sig;
                generic.params.extend(argument.params);
            }
        }
        let args = &args[..];
        let within = format!("in call to {}, ", self.expr_string(cx, fun));
        let at = (call, &within[..]);
        // In a `.gopp` file a pointer stays a pointer where that fits;
        // where it does not, the arguments are read as what they point
        // to (OWN-P5).
        let how = |quiet: bool, read: bool| How {
            quiet,
            read,
            place: None,
        };
        let found = match self.own_rules(cx) {
            true => self
                .infer(cx, at, &generic, &wanted, args, how(true, false))
                .or_else(|| self.infer(cx, at, &generic, &wanted, args, how(true, true)))
                .or_else(|| self.infer(cx, at, &generic, &wanted, args, how(false, false))),
            false => self.infer(cx, at, &generic, &wanted, args, how(false, false)),
        }?;
        let instance = self.types.substitute(generic.sig, &generic.params, &found);
        self.note_instance(fun, &found[..own], instance);
        self.instances.push(Instance {
            params: generic.params,
            args: found,
            spans: generic.written.iter().map(|&(_, span)| span).collect(),
            at: call,
        });
        Some(instance)
    }

    fn infer(
        &mut self,
        cx: &Cx<'a>,
        (call, within): (Span, &str),
        generic: &Generic,
        wanted: &[TypeId],
        args: &[(ExprId, Operand)],
        how: How<'_>,
    ) -> Option<Vec<TypeId>> {
        let How { quiet, read, place } = how;
        let params = &generic.params;
        let written: Vec<TypeId> = generic.written.iter().map(|&(ty, _)| ty).collect();
        if written.len() == params.len() {
            return Some(written);
        }
        if args.iter().any(|(_, x)| x.mode == Mode::Unknown) {
            return None;
        }
        // The parameters with the type arguments that are written, for
        // messages that name what was written.
        let given = &params[..written.len()];
        let wanted: Vec<TypeId> = wanted
            .iter()
            .map(|&ty| self.types.substitute(ty, given, &written))
            .collect();
        let mut unifier = Unifier::new(self.types, self.names, params);
        for (at, &ty) in written.iter().enumerate() {
            unifier.set(at, ty);
        }
        let report = |this: &mut Self, span: Span, message: String| {
            if !quiet {
                this.error(Code::CannotInfer, span, format!("{within}{message}"));
            }
        };

        // What the arguments say.
        let nil = self.types.basic(Basic::UntypedNil);
        let mut untyped = Vec::new();
        for (at, (arg, x)) in args.iter().enumerate() {
            let parameter = wanted[at];
            let parameterized =
                self.types.mentions(parameter, params) || self.types.mentions(x.ty, params);
            if !parameterized {
                continue;
            }
            if self.is_untyped(x.ty) {
                if unifier.index(parameter).is_some() && x.ty != nil {
                    untyped.push(at);
                }
                continue;
            }
            let ty = match read {
                true => self.read(cx, x.clone()).ty,
                false => x.ty,
            };
            // What an argument that does not fit found stays found, for
            // the message.
            let fits = unifier.unify(parameter, ty, ASSIGN);
            if fits {
                continue;
            }
            let found = unifier.inferred();
            // As an argument, the generic function is what does not
            // match the type of its parameter.
            let (of, shown, pattern) = match place {
                Some(Place::Parameter(generic)) => (
                    generic.to_owned(),
                    self.expr_string(cx, *arg),
                    self.type_string(x.ty),
                ),
                Some(Place::Named(place)) => (
                    self.type_string(x.ty),
                    place.to_owned(),
                    self.type_string(parameter),
                ),
                None => (
                    self.type_string(x.ty),
                    self.expr_string(cx, *arg),
                    self.type_string(parameter),
                ),
            };
            let as_argument = matches!(place, Some(Place::Parameter(_)));
            let message = if found.iter().all(Option::is_none) {
                format!(
                    "type {of} of {shown} does not match {pattern} (cannot infer {})",
                    self.params_string(params)
                )
            } else {
                let (known, types): (Vec<TypeId>, Vec<TypeId>) = params
                    .iter()
                    .zip(&found)
                    .filter_map(|(&param, ty)| ty.map(|ty| (param, ty)))
                    .unzip();
                let inferred = self.types.substitute(parameter, &known, &types);
                let inferred_type = self.type_string(inferred);
                match (inferred != parameter && !as_argument, place.is_some()) {
                    (true, true) => format!(
                        "inferred type {inferred_type} for {pattern} does not match type {of} of \
                         {shown}"
                    ),
                    (true, false) => format!(
                        "type {of} of {shown} does not match inferred type {inferred_type} for \
                         {pattern}"
                    ),
                    (false, _) => format!("type {of} of {shown} does not match {pattern}"),
                }
            };
            report(self, cx.tree.exprs[*arg].span, message);
            return None;
        }

        // What the constraints say, as long as that finds more.
        loop {
            let before = unifier.unknowns();
            for (at, &param) in params.iter().enumerate() {
                let known = unifier.at(at);
                let constraint = self.types.constraint(param);
                let shown = |this: &Self, known: TypeId| {
                    format!(
                        "{} (type {}) does not satisfy {}",
                        this.type_string(param),
                        this.type_string(known),
                        constraint.map_or(String::new(), |c| this.type_string(c))
                    )
                };
                if let Some((core, single)) = self.types.core_term(param) {
                    match known {
                        Some(known) if !unifier.unify(known, core.ty, INEXACT) => {
                            let message = shown(self, known);
                            report(self, call, message);
                            return None;
                        }
                        None if single && !core.tilde => unifier.set(at, core.ty),
                        _ => {}
                    }
                }
                // A type that is found has the methods of the constraint,
                // whose types may say what other type parameters are.
                let Some(known) = known.filter(|&known| !self.types.mentions(known, params)) else {
                    continue;
                };
                let methods = constraint
                    .and_then(|constraint| self.types.type_set(constraint))
                    .map(|set| set.methods);
                let undecided = self.types.underlying(known) == Types::INVALID;
                let Some(methods) = methods.filter(|_| !undecided) else {
                    continue;
                };
                let mut same = |a: TypeId, b: TypeId| unifier.unify(a, b, EXACT);
                let lacks = self
                    .types
                    .lacks_with(known, &methods, self.names, &mut same);
                if let Some((method, lacks)) = lacks {
                    if !quiet {
                        let message = format!("{within}{}", shown(self, known));
                        let cause = self.lacks_cause(known, &method, lacks);
                        self.error_with_cause_after(Code::CannotInfer, call, message, cause);
                    }
                    return None;
                }
            }
            if unifier.unknowns() == before {
                break;
            }
        }

        // A constant of no type gives a type parameter that is still
        // open the default type of its kind; of several constants, the
        // largest kind.
        let mut kinds: Vec<Option<TypeId>> = vec![None; params.len()];
        for at in untyped {
            let Some(param) = unifier.index(wanted[at]) else {
                continue;
            };
            if unifier.at(param).is_some() {
                continue;
            }
            let ty = args[at].1.ty;
            kinds[param] = match kinds[param] {
                None => Some(ty),
                Some(kind) => match self.larger_kind(kind, ty) {
                    Some(larger) => Some(larger),
                    None => {
                        let message = format!(
                            "mismatched types {} and {} (cannot infer {})",
                            self.type_string(kind),
                            self.type_string(ty),
                            self.type_string(params[param])
                        );
                        report(self, cx.tree.exprs[args[at].0].span, message);
                        return None;
                    }
                },
            };
        }
        for (param, kind) in kinds.into_iter().enumerate() {
            if let Some(kind) = kind {
                unifier.set(param, self.types.default_type(kind));
            }
        }

        // What was found may be written with type parameters that were
        // found too: they are replaced until nothing changes. A type
        // parameter that is found as a type written with itself is not
        // found.
        let mut found = unifier.inferred();
        self.steps += unifier.steps + params.len() as u64;
        self.kill_cycles(params, &mut found);
        loop {
            let (known, types): (Vec<TypeId>, Vec<TypeId>) = params
                .iter()
                .zip(&found)
                .filter_map(|(&param, ty)| ty.map(|ty| (param, ty)))
                .unzip();
            let mut changed = false;
            for slot in found.iter_mut().skip(written.len()) {
                if let Some(ty) = *slot {
                    let simpler = self.types.substitute(ty, &known, &types);
                    changed |= simpler != ty;
                    *slot = Some(simpler);
                }
            }
            if !changed {
                break;
            }
        }
        for (at, ty) in found.iter().enumerate() {
            if ty.is_none_or(|ty| self.types.mentions(ty, params)) {
                if !quiet {
                    self.cannot_infer(call, within, params[at]);
                }
                return None;
            }
        }
        found.into_iter().collect()
    }

    fn cannot_infer(&mut self, at: Span, within: &str, param: TypeId) {
        if !self.reported.insert((at, Code::CannotInfer)) {
            return;
        }
        let name = self.type_string(param);
        let message = format!("{within}cannot infer {name}");
        let mut diagnostic = Diagnostic::new(Code::CannotInfer, at, message);
        if let Some(&declared) = self.param_spans.get(&param) {
            let note = format!("{name} declared here");
            diagnostic = diagnostic.with_note(Code::NoteDeclaredHere, Some(declared), note);
        }
        self.sink.push(diagnostic);
    }

    /// Notes that `expr` names a generic function or type with the type
    /// arguments `args`, which make it the type `ty`.
    pub(super) fn note_instance(&mut self, expr: ExprId, args: &[TypeId], ty: TypeId) {
        if self.memo.is_some() {
            let instance = crate::object::Instance {
                args: args.to_vec(),
                ty,
            };
            self.instantiated.insert(expr, instance);
        }
    }

    /// Forgets what was found for a type parameter if it is written with
    /// the type parameter itself, through what was found for others:
    /// replacing it would never end.
    fn kill_cycles(&self, params: &[TypeId], found: &mut [Option<TypeId>]) {
        let mut walk = (FxHashSet::default(), FxHashSet::default());
        for &param in params {
            self.find_cycles(param, params, found, &mut walk, 0);
        }
    }

    /// `walk` is the types on the way to `ty`, and the types from which
    /// no way leads back to a type on the way to them: those are not
    /// walked again. Says whether `ty` is such a type.
    fn find_cycles(
        &self,
        ty: TypeId,
        params: &[TypeId],
        found: &mut [Option<TypeId>],
        walk: &mut (FxHashSet<TypeId>, FxHashSet<TypeId>),
        depth: u32,
    ) -> bool {
        // Only a type parameter leads back to a type parameter.
        if !self.types.has_param(ty) || walk.1.contains(&ty) {
            return true;
        }
        let index = params.iter().position(|&param| param == ty);
        if !walk.0.insert(ty) {
            if let Some(index) = index {
                found[index] = None;
            }
            return false;
        }
        // A type deeper than unification goes says nothing more.
        let mut clear = depth < DEPTH_LIMIT;
        if clear {
            match index {
                Some(index) => {
                    if let Some(known) = found[index] {
                        clear = self.find_cycles(known, params, found, walk, depth + 1);
                    }
                }
                None => {
                    for part in self.types.parts(ty) {
                        clear &= self.find_cycles(part, params, found, walk, depth + 1);
                    }
                }
            }
        }
        walk.0.remove(&ty);
        if clear {
            walk.1.insert(ty);
        }
        clear
    }

    /// A generic function that is used as a value: it has the type
    /// arguments that the place it is given to asks for, a place of the
    /// function type `to` that a message calls `place`.
    pub(super) fn generic_value(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        x: Operand,
        to: Option<TypeId>,
        (place, within): (Place<'_>, &str),
    ) -> Operand {
        let Some(params) = self.types.generic_func(x.ty) else {
            return x;
        };
        let span = cx.tree.exprs[expr].span;
        let target = to.map(|to| self.types.underlying(to)).filter(|&to| {
            matches!(&*self.types.get(to), Type::Func { type_params, .. } if type_params.is_empty())
        });
        let written = self.partial.get(&self.unparen(cx, expr)).cloned();
        // Without a place that says what the function is, the type
        // arguments that are written and the constraints may.
        if target.is_none() && written.is_none() {
            let message = format!(
                "cannot use generic function {} without instantiation",
                self.expr_string(cx, expr)
            );
            self.error(Code::GenericWithoutInstantiation, span, message);
            return Operand::UNKNOWN;
        }
        let mut generic = Generic {
            sig: self.types.underlying(x.ty),
            params,
            written: written.unwrap_or_default(),
        };
        if target.is_some_and(|target| self.types.mentions(target, &generic.params)) {
            generic = self.renamed(generic);
        }
        // The function without its type parameters is what the place
        // takes.
        let plain = self.types.generic_func_type(generic.sig, &[]);
        let args: Vec<(ExprId, Operand)> = target
            .map(|target| (expr, Operand::value(target)))
            .into_iter()
            .collect();
        let wanted = &[plain][..args.len()];
        let how = How {
            place: Some(place),
            ..How::default()
        };
        let Some(found) = self.infer(cx, (span, within), &generic, wanted, &args, how) else {
            return Operand::UNKNOWN;
        };
        let instance = self.types.substitute(generic.sig, &generic.params, &found);
        self.note_instance(expr, &found, instance);
        self.instances.push(Instance {
            params: generic.params,
            args: found,
            spans: generic.written.iter().map(|&(_, span)| span).collect(),
            at: span,
        });
        if let Some(memo) = &mut self.memo {
            memo.insert(expr, Operand::value(instance));
        }
        Operand::value(instance)
    }
}
