//! How a value of a type is copied: by its bytes, by a rule of its own,
//! or not at all (OWN-C1 to OWN-C5).
//!
//! The category of a type is that of its parts, unless the type has a
//! `drop` method or is pinned. It is worked out once per type and kept.
//! It is asked only when the methods of every declared type that the
//! type reaches are declared.

use std::sync::atomic::Ordering;

use rustc_hash::{FxHashMap, FxHashSet};

use super::{Basic, Marker, NamedId, Type, TypeId, TypeSet, Types, deep};

/// How a value of a type is copied. A type made of parts has the last
/// category that one of its parts has.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Category {
    /// By its bytes: it satisfies `Copy`.
    Trivial,
    /// By a rule of the type: it satisfies `Clone`.
    Copyable,
    /// Not at all: it has one owner.
    NoCopy,
}

/// Why a type is not copied the way that was asked for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotCopied {
    /// The type has a `drop` method, and no copy method `Clone() T`.
    Drop,
    /// The type has a `drop` method and its copy method `Clone() T`.
    CopyMethod,
    /// The type is declared with `@Pinned`.
    Pinned,
    /// A type parameter whose constraint does not say that it is copied
    /// that way.
    Param,
    /// A type that is copied by a rule of its own: a string, a slice, an
    /// interface value.
    OwnRule,
}

/// What a copy of a value is (OWN-R6).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CopyRule {
    /// The bytes.
    Bytes,
    /// A string: the same bytes, which never change, counted once more.
    SameBytes,
    /// A slice or a map: a new buffer with a copy of every element.
    Elements,
    /// A `Unique`: a new allocation with a copy of what it points to.
    Pointee,
    /// A `Shared`, a `Weak`, a channel, a function value: the handle,
    /// counted once more.
    Handle,
    /// An interface value: a box of its own with a copy of the value.
    Boxed,
    /// A struct, an array, an option: each part by its own rule.
    Parts,
    /// A type with a `drop` method: a call of its method `Clone() T`.
    Method,
    /// A type parameter: the rule of the type that it stands for.
    OfInstance,
}

/// One walk over the types that a type holds.
#[derive(Default)]
struct Walk {
    /// What is asked is whether a value holds what must be ended, not
    /// how it is copied: `@Pinned` alone says nothing of that.
    owned: bool,
    /// The declared types that the walk is inside of, the outermost
    /// first, and where each is in that order.
    inside: Vec<TypeId>,
    depth: FxHashMap<TypeId, usize>,
    /// The types whose category is not final yet: each holds, through a
    /// slice or a map, a declared type that the walk is inside of.
    open: Vec<TypeId>,
    /// A constraint that is still being declared was met: nothing of
    /// this walk is kept.
    unknown: bool,
    steps: u64,
}

/// For a type that is final: it holds no type that the walk is inside of.
const FINAL: usize = usize::MAX;

impl Types {
    /// How a value of the type is copied. A type parameter is copied as
    /// its constraint says: by its bytes if the constraint is `Copy` or
    /// lists only such types, and not at all if it says nothing.
    pub fn category(&self, ty: TypeId) -> Category {
        self.category_as(ty, false)
    }

    /// Whether a value of the type holds something that is ended with
    /// it: a buffer, a counted handle, a value with a `drop` method.
    pub fn holds_owned(&self, ty: TypeId) -> bool {
        self.category_as(ty, true) != Category::Trivial
    }

    fn category_as(&self, ty: TypeId, owned: bool) -> Category {
        let mut walk = Walk {
            owned,
            ..Walk::default()
        };
        let (category, _) = self.category_in(ty, &mut walk);
        self.steps.fetch_add(walk.steps, Ordering::Relaxed);
        category
    }

    /// What a copy of a value of the type is; `None` for a type that is
    /// not copied.
    pub fn copy_rule(&self, ty: TypeId) -> Option<CopyRule> {
        match self.category(ty) {
            Category::NoCopy => return None,
            Category::Trivial => return Some(CopyRule::Bytes),
            Category::Copyable => {}
        }
        Some(match &*self.get(ty) {
            Type::Basic(_) => CopyRule::SameBytes,
            Type::Slice(_) | Type::Map { .. } => CopyRule::Elements,
            Type::Unique(_) => CopyRule::Pointee,
            Type::Interface { .. } => CopyRule::Boxed,
            Type::Param(_) => CopyRule::OfInstance,
            Type::Named { id, args } => match self.own_copy(ty, *id, args) {
                Some(_) => CopyRule::Method,
                None => return self.copy_rule(self.underlying(ty)),
            },
            Type::Array { .. } | Type::Option(_) | Type::Struct(_) | Type::Tuple(_) => {
                CopyRule::Parts
            }
            _ => CopyRule::Handle,
        })
    }

