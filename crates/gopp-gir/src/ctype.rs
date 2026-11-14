//! Which types cross to C (c-abi.md): a C function takes and gives
//! numbers, pointers, and structs of them. What is kept of a type is
//! what the type is by itself, never where a question started, so an
//! answer does not depend on what was asked before.

use gopp_target::{Layout, TargetLayout};
use rustc_hash::{FxHashMap, FxHashSet};

use crate::ir::CallConv;
use crate::types::{FuncType, Type, TypeId, TypeInterner};

/// How deep structs and arrays may be nested in a value that crosses.
pub const MAX_DEPTH: u32 = 64;
/// How many fields a value that crosses may have: a struct counts
/// itself and each field, an array itself and its element once.
pub const MAX_FIELDS: u32 = 4096;
/// How many bytes a value that crosses may have.
pub const MAX_BYTES: u64 = 1 << 30;

/// Why a type does not cross to C.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CRefusal {
    Depth,
    /// The first part that C has no type for.
    NotC(TypeId),
    /// An array that is no part of a struct: C would pass a pointer.
    BareArray,
    Fields,
    Bytes,
}

impl CRefusal {
    pub fn describe(self, types: &TypeInterner) -> String {
        match self {
            Self::Depth => format!("its structs and arrays are nested more than {MAX_DEPTH} deep"),
            Self::NotC(part) => format!("C has no type for {}", types.display(part)),
            Self::BareArray => "C passes an array only inside a struct".to_owned(),
            Self::Fields => format!("it has more than {MAX_FIELDS} fields"),
            Self::Bytes => "it is larger than 1 GiB".to_owned(),
        }
    }
}

/// What a type is by itself.
#[derive(Clone, Copy)]
struct Facts {
    /// How deep structs and arrays are nested in it.
    height: u32,
    /// Counted up to one more than the limit.
    fields: u32,
    not_c: Option<TypeId>,
    /// A value of it takes no room.
    empty: bool,
}

/// The rules, with what is known of the types that were asked for.
pub struct CTypeRules {
    /// The target, for the limit of bytes; without one that limit is
    /// not checked.
    target: Option<TargetLayout>,
    facts: FxHashMap<TypeId, Facts>,
    layouts: FxHashMap<TypeId, Layout>,
    pub steps: u64,
}

impl CTypeRules {
    pub fn new(target: Option<TargetLayout>) -> Self {
        Self {
            target,
            facts: FxHashMap::default(),
            layouts: FxHashMap::default(),
            steps: 0,
        }
    }

    /// Whether a C function takes a value of the type.
    pub fn param(&mut self, types: &TypeInterner, ty: TypeId) -> Result<(), CRefusal> {
        let facts = self.facts(types, ty);
        self.whole(types, ty, facts)
    }

    /// Whether a C function gives a value of the type; it may give
    /// nothing.
    pub fn result(&mut self, types: &TypeInterner, ty: TypeId) -> Result<(), CRefusal> {
        match ty == TypeInterner::UNIT {
            true => Ok(()),
            false => self.param(types, ty),
        }
    }

    /// Whether a function of the type is one of C: `Err` has the number
    /// of the parameter, or none for the result.
    pub fn signature(
        &mut self,
        types: &TypeInterner,
        func: &FuncType,
    ) -> Result<(), (Option<usize>, CRefusal)> {
        for (at, &param) in func.params.iter().enumerate() {
            self.param(types, param).map_err(|why| (Some(at), why))?;
        }
        self.result(types, func.ret).map_err(|why| (None, why))
    }

    /// The parts of a type that say what it is.
    fn parts(types: &TypeInterner, ty: TypeId) -> Vec<TypeId> {
        match types.get(ty) {
            Type::Array(of, _) => vec![*of],
            Type::Struct(id) => types.struct_fields(*id).unwrap_or_default().to_vec(),
            Type::Func(func) if Self::of_c(func) => {
                let given = Some(func.ret).filter(|&ret| ret != TypeInterner::UNIT);
                func.params.iter().copied().chain(given).collect()
            }
            _ => Vec::new(),
        }
    }

    fn of_c(func: &FuncType) -> bool {
        func.cc == CallConv::C && !func.has_env
    }

