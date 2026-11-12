//! The two labels of every type: whether a value may be given to another
//! goroutine (`Send`), and whether several goroutines may read one value
//! at the same time (`Sync`) (CON-12, CON-19).

use std::sync::atomic::Ordering;

use rustc_hash::{FxHashMap, FxHashSet};

use super::{Marker, Type, TypeId, Types, deep};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Labels {
    pub send: bool,
    pub sync: bool,
}

impl Labels {
    const BOTH: Self = Self {
        send: true,
        sync: true,
    };
    const NEITHER: Self = Self {
        send: false,
        sync: false,
    };

    fn and(self, other: Self) -> Self {
        Self {
            send: self.send && other.send,
            sync: self.sync && other.sync,
        }
    }

    pub fn has(self, label: Marker) -> bool {
        match label {
            Marker::Send => self.send,
            Marker::Sync => self.sync,
            _ => true,
        }
    }
}

/// Why a type lacks a label.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unlabeled {
    /// A pointer borrows what it points to.
    Borrow,
    /// `Local[T]` stays with one goroutine.
    Local,
    /// A `Shared` or a `Weak` of what is not `Sync`.
    SharedUnsync,
    /// An interface that does not embed `Sync`.
    Interface,
    /// A type parameter whose constraint does not say.
    Param,
}

/// The types that a walk is inside of, with those whose labels wait for
/// the outermost of the types that hold each other.
#[derive(Default)]
struct Walk {
    /// Each declared type that the walk is inside of, with how many are
    /// around it.
    inside: FxHashMap<TypeId, usize>,
    waiting: Vec<(TypeId, usize)>,
    steps: u64,
}

impl Types {
    /// The labels of the type.
    pub fn labels(&self, ty: TypeId) -> Labels {
        let mut walk = Walk::default();
        let (labels, _) = self.labels_in(ty, &mut walk);
        self.steps.fetch_add(walk.steps, Ordering::Relaxed);
        labels
    }

    /// The labels of `ty`, and the depth of the outermost type that the
    /// walk is inside of and that `ty` holds: types that hold each other
    /// have the labels of all of them, known when the walk leaves the
    /// outermost.
    fn labels_in(&self, ty: TypeId, walk: &mut Walk) -> (Labels, usize) {
        let node = self.get(ty);
        let parts: Vec<TypeId> = match &*node {
            Type::Pointer(_) | Type::Local(_) => return (Labels::NEITHER, usize::MAX),
            Type::Chan { .. } | Type::Func { .. } => return (Labels::BOTH, usize::MAX),
            Type::Interface { .. } => {
                let sync = ty == self.error
                    || self
                        .type_set(ty)
                        .is_some_and(|set| set.markers.contains(&Marker::Sync));
                return (Labels { send: true, sync }, usize::MAX);
            }
            Type::Array { elem, .. }
            | Type::Slice(elem)
            | Type::Unique(elem)
            | Type::Option(elem)
            | Type::Shared(elem)
            | Type::Weak(elem) => vec![*elem],
            Type::Map { key, value } => vec![*key, *value],
            Type::Tuple(types) => types.to_vec(),
            Type::Struct(fields) => fields.iter().map(|field| field.ty).collect(),
            Type::Named { .. } | Type::Param(_) => Vec::new(),
            _ => return (Labels::BOTH, usize::MAX),
        };
        if let Some(known) = self.read(|inner| inner.labels.get(&ty).copied()) {
            return (known, usize::MAX);
        }
        if let Some(&depth) = walk.inside.get(&ty) {
            return (Labels::BOTH, depth);
        }
        walk.steps += 1;
        let depth = walk.inside.len();
        let waiting = walk.waiting.len();
        let mut outermost = usize::MAX;
        let mut labels = Labels::BOTH;
        match &*node {
            // A type parameter has the labels that its constraint names,
            // or that every type it lists has.
            Type::Param(_) => {
                let set = self.param_set(ty);
                let listed = self.specific_types(ty).filter(|types| !types.is_empty());
                for label in [Marker::Send, Marker::Sync] {
                    let named = set.as_ref().is_some_and(|set| set.markers.contains(&label));
                    let all = listed.as_ref().is_some_and(|types| {
                        types
                            .iter()
                            .all(|&(listed, _)| self.labels(listed).has(label))
                    });
                    match label {
                        Marker::Send => labels.send = named || all,
                        _ => labels.sync = named || all,
                    }
                }
            }
            // `error` embeds `Sync`.
            Type::Named { .. } if ty == self.error => {}
            Type::Named { .. } => {
                walk.inside.insert(ty, depth);
                let under = self.underlying(ty);
                let (of, held) = deep(|| self.labels_in(under, walk));
                walk.inside.remove(&ty);
                labels = of;
                outermost = held;
            }
            _ => {
                for part in parts {
                    let (of, held) = deep(|| self.labels_in(part, walk));
                    labels = labels.and(of);
                    outermost = outermost.min(held);
                }
                // A `Shared` or a `Weak` is given and shared where what
                // it holds may be shared.
                if matches!(&*node, Type::Shared(_) | Type::Weak(_)) {
                    labels = Labels {
                        send: labels.sync,
                        sync: labels.sync,
                    };
                }
            }
        }
        if outermost < depth {
            walk.waiting.push((ty, outermost));
            return (labels, outermost);
        }
        // Nothing around holds this type: its labels are final, and those
        // of the types that waited for it are its own.
        let done: Vec<TypeId> = walk
            .waiting
            .drain(waiting..)
            .map(|(waited, _)| waited)
            .collect();
        self.write(|inner| {
            inner.labels.insert(ty, labels);
            for waited in done {
                inner.labels.insert(waited, labels);
            }
        });
        (labels, usize::MAX)
    }

