//! Which types a constant may have in a Go++ file: what is made of
//! numbers, strings and booleans by value, with nothing that runs when a
//! value ends (EXP-31). It is worked out once per type and kept.

use std::sync::atomic::Ordering;

use lasso::ThreadedRodeo;

use super::{Type, TypeId, Types, deep};

/// Why a type is not the type of a constant.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotConstant {
    /// A pointer, a `Unique`, a `Shared` or a `Weak`: it names one value
    /// of the running program.
    Handle,
    /// A slice, a map or a channel: its buffer is made while the program
    /// runs.
    Buffer,
    Func,
    Interface,
    /// A declared type that is `@Pinned`.
    Pinned,
    /// A declared type with a `drop` method.
    Drop,
    /// A type with a type table: its values say what they are while the
    /// program runs.
    Table,
    /// A type parameter: what it stands for is not known.
    Param,
    /// `unsafe.Pointer`, or `Local`.
    Other,
}

impl Types {
    /// The part of the type that keeps it from being the type of a
    /// constant, with why: the type itself, or what it holds by value.
    /// `None` for a type that a constant may have.
    pub fn not_constant(&self, ty: TypeId, names: &ThreadedRodeo) -> Option<(TypeId, NotConstant)> {
        let mut steps = 0;
        let found = self.not_constant_in(ty, names, &mut steps);
        self.steps.fetch_add(steps, Ordering::Relaxed);
        found
    }

    fn not_constant_in(
        &self,
        ty: TypeId,
        names: &ThreadedRodeo,
        steps: &mut u64,
    ) -> Option<(TypeId, NotConstant)> {
        let node = self.get(ty);
        let no = |why: NotConstant| Some((ty, why));
        let parts: Vec<TypeId> = match &*node {
            Type::Basic(_) => return None,
            Type::Pointer(_) | Type::Unique(_) | Type::Shared(_) | Type::Weak(_) => {
                return no(NotConstant::Handle);
            }
            Type::Slice(_) | Type::Map { .. } | Type::Chan { .. } => {
                return no(NotConstant::Buffer);
            }
            Type::Func { .. } => return no(NotConstant::Func),
            Type::Interface { .. } => return no(NotConstant::Interface),
            Type::Param(_) => return no(NotConstant::Param),
            Type::Array { elem, .. } | Type::Option(elem) => vec![*elem],
            Type::Struct(fields) => fields.iter().map(|field| field.ty).collect(),
            Type::Named { .. } => Vec::new(),
            _ => return no(NotConstant::Other),
        };
        // Nothing is kept of a type that is not worked out yet.
        if self.underlying(ty) == Self::INVALID {
            return None;
        }
        if let Some(known) = self.read(|inner| inner.not_constant.get(&ty).copied()) {
            return known;
        }
        *steps += 1;
        let found = match &*node {
            Type::Named { id, .. } => {
                let (pinned, drops) = self.read(|inner| {
                    let named = &inner.named[id.0 as usize];
                    (named.pinned, named.drops)
                });
                if pinned {
                    no(NotConstant::Pinned)
                } else if drops {
                    no(NotConstant::Drop)
                } else if self.has_table(ty) {
                    no(NotConstant::Table)
                } else if self.is_unsafe_pointer(ty, names) {
                    no(NotConstant::Other)
                } else {
                    // The parents are parts of the value.
                    let mut parts = vec![self.underlying(ty)];
                    parts.extend(self.parents(ty));
                    parts
                        .into_iter()
                        .find_map(|part| deep(|| self.not_constant_in(part, names, steps)))
                }
            }
            _ => parts
                .into_iter()
                .find_map(|part| deep(|| self.not_constant_in(part, names, steps))),
        };
        self.write(|inner| inner.not_constant.insert(ty, found));
        found
    }
}