    /// The category of `ty`, and the outermost declared type of those
    /// that the walk is inside of that `ty` holds; `FINAL` if it holds
    /// none. Types that hold each other through slices and maps have one
    /// category, which is known when the walk leaves the outermost of
    /// them: until then they are `open`.
    fn category_in(&self, ty: TypeId, walk: &mut Walk) -> (Category, usize) {
        use Category::{Copyable, Trivial};
        let node = self.get(ty);
        // What has no parts is said at once. A slice, a map and a
        // `Unique` are copied by their own rule whatever is in them.
        let (least, parts): (Category, Vec<TypeId>) = match &*node {
            Type::Basic(Basic::String | Basic::UntypedString) => return (Copyable, FINAL),
            Type::Invalid
            | Type::Basic(_)
            | Type::Pointer(_)
            | Type::Marker(_)
            | Type::Union(_) => {
                return (Trivial, FINAL);
            }
            Type::Shared(_)
            | Type::Weak(_)
            | Type::Chan { .. }
            | Type::Func { .. }
            | Type::Interface { .. }
            | Type::Local(_) => return (Copyable, FINAL),
            Type::Unique(elem) | Type::Slice(elem) => (Copyable, vec![*elem]),
            Type::Map { key, value } => (Copyable, vec![*key, *value]),
            Type::Array { elem, .. } | Type::Option(elem) => (Trivial, vec![*elem]),
            Type::Tuple(types) => (Trivial, types.to_vec()),
            Type::Struct(fields) => (Trivial, fields.iter().map(|field| field.ty).collect()),
            Type::Named { .. } | Type::Param(_) => (Trivial, Vec::new()),
        };
        if let Some(known) = self.read(|inner| inner.categories.get(&(ty, walk.owned)).copied()) {
            return (known, FINAL);
        }
        walk.steps += 1;
        let (category, outermost) = match &*node {
            Type::Named { id, args } => return self.declared_category(ty, *id, args, walk),
            Type::Param(_) => match self.param_set(ty) {
                Some(set) => self.set_category(&set, walk),
                None => {
                    walk.unknown = true;
                    (Category::NoCopy, FINAL)
                }
            },
            _ => {
                let (mut category, mut outermost) = (least, FINAL);
                for part in parts {
                    let (of_part, held) = deep(|| self.category_in(part, walk));
                    outermost = outermost.min(held);
                    category = category.max(of_part);
                }
                (category, outermost)
            }
        };
        match outermost {
            FINAL => self.keep(ty, category, walk),
            _ => walk.open.push(ty),
        }
        (category, outermost)
    }

    fn keep(&self, ty: TypeId, category: Category, walk: &Walk) {
        if !walk.unknown {
            self.write(|inner| inner.categories.insert((ty, walk.owned), category));
        }
    }

    fn declared_category(
        &self,
        ty: TypeId,
        named: NamedId,
        args: &[TypeId],
        walk: &mut Walk,
    ) -> (Category, usize) {
        let own = self.own_copy(ty, named, args);
        if let Some(own) = own.filter(|&own| !(walk.owned && own == NotCopied::Pinned)) {
            let category = match own {
                NotCopied::CopyMethod => Category::Copyable,
                _ => Category::NoCopy,
            };
            self.keep(ty, category, walk);
            return (category, FINAL);
        }
        // The instances of a type on an instantiation cycle have no end.
        if self.is_expanding(ty) {
            return (Category::Trivial, FINAL);
        }
        // A type that holds itself is as its other parts are.
        if let Some(&depth) = walk.depth.get(&ty) {
            return (Category::Trivial, depth);
        }
        let (depth, opened) = (walk.inside.len(), walk.open.len());
        walk.inside.push(ty);
        walk.depth.insert(ty, depth);
        let (mut category, mut outermost) = deep(|| self.category_in(self.underlying(ty), walk));
        // The parents are parts of the value.
        for parent in self.parents(ty) {
            let (of_parent, held) = deep(|| self.category_in(parent, walk));
            category = category.max(of_parent);
            outermost = outermost.min(held);
        }
        walk.inside.pop();
        walk.depth.remove(&ty);
        if outermost < depth {
            walk.open.push(ty);
            return (category, outermost);
        }
        // The outermost of the types that hold each other: they all hold
        // the same types, and are copied as this one is.
        self.keep(ty, category, walk);
        for held in walk.open.split_off(opened) {
            self.keep(held, category, walk);
        }
        (category, FINAL)
    }

