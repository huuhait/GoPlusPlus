//! The parents of a type (`@Extends`, TYP-16): what it inherits from
//! them, whether it has a type table, and whether it is abstract.

use std::sync::Arc;

use rustc_hash::FxHashSet;

use super::{Marks, NamedId, Type, TypeId, Types};

/// What a value of a type holds where it is, of what is not kept on the
/// heap.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Held {
    /// A pointer, or a `Local`, which may be made of one.
    Pointer,
    /// No pointer, but values of these type parameters, each of which
    /// stands for a type that may hold one.
    Params(Arc<[TypeId]>),
}

/// What a function does with the pointers that it is given, for those
/// who call it (OWN-B3).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Lends {
    /// For each parameter, the receiver first: whether a result may
    /// point into what the parameter reaches.
    pub gives: Vec<bool>,
    /// Each pair of parameters of which the function may store what
    /// the second reaches behind a pointer that the first reaches.
    pub stores: Vec<(u32, u32)>,
    /// For each parameter: whether the function may keep a function
    /// value that it is given there longer than the call (CLO-4).
    pub keeps: Vec<bool>,
    /// For each parameter: whether the function may change what the
    /// parameter reaches through pointers.
    pub changes: Vec<bool>,
    /// For each parameter: whether the function writes through a
    /// pointer of the parameter, itself or in a function that it
    /// calls and that says so; and whether it writes what is read
    /// through pointers behind what the parameter points to. A call
    /// of a function that is not known counts for the pointers that
    /// it is given, not for what a value that it is given holds: this
    /// is what a pointer into what is only read is held to.
    pub writes: Vec<bool>,
    pub writes_deep: Vec<bool>,
    /// For each parameter: whether the function points into what the
    /// value behind it owns apart from itself (OWN-B10).
    pub borrows: Vec<bool>,
    /// For each parameter that a result may point into: whether it may
    /// point into what is read through pointers behind the parameter,
    /// and not only into what the parameter points to.
    pub deep: Vec<bool>,
    /// Whether a result may point to what no parameter leads to.
    pub outside: bool,
    /// Whether a result may point into a variable of a package, which
    /// is only read once the program has started (CON-18).
    pub package: bool,
}

/// A variable of a package: the path of its package, and its object
/// there.
pub type Global = (lasso::Spur, crate::object::ObjId);

/// A function as those who follow what runs in a goroutine name it.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum TouchKey {
    /// A function of the package with the path.
    Func(lasso::Spur, crate::object::ObjId),
    /// A method of a declared type, by its name.
    Method(NamedId, lasso::Spur),
    /// A function literal, `init` or the value of a variable of the
    /// package with the path: its number there.
    Body(lasso::Spur, u32),
}

/// The variables of packages that a function names itself, and what it
/// calls (CON-18).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Touch {
    pub uses: Vec<Global>,
    pub calls: Vec<TouchKey>,
    /// The variables that it changes itself outside `init`; and
    /// whether it writes through a pointer that may point into any
    /// variable which a pointer is taken into.
    pub writes: Vec<Global>,
    pub writes_any: bool,
    /// Those of them of which it may free or move what the variable
    /// owns apart from itself: a pointer into that does not live
    /// through a call of the function.
    pub replaces: Vec<Global>,
    /// What its `go` statements start.
    pub starts: Vec<TouchKey>,
    /// It calls a function value: any function that is used as a
    /// value may run.
    pub values: bool,
    /// The names of the methods of interfaces, and of those that a
    /// child may replace, that it calls: any method of the name may
    /// run.
    pub methods: Vec<lasso::Spur>,
}

impl Lends {
    /// Whether the function may give back what the argument at
    /// `position` of `count` reaches. The last parameter takes the
    /// arguments that are left over, and one call that gives several
    /// values is given to all of them.
    pub fn gives_back(&self, position: usize, count: usize) -> bool {
        match count < self.gives.len() {
            true => self.gives.contains(&true),
            false => {
                let last = self.gives.len().saturating_sub(1);
                self.gives.get(position.min(last)).copied().unwrap_or(false)
            }
        }
    }

