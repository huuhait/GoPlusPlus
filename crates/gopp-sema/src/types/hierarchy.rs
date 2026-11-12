//! The parents of a type (`@Extends`, TYP-16): what it inherits from
//! them, whether it has a type table, and whether it is abstract.

use std::sync::Arc;

use rustc_hash::FxHashSet;

use super::{Marks, NamedId, Type, TypeId, Types};

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

    /// Forgets what was worked out of hierarchies: the methods of a type
    /// are declared after its parents are known.
    pub fn forget_hierarchy(&self) {
        self.write(|inner| {
            inner.tables.clear();
            inner.abstracts.clear();
            inner.open_abstracts.clear();
        });
    }
}