    /// What the declared type itself says of its copy: it is pinned, or
    /// has a `drop` method, with or without the copy method. `None` if it
    /// is copied as what it is declared as.
    fn own_copy(&self, ty: TypeId, named: NamedId, args: &[TypeId]) -> Option<NotCopied> {
        let (pinned, drop, clone) = self.read(|inner| {
            let named = &inner.named[named.0 as usize];
            let clone = named.method_index.get(&(self.clone_name, None));
            let clone = clone.map(|&at| named.methods[at as usize].clone());
            (named.pinned, named.drops, clone)
        });
        if pinned {
            return Some(NotCopied::Pinned);
        }
        if !drop {
            return None;
        }
        // The copy method: `func (t T) Clone() T`.
        let copies = clone.is_some_and(|method| {
            let sig = self.method_of_instance(&method, args);
            !method.pointer_receiver
                && matches!(&*self.get(sig), Type::Func { type_params, params, results, variadic }
                    if type_params.is_empty() && params.is_empty() && !variadic && results[..] == [ty])
        });
        Some(match copies {
            true => NotCopied::CopyMethod,
            false => NotCopied::Drop,
        })
    }

    /// How a value of a type parameter with the type set `set` is copied.
    pub fn constraint_category(&self, set: &TypeSet) -> Category {
        let mut walk = Walk::default();
        let (category, _) = self.set_category(set, &mut walk);
        self.steps.fetch_add(walk.steps, Ordering::Relaxed);
        category
    }

    fn set_category(&self, set: &TypeSet, walk: &mut Walk) -> (Category, usize) {
        if set.is_empty() {
            return (Category::NoCopy, FINAL);
        }
        if set.markers.contains(&Marker::Copy) {
            return (Category::Trivial, FINAL);
        }
        // The types that the constraint lists, as they are written: a
        // term `~T` also holds declared types with a `drop` method, which
        // the type arguments are held to (GEN-1).
        let (mut listed, mut outermost) = (None, FINAL);
        for term in set.terms.iter().flatten() {
            let (of_term, held) = deep(|| self.category_in(term.ty, walk));
            outermost = outermost.min(held);
            listed = Some(listed.map_or(of_term, |all: Category| all.max(of_term)));
        }
        let category = match listed {
            Some(listed) if listed != Category::NoCopy => listed,
            _ if set.markers.contains(&Marker::Clone) => Category::Copyable,
            _ => Category::NoCopy,
        };
        (category, outermost)
    }

    /// Why a value of the type `ty` is not copied by its bytes, or not
    /// copied at all if `at_all`: the part that says so, and what it is.
    /// `None` if the type is copied that way.
    pub fn not_copied(&self, ty: TypeId, at_all: bool) -> Option<(TypeId, NotCopied)> {
        let wanted = match at_all {
            true => Category::Copyable,
            false => Category::Trivial,
        };
        let mut seen = FxHashSet::default();
        let mut current = ty;
        // Each step goes to a part that is not copied that way either.
        loop {
            if self.category(current) <= wanted || !seen.insert(current) {
                return None;
            }
            let fails = |part: &TypeId| self.category(*part) > wanted;
            let node = self.get(current);
            let part = match &*node {
                Type::Named { id, args } => match self.own_copy(current, *id, args) {
                    Some(own) => return Some((current, own)),
                    None => Some(self.underlying(current)),
                },
                Type::Param(_) => return Some((current, NotCopied::Param)),
                Type::Unique(elem) | Type::Slice(elem) if at_all => Some(*elem),
                Type::Map { key, value } if at_all => [*key, *value].into_iter().find(fails),
                Type::Array { elem, .. } | Type::Option(elem) => Some(*elem),
                Type::Tuple(types) => types.iter().copied().find(fails),
                Type::Struct(fields) => fields.iter().map(|field| field.ty).find(fails),
                _ => None,
            };
            match part {
                Some(part) => current = part,
                None => return Some((current, NotCopied::OwnRule)),
            }
        }
    }
}