    /// What the function says, for a call with `count` arguments that
    /// gives the elements of the last parameter one by one: each of
    /// them is held to what is said of that parameter. An element that
    /// is given so holds no pointer (OWN-B5), so nothing is stored
    /// behind one.
    pub fn listed(&self, count: usize) -> Lends {
        let last = self.gives.len().saturating_sub(1);
        let each = |all: &[bool]| -> Vec<bool> {
            (0..count)
                .map(|arg| all.get(arg.min(last)).copied().unwrap_or(false))
                .collect()
        };
        let behind = |&&(sink, _): &&(u32, u32)| (sink as usize) < last;
        let stores = self.stores.iter().filter(behind).flat_map(|&(sink, from)| {
            let given = match (from as usize) < last {
                true => from as usize..from as usize + 1,
                false => last..count,
            };
            given.map(move |from| (sink, from as u32))
        });
        Lends {
            gives: each(&self.gives),
            stores: stores.collect(),
            keeps: each(&self.keeps),
            changes: each(&self.changes),
            writes: each(&self.writes),
            writes_deep: each(&self.writes_deep),
            borrows: each(&self.borrows),
            deep: each(&self.deep),
            outside: self.outside,
            package: self.package,
        }
    }
}

impl Default for Held {
    fn default() -> Held {
        Held::Params([].into())
    }
}

/// In the path of a member that is found: the step to the parent with
/// the index in the other bits. A step without the bit is a field.
pub const PARENT: u32 = 1 << 31;

impl Types {
    /// Gives the declared type its parents, in the order of its
    /// `@Extends` attributes, written with its type parameters.
    pub fn set_parents(&self, named: NamedId, parents: Vec<TypeId>) {
        self.write(|inner| inner.named[named.0 as usize].parents = parents);
    }

    /// Says that the declared type is marked `@Abstract`.
    pub fn set_abstract(&self, named: NamedId) {
        self.write(|inner| inner.named[named.0 as usize].abstract_marked = true);
    }

    /// The direct parents of the type, with its type arguments put in.
    /// A type parameter has those that its constraint names.
    pub fn parents(&self, ty: TypeId) -> Vec<TypeId> {
        let of = match &*self.get(ty) {
            Type::Param(_) => match self.constraint(ty) {
                Some(constraint) => constraint,
                None => return Vec::new(),
            },
            _ => ty,
        };
        let Type::Named { id, args } = &*self.get(of) else {
            return Vec::new();
        };
        let (parents, params) = self.read(|inner| {
            let named = &inner.named[id.0 as usize];
            (named.parents.clone(), named.params.clone())
        });
        match args.is_empty() || parents.is_empty() {
            true => parents,
            false => parents
                .iter()
                .map(|&parent| self.substitute(parent, &params, args))
                .collect(),
        }
    }

    /// The way from the type to its ancestor `ancestor`: the index of the
    /// parent at each step; empty for the type itself. `None` if it is no
    /// ancestor.
    pub fn ancestor_path(&self, ty: TypeId, ancestor: TypeId) -> Option<Vec<u32>> {
        let mut seen = FxHashSet::default();
        let mut path = Vec::new();
        self.ancestor_in(ty, ancestor, &mut seen, &mut path)
            .then_some(path)
    }

    fn ancestor_in(
        &self,
        ty: TypeId,
        ancestor: TypeId,
        seen: &mut FxHashSet<TypeId>,
        path: &mut Vec<u32>,
    ) -> bool {
        if self.identical(ty, ancestor) {
            return true;
        }
        if !seen.insert(ty) {
            return false;
        }
        for (index, parent) in self.parents(ty).into_iter().enumerate() {
            path.push(index as u32);
            if self.ancestor_in(parent, ancestor, seen, path) {
                return true;
            }
            path.pop();
        }
        false
    }