    /// The part of the type that lacks the label, with why; `None` if
    /// the type has it.
    pub fn unlabeled(&self, ty: TypeId, label: Marker) -> Option<(TypeId, Unlabeled)> {
        self.unlabeled_in(ty, label, &mut FxHashSet::default())
    }

    fn unlabeled_in(
        &self,
        ty: TypeId,
        label: Marker,
        seen: &mut FxHashSet<TypeId>,
    ) -> Option<(TypeId, Unlabeled)> {
        if self.labels(ty).has(label) || !seen.insert(ty) {
            return None;
        }
        let node = self.get(ty);
        let parts: Vec<TypeId> = match &*node {
            Type::Pointer(_) => return Some((ty, Unlabeled::Borrow)),
            Type::Local(_) => return Some((ty, Unlabeled::Local)),
            Type::Interface { .. } => return Some((ty, Unlabeled::Interface)),
            Type::Param(_) => return Some((ty, Unlabeled::Param)),
            Type::Shared(_) | Type::Weak(_) => return Some((ty, Unlabeled::SharedUnsync)),
            // An interface, a pointer and a `Local` are named as they are
            // declared.
            Type::Named { .. } => {
                let under = self.underlying(ty);
                let why = match &*self.get(under) {
                    Type::Pointer(_) => Some(Unlabeled::Borrow),
                    Type::Local(_) => Some(Unlabeled::Local),
                    Type::Interface { .. } => Some(Unlabeled::Interface),
                    _ => None,
                };
                if let Some(why) = why {
                    return Some((ty, why));
                }
                vec![under]
            }
            Type::Array { elem, .. }
            | Type::Slice(elem)
            | Type::Unique(elem)
            | Type::Option(elem) => vec![*elem],
            Type::Map { key, value } => vec![*key, *value],
            Type::Tuple(types) => types.to_vec(),
            Type::Struct(fields) => fields.iter().map(|field| field.ty).collect(),
            _ => return None,
        };
        // The first part that says why; one that only leads back here
        // says nothing.
        parts
            .into_iter()
            .find_map(|part| deep(|| self.unlabeled_in(part, label, seen)))
    }
}