    /// The verdict on a value of the type that is a parameter or a
    /// result by itself, from what is known of it: the depth first,
    /// then the first part that is no type of C, then the count of the
    /// fields, then the bytes.
    fn whole(&mut self, types: &TypeInterner, ty: TypeId, facts: Facts) -> Result<(), CRefusal> {
        if facts.height > MAX_DEPTH {
            return Err(CRefusal::Depth);
        }
        if let Some(part) = facts.not_c {
            return Err(CRefusal::NotC(part));
        }
        if matches!(types.get(ty), Type::Array(..)) {
            return Err(CRefusal::BareArray);
        }
        if facts.fields > MAX_FIELDS {
            return Err(CRefusal::Fields);
        }
        let Some(target) = self.target else {
            return Ok(());
        };
        match types.layout_with(ty, target, &mut self.layouts) {
            Ok(layout) if layout.size <= MAX_BYTES => Ok(()),
            _ => Err(CRefusal::Bytes),
        }
    }

    /// What the type is by itself. The walk keeps what it has to do in
    /// a list, and a type is worked out once.
    fn facts(&mut self, types: &TypeInterner, ty: TypeId) -> Facts {
        let refused = |part: TypeId, empty: bool| Facts {
            height: 0,
            fields: 1,
            not_c: Some(part),
            empty,
        };
        let leaf = Facts {
            height: 0,
            fields: 1,
            not_c: None,
            empty: false,
        };
        // A type is taken twice: to ask for its parts, and with them.
        let mut rest = vec![(ty, false)];
        let mut inside: FxHashSet<TypeId> = FxHashSet::default();
        while let Some((next, with_parts)) = rest.pop() {
            self.steps += 1;
            if self.facts.contains_key(&next) {
                continue;
            }
            let parts = Self::parts(types, next);
            if !with_parts && !parts.is_empty() {
                // A value that holds a value of its own type is none.
                if !inside.insert(next) {
                    self.facts.insert(next, refused(next, false));
                    continue;
                }
                rest.push((next, true));
                rest.extend(parts.iter().map(|&part| (part, false)));
                continue;
            }
            inside.remove(&next);
            self.steps += parts.len() as u64;
            // Each part is known by now.
            let of = |part: &TypeId| {
                self.facts
                    .get(part)
                    .copied()
                    .unwrap_or(refused(*part, false))
            };
            let known: Vec<Facts> = parts.iter().map(of).collect();
            let facts = match types.get(next) {
                Type::Bool | Type::Int { .. } | Type::Float { .. } | Type::Ptr(_) => leaf,
                Type::Array(_, count) => match known.first() {
                    // An array of what takes no room has no one layout
                    // in C.
                    Some(elem) => Facts {
                        height: elem.height.saturating_add(1),
                        fields: elem.fields.saturating_add(1).min(MAX_FIELDS + 1),
                        not_c: elem.not_c.or(elem.empty.then_some(next)),
                        empty: *count == 0,
                    },
                    None => refused(next, false),
                },
                // A struct without fields has no room here and four
                // bytes in the C of Windows: it does not cross.
                Type::Struct(_) if known.iter().all(|field| field.empty) => refused(next, true),
                Type::Struct(_) => {
                    let count = |sum: u32, field: &Facts| sum.saturating_add(field.fields);
                    let height = known.iter().map(|field| field.height).max().unwrap_or(0);
                    Facts {
                        height: height.saturating_add(1),
                        fields: known.iter().fold(1, count).min(MAX_FIELDS + 1),
                        not_c: known.iter().find_map(|field| field.not_c),
                        empty: false,
                    }
                }
                Type::Func(func) if Self::of_c(func) => {
                    let mut crosses = true;
                    for (&part, &facts) in parts.iter().zip(&known) {
                        crosses &= self.whole(types, part, facts).is_ok();
                    }
                    match crosses {
                        true => leaf,
                        false => refused(next, false),
                    }
                }
                other => refused(next, matches!(other, Type::Unit)),
            };
            self.facts.insert(next, facts);
        }
        self.facts.get(&ty).copied().unwrap_or(refused(ty, false))
    }
}

#[cfg(test)]
mod tests;