    /// Every ancestor of the type, each once: the parents in their order,
    /// each followed by its own.
    pub fn ancestors(&self, ty: TypeId) -> Vec<TypeId> {
        let mut all = Vec::new();
        let mut seen: FxHashSet<TypeId> = [ty].into_iter().collect();
        let mut rest = self.parents(ty);
        rest.reverse();
        while let Some(next) = rest.pop() {
            if !seen.insert(next) {
                continue;
            }
            all.push(next);
            let mut parents = self.parents(next);
            parents.reverse();
            rest.extend(parents);
        }
        all
    }

    /// The methods that the type has of its own and of its ancestors,
    /// the type itself first: each with the type that declares it and
    /// its marks. A name that comes again is replaced by the one before.
    pub fn methods_from(&self, ty: TypeId) -> Vec<(lasso::Spur, TypeId, Marks)> {
        let mut all = Vec::new();
        for of in std::iter::once(ty).chain(self.ancestors(ty)) {
            all.extend(
                (self.own_marks(of).into_iter()).map(|(method, marks)| (method.name, of, marks)),
            );
        }
        all
    }

    /// Whether the declared type declares the method `name` without a
    /// body (`@Abstract`).
    pub fn declares_abstract(&self, named: NamedId, name: lasso::Spur) -> bool {
        self.read(|inner| {
            let methods = &inner.named[named.0 as usize].methods;
            let found = methods.iter().find(|m| m.method.name == name);
            found.is_some_and(|m| m.marks.has(Marks::ABSTRACT))
        })
    }

    /// The marks of all the methods that the declared type declares
    /// itself, together.
    fn own_marks(&self, ty: TypeId) -> Vec<(super::Method, Marks)> {
        let Type::Named { id, .. } = &*self.get(ty) else {
            return Vec::new();
        };
        self.read(|inner| {
            let methods = &inner.named[id.0 as usize].methods;
            methods
                .iter()
                .map(|m| (m.method.clone(), m.marks))
                .collect()
        })
    }

    /// Whether a value of the type starts with a type table: the type
    /// declares or inherits a method that may be replaced, or a parent
    /// has a table (TYP-22). Only a type with one is the target of an
    /// upcast and the source of a downcast.
    pub fn has_table(&self, ty: TypeId) -> bool {
        if let Some(known) = self.read(|inner| inner.tables.get(&ty).copied()) {
            return known;
        }
        let dynamic = |marks: Marks| {
            marks.has(Marks::VIRTUAL) || marks.has(Marks::ABSTRACT) || marks.has(Marks::OVERRIDE)
        };
        let own = self.own_marks(ty).iter().any(|&(_, marks)| dynamic(marks));
        let has = own
            || self
                .parents(ty)
                .into_iter()
                .any(|parent| self.has_table(parent));
        self.steps
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        self.write(|inner| inner.tables.insert(ty, has));
        has
    }

    /// The `@Abstract` methods of the type and of its ancestors that the
    /// type has no other version of: each with the type that declares it.
    /// What the parents leave open and the type does not declare, it
    /// leaves open too.
    pub fn open_abstracts(&self, ty: TypeId) -> Arc<Vec<(TypeId, super::Method)>> {
        if let Some(known) = self.read(|inner| inner.open_abstracts.get(&ty).cloned()) {
            return known;
        }
        self.steps
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let own = self.own_marks(ty);
        let declares = |method: &super::Method| {
            own.iter()
                .any(|(other, _)| (other.name, other.package) == (method.name, method.package))
        };
        let mut open: Vec<(TypeId, super::Method)> = Vec::new();
        for parent in self.parents(ty) {
            for (holder, method) in self.open_abstracts(parent).iter() {
                if !declares(method) {
                    open.push((*holder, method.clone()));
                }
            }
        }
        for (method, marks) in &own {
            if marks.has(Marks::ABSTRACT) {
                open.push((ty, method.clone()));
            }
        }
        let open = Arc::new(open);
        self.write(|inner| inner.open_abstracts.insert(ty, open.clone()));
        open
    }

