//! Which types have no zero value: those that are never nil, a struct
//! with a field that must be set, and what holds one of them by value
//! (OWN-R9, ADR-0016).

use std::sync::atomic::Ordering;

use gopp_span::Dialect;
use lasso::{Spur, ThreadedRodeo};

use super::{Type, TypeId, Types, deep};

/// Why a type has no zero value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NoZero {
    /// A pointer, a `Unique`, a `Shared`, a `Weak`, a func and an
    /// interface with methods are never nil.
    NeverNil,
    /// A struct with the field of this name, which is `@Required`.
    Required(Spur),
    /// A type parameter that may stand for a type without one.
    Param,
}

impl Types {
    /// The part of the type that has no zero value, with why: the type
    /// itself, or what it holds by value in a field or an element. `None`
    /// for a type that has a zero value.
    pub fn no_zero(&self, ty: TypeId, names: &ThreadedRodeo) -> Option<(TypeId, NoZero)> {
        let mut steps = 0;
        let found = self.no_zero_in(ty, names, &mut steps);
        self.steps.fetch_add(steps, Ordering::Relaxed);
        found
    }

    fn no_zero_in(
        &self,
        ty: TypeId,
        names: &ThreadedRodeo,
        steps: &mut u64,
    ) -> Option<(TypeId, NoZero)> {
        let node = self.get(ty);
        // What has no parts is said at once.
        let parts: Vec<TypeId> = match &*node {
            Type::Pointer(_)
            | Type::Unique(_)
            | Type::Shared(_)
            | Type::Weak(_)
            | Type::Func { .. } => return Some((ty, NoZero::NeverNil)),
            Type::Interface { .. } => {
                let never_nil = !self.has_nil(ty, Dialect::Gopp);
                return never_nil.then_some((ty, NoZero::NeverNil));
            }
            Type::Array { len: 0, .. } => return None,
            Type::Array { elem, .. } => vec![*elem],
            Type::Struct(fields) => {
                if let Some(required) = fields.iter().find(|field| field.required) {
                    return Some((ty, NoZero::Required(required.name)));
                }
                fields.iter().map(|field| field.ty).collect()
            }
            Type::Named { .. } | Type::Param(_) => Vec::new(),
            _ => return None,
        };
        // Nothing is kept of a type that is not worked out yet.
        if self.underlying(ty) == Self::INVALID {
            return None;
        }
        if let Some(known) = self.read(|inner| inner.no_zero.get(&ty).copied()) {
            return known;
        }
        *steps += 1;
        let found = match &*node {
            Type::Named { .. } if self.is_unsafe_pointer(ty, names) => None,
            // A declared type has the zero value of what it is declared
            // as; a struct that must be set is named by its own name.
            Type::Named { .. } => {
                let under = self.underlying(ty);
                // Whether an interface has nil, its name says: `error`.
                let found = match &*self.get(under) {
                    Type::Interface { .. } => {
                        let never_nil = !self.has_nil(ty, Dialect::Gopp);
                        never_nil.then_some((under, NoZero::NeverNil))
                    }
                    _ => deep(|| self.no_zero_in(under, names, steps)),
                };
                found.map(|(part, why)| match part == under {
                    true => (ty, why),
                    false => (part, why),
                })
            }
            // A type parameter has one if every type that it may be has.
            Type::Param(_) => {
                let listed = self.specific_types(ty).filter(|types| !types.is_empty());
                let all = listed.is_some_and(|types| {
                    types
                        .iter()
                        .all(|&(listed, _)| self.no_zero_in(listed, names, steps).is_none())
                });
                (!all).then_some((ty, NoZero::Param))
            }
            _ => parts
                .into_iter()
                .find_map(|part| deep(|| self.no_zero_in(part, names, steps))),
        };
        self.write(|inner| inner.no_zero.insert(ty, found));
        found
    }
}
