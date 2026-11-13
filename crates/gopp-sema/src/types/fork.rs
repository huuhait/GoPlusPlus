//! A copy of the type table that one worker has to itself, and how the
//! types that the worker made come back into the table. Workers that
//! share nothing wait for nothing; what they made is taken in the order
//! of the source, so the table is the same for any number of workers.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, RwLock};

use rustc_hash::FxHashMap;

use super::{ALIAS, DeclaredMethod, Inner, Method, Named, NamedId, ParamId, Type, TypeId, Types};
use super::{Spellings, deep};

/// How much of a table there was when a copy of it was made.
#[derive(Clone, Copy, Debug, Default)]
pub(super) struct Sizes {
    types: u32,
    aliases: u32,
    named: u32,
}

/// What of a copy is in the table already.
#[derive(Default)]
pub struct Imported {
    /// By where a type is in the copy, as it is written.
    types: FxHashMap<u32, TypeId>,
    aliases: FxHashMap<u32, TypeId>,
    named: FxHashMap<u32, NamedId>,
}

impl Inner {
    /// A copy that shares no counted pointer with this table.
    fn private_copy(&self) -> Inner {
        let types: Vec<Arc<Type>> = self
            .types
            .iter()
            .map(|ty| Arc::new((**ty).clone()))
            .collect();
        let ids = self.ids.values().map(|spellings| {
            let key = Arc::clone(&types[spellings.canonical as usize]);
            let spellings = Spellings {
                canonical: spellings.canonical,
                written: spellings.written.clone(),
            };
            (key, spellings)
        });
        let open_abstracts = self.open_abstracts.iter();
        Inner {
            ids: ids.collect(),
            types,
            canon: self.canon.clone(),
            aliases: self.aliases.clone(),
            alias_ids: self.alias_ids.clone(),
            has_param: self.has_param.clone(),
            named: self.named.clone(),
            params: self.params.clone(),
            param_origins: self.param_origins.clone(),
            element_params: self.element_params.clone(),
            method_receivers: self.method_receivers.clone(),
            instances: self.instances.clone(),
            type_sets: self.type_sets.clone(),
            categories: self.categories.clone(),
            tables: self.tables.clone(),
            abstracts: self.abstracts.clone(),
            holding: self.holding.clone(),
            kept: self.kept.clone(),
            lends: self.lends.clone(),
            open_abstracts: open_abstracts
                .map(|(&ty, open)| (ty, Arc::new((**open).clone())))
                .collect(),
            labels: self.labels.clone(),
            optional: self.optional.clone(),
            held: self.held.clone(),
            required: self.required.clone(),
            no_zero: self.no_zero.clone(),
            not_constant: self.not_constant.clone(),
        }
    }
}

impl Types {
    /// A copy of the table for one worker.
    pub fn fork(&self) -> Types {
        let (inner, forked) = self.read(|inner| {
            let sizes = Sizes {
                types: inner.types.len() as u32,
                aliases: inner.aliases.len() as u32,
                named: inner.named.len() as u32,
            };
            (inner.private_copy(), sizes)
        });
        Types {
            inner: RwLock::new(inner),
            error: self.error,
            byte: self.byte,
            rune: self.rune,
            any: self.any,
            clone_name: self.clone_name,
            steps: AtomicU64::new(0),
            forked,
        }
    }

    /// How many types the table has.
    pub fn count(&self) -> usize {
        self.read(|inner| inner.types.len())
    }

    /// Counts the work that a copy of the table did.
    pub fn add_steps(&self, steps: u64) {
        self.steps.fetch_add(steps, Ordering::Relaxed);
    }

    /// The type `id` of the copy `from` of this table, in this table.
    /// What the copy made is made here when it is first asked for.
    pub fn import(&self, from: &Types, kept: &mut Imported, id: TypeId) -> TypeId {
        let base = from.forked;
        if id.written & ALIAS != 0 {
            let index = id.written ^ ALIAS;
            if index < base.aliases {
                return id;
            }
            if let Some(&known) = kept.aliases.get(&index) {
                return known;
            }
            let (named, args, target) = from.read(|inner| {
                let alias = &inner.aliases[index as usize];
                (alias.named, alias.args.to_vec(), alias.target)
            });
            let named = self.import_named(from, kept, named);
            let args: Vec<TypeId> = args
                .into_iter()
                .map(|arg| self.import(from, kept, arg))
                .collect();
            let target = self.import(from, kept, target);
            let made = self.write(|inner| inner.alias(named, &args, target));
            kept.aliases.insert(index, made);
            return made;
        }
        if id.written < base.types {
            return id;
        }
        if let Some(&known) = kept.types.get(&id.written) {
            return known;
        }
        let node = from.read(|inner| Arc::clone(&inner.types[id.written as usize]));
        let made = match &*node {
            Type::Param(param) => self.import_param(from, kept, id, *param),
            Type::Named { id: named, args } => {
                let named = self.import_named(from, kept, *named);
                let args = args.iter().map(|&arg| self.import(from, kept, arg));
                self.intern(Type::Named {
                    id: named,
                    args: args.collect(),
                })
            }
            other => {
                let mapped = other.map_parts(&mut |part| deep(|| self.import(from, kept, part)));
                self.intern(mapped)
            }
        };
        kept.types.insert(id.written, made);
        made
    }