    /// Whether the type has no values of its own: it is marked
    /// `@Abstract`, or an `@Abstract` method is left open (TYP-19).
    pub fn is_abstract(&self, ty: TypeId, _names: &lasso::ThreadedRodeo) -> bool {
        let Type::Named { id, .. } = &*self.get(ty) else {
            return false;
        };
        if let Some(known) = self.read(|inner| inner.abstracts.get(&ty).copied()) {
            return known;
        }
        let marked = self.read(|inner| inner.named[id.0 as usize].abstract_marked);
        let is = marked || !self.open_abstracts(ty).is_empty();
        self.write(|inner| inner.abstracts.insert(ty, is));
        is
    }

    pub fn is_abstract_marked(&self, ty: TypeId) -> bool {
        match &*self.get(ty) {
            Type::Named { id, .. } => self.read(|inner| inner.named[id.0 as usize].abstract_marked),
            _ => false,
        }
    }

    /// The names of the fields and the methods that the declared type
    /// declares itself, each with its package.
    pub fn member_names(&self, ty: TypeId) -> Vec<(lasso::Spur, Option<lasso::Spur>)> {
        let mut names: Vec<_> = self
            .own_marks(ty)
            .into_iter()
            .map(|(method, _)| (method.name, method.package))
            .collect();
        if let Type::Struct(fields) = &*self.get(self.underlying(ty)) {
            names.extend(fields.iter().map(|field| (field.name, field.package)));
        }
        names
    }

    /// Whether a value of the type holds a pointer where it is.
    pub fn holds_pointer(&self, ty: TypeId) -> bool {
        matches!(self.held(ty), Held::Pointer)
    }

    /// Whether a value of the type holds a pointer where it is, or a
    /// value of a type parameter, which stands for a type that may
    /// hold one.
    pub fn may_hold_pointer(&self, ty: TypeId) -> bool {
        !matches!(self.held(ty), Held::Params(params) if params.is_empty())
    }

    /// What a value of the type holds where it is, of what is not kept
    /// on the heap (OWN-B5): in itself, in a field (of a parent too),
    /// an element of an array, an option or a part of a tuple. What a
    /// slice, a map, a channel or a handle leads to is on the heap,
    /// where no pointer is kept.
    pub fn held(&self, ty: TypeId) -> Held {
        self.holds(ty, &mut (FxHashSet::default(), 0))
    }

    /// `held`, with the types that are being looked into and how often
    /// one of them was met again: a type that holds itself is refused
    /// elsewhere, and is taken to hold nothing more here. Without a
    /// pointer, the answer is kept only when no type was met again
    /// below, so what is kept for a type is the same whoever asks first.
    fn holds(&self, ty: TypeId, seen: &mut (FxHashSet<TypeId>, usize)) -> Held {
        if let Some(known) = self.read(|inner| inner.holding.get(&ty).cloned()) {
            return known;
        }
        if !seen.0.insert(ty) {
            seen.1 += 1;
            return Held::default();
        }
        let before = seen.1;
        self.steps
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let under = self.underlying(ty);
        let parts: Vec<TypeId> = match &*self.get(under) {
            Type::Struct(fields) => {
                let own = fields.iter().map(|field| field.ty);
                own.chain(self.parents(ty)).collect()
            }
            Type::Array { elem, .. } | Type::Option(elem) => vec![*elem],
            Type::Tuple(parts) => parts.to_vec(),
            _ => Vec::new(),
        };
        let mut found = match &*self.get(under) {
            Type::Pointer(_) => Held::Pointer,
            // A `Local` interface may be made of a pointer; a `Local`
            // function is a function value.
            Type::Local(of) if !matches!(&*self.get(self.underlying(*of)), Type::Func { .. }) => {
                Held::Pointer
            }
            Type::Param(_) => Held::Params([under].into()),
            _ => Held::default(),
        };
        for part in parts {
            let Held::Params(so_far) = &found else {
                break;
            };
            found = match super::deep(|| self.holds(part, seen)) {
                Held::Pointer => Held::Pointer,
                Held::Params(more) if more.iter().all(|param| so_far.contains(param)) => continue,
                Held::Params(more) => {
                    let mut all = so_far.to_vec();
                    for &param in more.iter() {
                        if !all.contains(&param) {
                            all.push(param);
                        }
                    }
                    Held::Params(all.into())
                }
            };
        }
        seen.0.remove(&ty);
        if found == Held::Pointer || seen.1 == before {
            self.write(|inner| inner.holding.insert(ty, found.clone()));
        }
        found
    }

    /// Notes that a declaration puts a value of its type parameter
    /// `param` on the heap, into `what`: no type argument for it holds
    /// a pointer (OWN-B5).
    pub fn set_kept(&self, param: TypeId, what: Arc<str>) {
        self.write(|inner| inner.kept.insert(param, what));
    }

    /// Whether a value of the type may lend: it may hold a pointer, or
    /// it holds a function value, which may use the variables of the
    /// function that made it. A function value counts behind a slice,
    /// a map, a channel and a handle too: it is kept there.
    pub fn may_lend(&self, ty: TypeId) -> bool {
        self.may_hold_pointer(ty) || self.has_func(ty, false, &mut (FxHashSet::default(), 0))
    }

    /// Whether a value of the type holds or leads to a plain function
    /// value: one that may be given to another goroutine, no `Local`
    /// one.
    pub fn holds_func(&self, ty: TypeId) -> bool {
        self.has_func(ty, true, &mut (FxHashSet::default(), 0))
    }

    /// Whether a value of the type holds or leads to a function value:
    /// with `plain`, to one that is no `Local` one. Kept as `holds`
    /// keeps what it finds.
    fn has_func(&self, ty: TypeId, plain: bool, seen: &mut (FxHashSet<TypeId>, usize)) -> bool {
        if let Some(known) = self.read(|inner| inner.lending.get(&(ty, plain)).copied()) {
            return known;
        }
        // What a type that needs instances without end is made of is
        // not walked.
        if self.is_expanding(ty) {
            return false;
        }
        if !seen.0.insert(ty) {
            seen.1 += 1;
            return false;
        }
        let before = seen.1;
        self.steps
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let mut parts: Vec<TypeId> = Vec::new();
        let found = match &*self.get(self.underlying(ty)) {
            Type::Func { .. } => true,
            Type::Local(of) => {
                !plain && matches!(&*self.get(self.underlying(*of)), Type::Func { .. })
            }
            Type::Struct(fields) => {
                parts.extend(fields.iter().map(|field| field.ty));
                parts.extend(self.parents(ty));
                false
            }
            Type::Map { key, value } => {
                parts.extend([*key, *value]);
                false
            }
            Type::Tuple(all) => {
                parts.extend(all.iter());
                false
            }
            Type::Array { elem, .. }
            | Type::Option(elem)
            | Type::Slice(elem)
            | Type::Chan { elem, .. }
            | Type::Unique(elem)
            | Type::Shared(elem)
            | Type::Weak(elem) => {
                parts.push(*elem);
                false
            }
            _ => false,
        };
        let found = found
            || (parts.into_iter()).any(|part| super::deep(|| self.has_func(part, plain, seen)));
        seen.0.remove(&ty);
        if found || seen.1 == before {
            self.write(|inner| inner.lending.insert((ty, plain), found));
        }
        found
    }

    /// Whether a value of the type owns something apart from itself
    /// where it is: a slice, a map, an interface value, a `Unique` or a
    /// `Shared`, in itself, a field, an element of an array or an
    /// option (OWN-B10). With `behind`, whether a pointer in it points
    /// to a value that does. A type parameter stands for a type that
    /// may.
    pub fn owns_apart(&self, ty: TypeId, behind: bool) -> bool {
        self.owns(ty, behind, &mut (FxHashSet::default(), 0))
    }