    /// A type parameter that the copy made, with what the copy says of
    /// it.
    fn import_param(
        &self,
        from: &Types,
        kept: &mut Imported,
        id: TypeId,
        param: ParamId,
    ) -> TypeId {
        let (name, constraint) = from.read(|inner| inner.params[param.0 as usize]);
        let made = self.param(name);
        // Its constraint may name it.
        kept.types.insert(id.written, made);
        if let Some(constraint) = constraint {
            let constraint = self.import(from, kept, constraint);
            self.set_constraint(made, constraint);
        }
        let (origin, elements, receiver) = from.read(|inner| {
            (
                inner.param_origins.get(&id).copied(),
                inner.element_params.contains(&id),
                inner.method_receivers.get(&id).cloned(),
            )
        });
        let origin = origin.map(|origin| self.import(from, kept, origin));
        let receiver = receiver.map(|(params, args)| {
            let mut all = |types: Vec<TypeId>| -> Vec<TypeId> {
                types
                    .into_iter()
                    .map(|ty| self.import(from, kept, ty))
                    .collect()
            };
            (all(params), all(args))
        });
        self.write(|inner| {
            if let Some(origin) = origin {
                inner.param_origins.insert(made, origin);
            }
            if elements {
                inner.element_params.insert(made);
            }
            if let Some(receiver) = receiver {
                inner.method_receivers.insert(made, receiver);
            }
        });
        made
    }

    /// The declared type `named` of the copy `from`, in this table.
    pub fn import_named(&self, from: &Types, kept: &mut Imported, named: NamedId) -> NamedId {
        if named.0 < from.forked.named {
            return named;
        }
        if let Some(&known) = kept.named.get(&named.0) {
            return known;
        }
        let record = from.read(|inner| inner.named[named.0 as usize].clone());
        let made = self.write(|inner| {
            inner.named.push(Named {
                name: record.name,
                package: record.package,
                path: record.path,
                local: record.local,
                underlying: None,
                declared_as: None,
                invalid: record.invalid,
                params: Vec::new(),
                methods: Vec::new(),
                method_index: record.method_index.clone(),
                operators: Vec::new(),
                extends: record.extends,
                parents: Vec::new(),
                abstract_marked: record.abstract_marked,
                expanding: record.expanding,
                pinned: record.pinned,
                drops: record.drops,
                alias: None,
            });
            NamedId(inner.named.len() as u32 - 1)
        });
        // The type may name itself.
        kept.named.insert(named.0, made);
        let mut one = |ty: TypeId| deep(|| self.import(from, kept, ty));
        let underlying = record.underlying.map(&mut one);
        let declared_as = record.declared_as.map(&mut one);
        let alias = record.alias.map(&mut one);
        let params: Vec<TypeId> = record.params.iter().map(|&ty| one(ty)).collect();
        let parents: Vec<TypeId> = record.parents.iter().map(|&ty| one(ty)).collect();
        let mut method = |declared: &DeclaredMethod| DeclaredMethod {
            method: Method {
                sig: one(declared.method.sig),
                ..declared.method.clone()
            },
            receiver_params: declared.receiver_params.iter().map(|&ty| one(ty)).collect(),
            ..declared.clone()
        };
        let methods: Vec<DeclaredMethod> = record.methods.iter().map(&mut method).collect();
        let operators: Vec<DeclaredMethod> = record.operators.iter().map(&mut method).collect();
        self.write(|inner| {
            let here = &mut inner.named[made.0 as usize];
            here.underlying = underlying;
            here.declared_as = declared_as;
            here.alias = alias;
            here.params = params;
            here.parents = parents;
            here.methods = methods;
            here.operators = operators;
        });
        made
    }
}