    /// `owns_apart`, kept as `holds` keeps what it finds.
    fn owns(&self, ty: TypeId, behind: bool, seen: &mut (FxHashSet<TypeId>, usize)) -> bool {
        if let Some(known) = self.read(|inner| inner.owning.get(&(ty, behind)).copied()) {
            return known;
        }
        if !seen.0.insert(ty) {
            seen.1 += 1;
            return false;
        }
        let before = seen.1;
        self.steps
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let mut parts: Vec<TypeId> = Vec::new();
        let found = match &*self.get(self.underlying(ty)) {
            Type::Slice(_)
            | Type::Map { .. }
            | Type::Interface { .. }
            | Type::Unique(_)
            | Type::Shared(_) => !behind,
            Type::Param(_) => true,
            Type::Pointer(to) => behind && self.owns_apart(*to, false),
            Type::Struct(fields) => {
                parts.extend(fields.iter().map(|field| field.ty));
                parts.extend(self.parents(ty));
                false
            }
            Type::Array { elem, .. } | Type::Option(elem) => {
                parts.push(*elem);
                false
            }
            Type::Tuple(all) => {
                parts.extend(all.iter());
                false
            }
            _ => false,
        };
        let found =
            found || (parts.into_iter()).any(|part| super::deep(|| self.owns(part, behind, seen)));
        seen.0.remove(&ty);
        if found || seen.1 == before {
            self.write(|inner| inner.owning.insert((ty, behind), found));
        }
        found
    }

    /// Whether a value of the type leads to a place that may be given
    /// a pointer: it holds a pointer to what holds one. A type
    /// parameter stands for a type that may.
    pub fn leads_to_store(&self, ty: TypeId) -> bool {
        self.leads(ty, &mut (FxHashSet::default(), 0))
    }

    /// `leads_to_store`, kept as `holds` keeps what it finds.
    fn leads(&self, ty: TypeId, seen: &mut (FxHashSet<TypeId>, usize)) -> bool {
        if let Some(known) = self.read(|inner| inner.leading.get(&ty).copied()) {
            return known;
        }
        if !seen.0.insert(ty) {
            seen.1 += 1;
            return false;
        }
        let before = seen.1;
        self.steps
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let parts: Vec<TypeId> = match &*self.get(self.underlying(ty)) {
            Type::Struct(fields) => {
                let own = fields.iter().map(|field| field.ty);
                own.chain(self.parents(ty)).collect()
            }
            Type::Array { elem, .. } | Type::Option(elem) => vec![*elem],
            Type::Tuple(parts) => parts.to_vec(),
            _ => Vec::new(),
        };
        let found = match &*self.get(self.underlying(ty)) {
            Type::Pointer(to) => self.may_hold_pointer(*to),
            Type::Param(_) => true,
            _ => (parts.into_iter()).any(|part| super::deep(|| self.leads(part, seen))),
        };
        seen.0.remove(&ty);
        if found || seen.1 == before {
            self.write(|inner| inner.leading.insert(ty, found));
        }
        found
    }

    /// Says what the method `name` of the declared type does with the
    /// pointers that it is given.
    pub fn set_lends(&self, named: NamedId, name: lasso::Spur, lends: Arc<Lends>) {
        self.write(|inner| inner.lends.insert((named, name), lends));
    }

    pub fn lends(&self, named: NamedId, name: lasso::Spur) -> Option<Arc<Lends>> {
        self.read(|inner| inner.lends.get(&(named, name)).cloned())
    }

    /// Says which variables of packages the function names and what it
    /// calls.
    pub fn set_touch(&self, key: TouchKey, touch: Arc<Touch>) {
        self.write(|inner| inner.touches.insert(key, touch));
    }

    pub fn touch(&self, key: TouchKey) -> Option<Arc<Touch>> {
        self.read(|inner| inner.touches.get(&key).cloned())
    }

    /// Into what a declaration puts a value of the type parameter on
    /// the heap, if one does.
    pub fn kept(&self, param: TypeId) -> Option<Arc<str>> {
        self.read(|inner| inner.kept.get(&param).cloned())
    }

    /// Forgets what was worked out of hierarchies: the methods of a type
    /// are declared after its parents are known.
    pub fn forget_hierarchy(&self) {
        self.write(|inner| {
            inner.tables.clear();
            inner.abstracts.clear();
            inner.holding.clear();
            inner.leading.clear();
            inner.lending.clear();
            inner.owning.clear();
            inner.open_abstracts.clear();
        });
    }
}
