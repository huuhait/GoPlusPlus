//! Types and the relations between them.
//!
//! Every type is interned: building the same type twice gives the same
//! [`TypeId`], so two ids that are equal are identical types. The reverse
//! holds for all types but interfaces, which may be written in several ways
//! (`interface{ io.Reader }` and `interface{ Read(*[]byte) (int, error) }`);
//! [`Types::identical`] decides those.
//!
//! A function that can fail (`func!(int) T`) has no type of its own: it is
//! `func(int) (T, error)`.

use std::sync::{Arc, PoisonError, RwLock};

use gopp_span::Dialect;
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::FxHashMap;

use crate::basic::{Basic, representable};
use crate::constant::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TypeId(u32);

/// A declared type: an index into the table of the interner.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NamedId(u32);

/// A type parameter. Two parameters are never the same type, whatever
/// their names.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ParamId(u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ChanDir {
    Both,
    Send,
    Recv,
}

/// A constraint that the compiler knows and no program can write out.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Marker {
    Comparable,
    Copy,
    Clone,
    Send,
    Sync,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Field {
    pub name: Spur,
    /// The path of the declaring package for a name that is not exported:
    /// such names of two packages are different names.
    pub package: Option<Spur>,
    pub ty: TypeId,
    pub embedded: bool,
    pub tag: Option<Box<[u8]>>,
    /// `@Required`: must be set in every literal.
    pub required: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Method {
    pub name: Spur,
    /// As for [`Field::package`].
    pub package: Option<Spur>,
    /// A [`Type::Func`].
    pub sig: TypeId,
}

/// One term of a union in a constraint: `T` or `~T`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Term {
    pub tilde: bool,
    pub ty: TypeId,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Type {
    /// The type of what is in error. It fits everywhere, so that one
    /// mistake is reported once.
    Invalid,
    Basic(Basic),
    Pointer(TypeId),
    Unique(TypeId),
    Shared(TypeId),
    Weak(TypeId),
    Option(TypeId),
    /// `Local[T]`: a func or an interface that stays on one goroutine.
    Local(TypeId),
    Array {
        len: u64,
        elem: TypeId,
    },
    Slice(TypeId),
    Map {
        key: TypeId,
        value: TypeId,
    },
    Chan {
        dir: ChanDir,
        elem: TypeId,
    },
    /// Several values at once: the results of a call.
    Tuple(Box<[TypeId]>),
    Func {
        params: Box<[TypeId]>,
        results: Box<[TypeId]>,
        /// The last parameter is `...T`, held as `[]T`.
        variadic: bool,
    },
    Struct(Box<[Field]>),
    Interface {
        /// The methods written in the interface, in the order of their
        /// names.
        methods: Box<[Method]>,
        /// The embedded interfaces and the type terms of a constraint.
        embedded: Box<[TypeId]>,
    },
    Union(Box<[Term]>),
    Marker(Marker),
    /// A declared type, with its type arguments if it is generic.
    Named {
        id: NamedId,
        args: Box<[TypeId]>,
    },
    Param(ParamId),
}

/// A method of a declared type.
#[derive(Clone, Debug)]
pub struct DeclaredMethod {
    pub method: Method,
    /// Whether the receiver is a pointer.
    pub pointer_receiver: bool,
    /// The type parameters that the receiver names, `P` in
    /// `func (l *List[P]) Len() int`: the signature is written with them.
    pub receiver_params: Vec<TypeId>,
}

/// What a selector `x.name` reaches in the type of `x`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Member {
    Field {
        ty: TypeId,
    },
    Method {
        /// A [`Type::Func`], without the receiver.
        sig: TypeId,
        pointer_receiver: bool,
    },
}

#[derive(Debug)]
struct Named {
    name: Spur,
    /// The name of the declaring package; `None` for a predeclared type.
    package: Option<Spur>,
    /// Set once the declaration is checked.
    underlying: Option<TypeId>,
    /// The type parameters of a generic type.
    params: Vec<TypeId>,
    methods: Vec<DeclaredMethod>,
}

#[derive(Debug, Default)]
struct Inner {
    types: Vec<Arc<Type>>,
    ids: FxHashMap<Arc<Type>, TypeId>,
    named: Vec<Named>,
    params: Vec<Spur>,
    /// The underlying type of each generic type with arguments that was
    /// asked for.
    instances: FxHashMap<TypeId, TypeId>,
    /// The type parameters of each generic function, by its type.
    generic_funcs: FxHashMap<TypeId, Vec<TypeId>>,
}

impl Inner {
    fn intern(&mut self, ty: Type) -> TypeId {
        if let Some(&id) = self.ids.get(&ty) {
            return id;
        }
        let id = TypeId(self.types.len() as u32);
        let ty = Arc::new(ty);
        self.types.push(Arc::clone(&ty));
        self.ids.insert(ty, id);
        id
    }
}

/// Why a predeclared generic type cannot take a type argument.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BadArgument {
    /// `Unique[*T]`: a borrowed pointer may not live on the heap (OWN-B5).
    PointerOnHeap,
    /// `Local[int]`: only a func or an interface is tied to a goroutine.
    LocalOfValue,
}

pub(crate) const BASICS: [Basic; 24] = [
    Basic::Bool,
    Basic::Int,
    Basic::Int8,
    Basic::Int16,
    Basic::Int32,
    Basic::Int64,
    Basic::Uint,
    Basic::Uint8,
    Basic::Uint16,
    Basic::Uint32,
    Basic::Uint64,
    Basic::Uintptr,
    Basic::Float32,
    Basic::Float64,
    Basic::Complex64,
    Basic::Complex128,
    Basic::String,
    Basic::UntypedBool,
    Basic::UntypedInt,
    Basic::UntypedRune,
    Basic::UntypedFloat,
    Basic::UntypedComplex,
    Basic::UntypedString,
    Basic::UntypedNil,
];

/// The types of one compilation. Shared by all packages and threads.
pub struct Types {
    inner: RwLock<Inner>,
    error: TypeId,
}

impl Types {
    /// `names` is the interner of the compilation.
    pub fn new(names: &ThreadedRodeo) -> Self {
        let mut inner = Inner::default();
        inner.intern(Type::Invalid);
        for basic in BASICS {
            inner.intern(Type::Basic(basic));
        }
        // type error interface { Error() string }
        let string = TypeId(1 + Basic::String as u32);
        let sig = inner.intern(Type::Func {
            params: Box::new([]),
            results: Box::new([string]),
            variadic: false,
        });
        let method = Method {
            name: names.get_or_intern_static("Error"),
            package: None,
            sig,
        };
        let underlying = inner.intern(Type::Interface {
            methods: Box::new([method]),
            embedded: Box::new([]),
        });
        inner.named.push(Named {
            name: names.get_or_intern_static("error"),
            package: None,
            underlying: Some(underlying),
            params: Vec::new(),
            methods: Vec::new(),
        });
        let error = inner.intern(Type::Named {
            id: NamedId(0),
            args: Box::new([]),
        });
        Self {
            inner: RwLock::new(inner),
            error,
        }
    }

    fn read<R>(&self, f: impl FnOnce(&Inner) -> R) -> R {
        f(&self.inner.read().unwrap_or_else(PoisonError::into_inner))
    }

    fn write<R>(&self, f: impl FnOnce(&mut Inner) -> R) -> R {
        f(&mut self.inner.write().unwrap_or_else(PoisonError::into_inner))
    }

    pub fn intern(&self, ty: Type) -> TypeId {
        // Most types asked for exist already.
        if let Some(id) = self.read(|inner| inner.ids.get(&ty).copied()) {
            return id;
        }
        self.write(|inner| inner.intern(ty))
    }

    pub fn get(&self, id: TypeId) -> Arc<Type> {
        self.read(|inner| Arc::clone(&inner.types[id.0 as usize]))
    }

    pub const INVALID: TypeId = TypeId(0);

    pub fn basic(&self, basic: Basic) -> TypeId {
        TypeId(1 + basic as u32)
    }

    /// The predeclared type `error`.
    pub fn error(&self) -> TypeId {
        self.error
    }

    /// `interface{}`, which `any` names.
    pub fn any(&self) -> TypeId {
        self.intern(Type::Interface {
            methods: Box::new([]),
            embedded: Box::new([]),
        })
    }

    pub fn pointer(&self, elem: TypeId) -> TypeId {
        self.intern(Type::Pointer(elem))
    }

    pub fn slice(&self, elem: TypeId) -> TypeId {
        self.intern(Type::Slice(elem))
    }

    pub fn array(&self, len: u64, elem: TypeId) -> TypeId {
        self.intern(Type::Array { len, elem })
    }

    pub fn map(&self, key: TypeId, value: TypeId) -> TypeId {
        self.intern(Type::Map { key, value })
    }

    pub fn chan(&self, dir: ChanDir, elem: TypeId) -> TypeId {
        self.intern(Type::Chan { dir, elem })
    }

    pub fn option(&self, elem: TypeId) -> TypeId {
        self.intern(Type::Option(elem))
    }

    pub fn func(&self, params: &[TypeId], results: &[TypeId], variadic: bool) -> TypeId {
        self.intern(Type::Func {
            params: params.into(),
            results: results.into(),
            variadic,
        })
    }

    /// The type of a function that can fail: `func!(params) results` is
    /// `func(params) (results, error)`.
    pub fn failing_func(&self, params: &[TypeId], results: &[TypeId], variadic: bool) -> TypeId {
        let results: Vec<TypeId> = results.iter().copied().chain([self.error]).collect();
        self.func(params, &results, variadic)
    }

    pub fn strukt(&self, fields: Vec<Field>) -> TypeId {
        self.intern(Type::Struct(fields.into()))
    }

    /// An interface with the methods written in it and what it embeds, in
    /// any order.
    pub fn interface(
        &self,
        mut methods: Vec<Method>,
        mut embedded: Vec<TypeId>,
        names: &ThreadedRodeo,
    ) -> TypeId {
        // By the text of the names: the order of interning differs from
        // run to run.
        let key = |m: &Method| (names.resolve(&m.name), m.package.map(|p| names.resolve(&p)));
        methods.sort_by(|a, b| key(a).cmp(&key(b)));
        embedded.sort_unstable();
        embedded.dedup();
        self.intern(Type::Interface {
            methods: methods.into(),
            embedded: embedded.into(),
        })
    }

    /// `Unique[elem]`, `Shared[elem]`, `Weak[elem]` or `Local[elem]`, as
    /// `make` builds it from the argument, or why the argument is refused.
    pub fn handle(&self, make: fn(TypeId) -> Type, elem: TypeId) -> Result<TypeId, BadArgument> {
        let ty = make(elem);
        let under = self.get(self.underlying(elem));
        match (&ty, &*under) {
            (Type::Local(_), Type::Func { .. } | Type::Interface { .. } | Type::Invalid) => {}
            (Type::Local(_), _) => return Err(BadArgument::LocalOfValue),
            (_, Type::Pointer(_)) => return Err(BadArgument::PointerOnHeap),
            _ => {}
        }
        Ok(self.intern(ty))
    }

    /// Declares a type; its underlying type is set when its declaration is
    /// checked. `package` is the name of the declaring package.
    pub fn declare(&self, name: Spur, package: Spur) -> NamedId {
        self.write(|inner| {
            inner.named.push(Named {
                name,
                package: Some(package),
                underlying: None,
                params: Vec::new(),
                methods: Vec::new(),
            });
            NamedId(inner.named.len() as u32 - 1)
        })
    }

    pub fn set_underlying(&self, named: NamedId, underlying: TypeId) {
        // The underlying type of a declared type is never a declared type.
        let underlying = self.underlying(underlying);
        self.write(|inner| inner.named[named.0 as usize].underlying = Some(underlying));
    }

    /// Says that the function type `sig` is that of a generic function
    /// with the type parameters `params`.
    pub fn set_generic_func(&self, sig: TypeId, params: Vec<TypeId>) {
        self.write(|inner| inner.generic_funcs.insert(sig, params));
    }

    /// The type parameters of a generic function of the type `sig`.
    pub fn generic_func(&self, sig: TypeId) -> Option<Vec<TypeId>> {
        self.read(|inner| inner.generic_funcs.get(&sig).cloned())
    }

    /// Finds what the type parameters `params` must be for `pattern`,
    /// which is written with them, to be the type `actual`. `bound` holds
    /// what is found, one place for each parameter. False if the two do
    /// not fit, or a parameter would have to be two types.
    pub fn unify(
        &self,
        pattern: TypeId,
        actual: TypeId,
        params: &[TypeId],
        bound: &mut [Option<TypeId>],
    ) -> bool {
        if let Some(at) = params.iter().position(|&param| param == pattern) {
            return match bound[at] {
                Some(known) => known == actual,
                None => {
                    bound[at] = Some(actual);
                    true
                }
            };
        }
        if pattern == actual {
            return true;
        }
        match (&*self.get(pattern), &*self.get(actual)) {
            (Type::Pointer(a), Type::Pointer(b))
            | (Type::Slice(a), Type::Slice(b))
            | (Type::Unique(a), Type::Unique(b))
            | (Type::Shared(a), Type::Shared(b))
            | (Type::Weak(a), Type::Weak(b))
            | (Type::Option(a), Type::Option(b))
            | (Type::Local(a), Type::Local(b))
            | (Type::Chan { elem: a, .. }, Type::Chan { elem: b, .. }) => {
                self.unify(*a, *b, params, bound)
            }
            (Type::Array { len: m, elem: a }, Type::Array { len: n, elem: b }) => {
                m == n && self.unify(*a, *b, params, bound)
            }
            (Type::Map { key: k, value: v }, Type::Map { key: l, value: w }) => {
                self.unify(*k, *l, params, bound) && self.unify(*v, *w, params, bound)
            }
            (
                Type::Func {
                    params: p,
                    results: r,
                    variadic: v,
                },
                Type::Func {
                    params: q,
                    results: s,
                    variadic: w,
                },
            ) => {
                v == w && self.unify_all(p, q, params, bound) && self.unify_all(r, s, params, bound)
            }
            (Type::Named { id: a, args: x }, Type::Named { id: b, args: y }) => {
                a == b && self.unify_all(x, y, params, bound)
            }
            // A declared type fits a pattern that is none through what it
            // was declared as: `[]E` takes a `type List []int`.
            (pattern_type, Type::Named { .. }) if !matches!(pattern_type, Type::Named { .. }) => {
                self.unify(pattern, self.underlying(actual), params, bound)
            }
            _ => false,
        }
    }

    fn unify_all(
        &self,
        patterns: &[TypeId],
        actuals: &[TypeId],
        params: &[TypeId],
        bound: &mut [Option<TypeId>],
    ) -> bool {
        patterns.len() == actuals.len()
            && patterns
                .iter()
                .zip(actuals)
                .all(|(&a, &b)| self.unify(a, b, params, bound))
    }

    /// Says that `named` is generic, with the type parameters `params`.
    pub fn set_type_params(&self, named: NamedId, params: Vec<TypeId>) {
        self.write(|inner| inner.named[named.0 as usize].params = params);
    }

    /// `ty` with each type parameter of `from` replaced by the type at the
    /// same place of `to`.
    pub fn substitute(&self, ty: TypeId, from: &[TypeId], to: &[TypeId]) -> TypeId {
        if from.is_empty() {
            return ty;
        }
        let sub = |ty: TypeId| self.substitute(ty, from, to);
        let list =
            |types: &[TypeId]| -> Box<[TypeId]> { types.iter().map(|&ty| sub(ty)).collect() };
        let replaced = match &*self.get(ty) {
            Type::Param(_) => {
                let at = from.iter().position(|&param| param == ty);
                return at.and_then(|at| to.get(at).copied()).unwrap_or(ty);
            }
            Type::Invalid | Type::Basic(_) | Type::Marker(_) => return ty,
            Type::Pointer(elem) => Type::Pointer(sub(*elem)),
            Type::Unique(elem) => Type::Unique(sub(*elem)),
            Type::Shared(elem) => Type::Shared(sub(*elem)),
            Type::Weak(elem) => Type::Weak(sub(*elem)),
            Type::Option(elem) => Type::Option(sub(*elem)),
            Type::Local(elem) => Type::Local(sub(*elem)),
            Type::Slice(elem) => Type::Slice(sub(*elem)),
            Type::Array { len, elem } => Type::Array {
                len: *len,
                elem: sub(*elem),
            },
            Type::Map { key, value } => Type::Map {
                key: sub(*key),
                value: sub(*value),
            },
            Type::Chan { dir, elem } => Type::Chan {
                dir: *dir,
                elem: sub(*elem),
            },
            Type::Tuple(types) => Type::Tuple(list(types)),
            Type::Func {
                params,
                results,
                variadic,
            } => Type::Func {
                params: list(params),
                results: list(results),
                variadic: *variadic,
            },
            Type::Struct(fields) => {
                let field = |field: &Field| Field {
                    ty: sub(field.ty),
                    ..field.clone()
                };
                Type::Struct(fields.iter().map(field).collect())
            }
            Type::Interface { methods, embedded } => {
                let method = |method: &Method| Method {
                    sig: sub(method.sig),
                    ..method.clone()
                };
                Type::Interface {
                    methods: methods.iter().map(method).collect(),
                    embedded: list(embedded),
                }
            }
            Type::Union(terms) => {
                let term = |term: &Term| Term {
                    ty: sub(term.ty),
                    ..*term
                };
                Type::Union(terms.iter().map(term).collect())
            }
            Type::Named { id, args } => Type::Named {
                id: *id,
                args: list(args),
            },
        };
        self.intern(replaced)
    }

    /// What `name` reaches in a value of the type `ty`: a field or a
    /// method of the type, or one reached through embedded fields, the
    /// shallowest first. `None` if there is none, or two at one depth.
    /// `package` is the path of the package that asks: a name that is not
    /// exported is found only in its own package.
    pub fn member(
        &self,
        ty: TypeId,
        name: Spur,
        package: Spur,
        names: &ThreadedRodeo,
    ) -> Option<Member> {
        let exported = names.resolve(&name).starts_with(char::is_uppercase);
        let matches = |other: Spur, owner: Option<Spur>| {
            other == name && (exported || owner == Some(package))
        };
        // Through one pointer, the members of what it points to.
        let start = match &*self.get(self.underlying(ty)) {
            Type::Pointer(elem) if !self.is_interface(*elem) => *elem,
            _ => ty,
        };
        let mut seen = vec![start];
        let mut level = vec![start];
        while !level.is_empty() {
            let mut found = Vec::new();
            let mut next = Vec::new();
            for &current in &level {
                if let Type::Named { id, args } = &*self.get(current) {
                    let declared = self.read(|inner| {
                        let mut methods = inner.named[id.0 as usize].methods.iter();
                        methods
                            .find(|m| matches(m.method.name, m.method.package))
                            .cloned()
                    });
                    if let Some(method) = declared {
                        found.push(Member::Method {
                            sig: self.substitute(method.method.sig, &method.receiver_params, args),
                            pointer_receiver: method.pointer_receiver,
                        });
                        continue;
                    }
                }
                match &*self.get(self.underlying(current)) {
                    Type::Struct(fields) => {
                        for field in fields.iter() {
                            if matches(field.name, field.package) {
                                found.push(Member::Field { ty: field.ty });
                            } else if field.embedded {
                                let inner = match &*self.get(field.ty) {
                                    Type::Pointer(elem) => *elem,
                                    _ => field.ty,
                                };
                                if !seen.contains(&inner) {
                                    seen.push(inner);
                                    next.push(inner);
                                }
                            }
                        }
                    }
                    Type::Interface { .. } => {
                        let methods = self.interface_methods(current, names).unwrap_or_default();
                        if let Some(method) = methods.iter().find(|m| matches(m.name, m.package)) {
                            found.push(Member::Method {
                                sig: method.sig,
                                pointer_receiver: false,
                            });
                        }
                    }
                    _ => {}
                }
            }
            match found[..] {
                [] => level = next,
                [one] => return Some(one),
                _ => return None,
            }
        }
        None
    }

    pub fn add_method(&self, named: NamedId, method: DeclaredMethod) {
        self.write(|inner| inner.named[named.0 as usize].methods.push(method));
    }

    /// The declared type `named` with the type arguments `args`.
    pub fn named(&self, named: NamedId, args: &[TypeId]) -> TypeId {
        self.intern(Type::Named {
            id: named,
            args: args.into(),
        })
    }

    /// A new type parameter.
    pub fn param(&self, name: Spur) -> TypeId {
        let id = self.write(|inner| {
            inner.params.push(name);
            ParamId(inner.params.len() as u32 - 1)
        });
        self.intern(Type::Param(id))
    }

    // ---- relations

    /// The underlying type: of a declared type the type it was declared
    /// as, of any other type itself. Invalid while the declaration of a
    /// declared type is not checked yet.
    pub fn underlying(&self, id: TypeId) -> TypeId {
        let Type::Named { id: named, args } = &*self.get(id) else {
            return id;
        };
        let (underlying, params, known) = self.read(|inner| {
            let named = &inner.named[named.0 as usize];
            (
                named.underlying,
                named.params.clone(),
                inner.instances.get(&id).copied(),
            )
        });
        let Some(underlying) = underlying else {
            return Self::INVALID;
        };
        if args.is_empty() || params.len() != args.len() {
            return underlying;
        }
        // A generic type with arguments: what it was declared as, with the
        // arguments in the place of its parameters.
        known.unwrap_or_else(|| {
            let instance = self.substitute(underlying, &params, args);
            self.write(|inner| inner.instances.insert(id, instance));
            instance
        })
    }

    pub fn is_untyped(&self, id: TypeId) -> bool {
        matches!(&*self.get(id), Type::Basic(basic) if basic.is_untyped())
    }

    pub fn is_interface(&self, id: TypeId) -> bool {
        matches!(&*self.get(self.underlying(id)), Type::Interface { .. })
    }

    fn is_named(&self, id: TypeId) -> bool {
        matches!(
            &*self.get(id),
            Type::Named { .. } | Type::Param(_) | Type::Basic(_)
        )
    }

    /// The methods of an interface with those of the interfaces it embeds,
    /// in the order of their names, each once. `None` if `id` is no
    /// interface or holds type terms, which make it a constraint.
    pub fn interface_methods(&self, id: TypeId, names: &ThreadedRodeo) -> Option<Vec<Method>> {
        let mut all = Vec::new();
        let mut seen = vec![self.underlying(id)];
        let mut next = 0;
        // Embedding is followed with a list of work: an interface may
        // reach itself through its own methods' types, not through here.
        while let Some(&current) = seen.get(next) {
            next += 1;
            let Type::Interface { methods, embedded } = &*self.get(current) else {
                return None;
            };
            all.extend(methods.iter().cloned());
            for &inner in embedded.iter() {
                let inner = self.underlying(inner);
                if !seen.contains(&inner) {
                    seen.push(inner);
                }
            }
        }
        let key = |m: &Method| (names.resolve(&m.name), m.package.map(|p| names.resolve(&p)));
        all.sort_by(|a, b| key(a).cmp(&key(b)));
        all.dedup();
        Some(all)
    }

    /// Whether two types are identical.
    pub fn identical(&self, a: TypeId, b: TypeId, names: &ThreadedRodeo) -> bool {
        if a == b {
            return true;
        }
        // Only interfaces have more than one spelling.
        let literal = |id| matches!(&*self.get(id), Type::Interface { .. });
        literal(a)
            && literal(b)
            && match (
                self.interface_methods(a, names),
                self.interface_methods(b, names),
            ) {
                (Some(a), Some(b)) => a == b,
                _ => false,
            }
    }

    /// Whether values of the type can be compared with `==`.
    pub fn comparable(&self, id: TypeId) -> bool {
        match &*self.get(self.underlying(id)) {
            Type::Basic(basic) => *basic != Basic::UntypedNil,
            // Pointers of every kind compare by address.
            Type::Pointer(_)
            | Type::Unique(_)
            | Type::Shared(_)
            | Type::Weak(_)
            | Type::Chan { .. }
            | Type::Interface { .. }
            | Type::Invalid => true,
            Type::Option(elem) | Type::Local(elem) | Type::Array { elem, .. } => {
                self.comparable(*elem)
            }
            Type::Struct(fields) => fields.iter().all(|field| self.comparable(field.ty)),
            _ => false,
        }
    }

    /// Whether `nil` is a value of the type. In a `.gopp` file a pointer,
    /// a func and an interface with methods are never nil (TYP-2).
    pub fn has_nil(&self, id: TypeId, dialect: Dialect) -> bool {
        let go = dialect == Dialect::Go;
        match &*self.get(self.underlying(id)) {
            Type::Slice(_) | Type::Map { .. } | Type::Chan { .. } | Type::Option(_) => true,
            Type::Basic(Basic::UntypedNil) | Type::Invalid => true,
            Type::Pointer(_) | Type::Func { .. } => go,
            Type::Interface { methods, embedded } => {
                go || id == self.error || (methods.is_empty() && embedded.is_empty())
            }
            _ => false,
        }
    }

    /// The methods that values of the type `id` have: those of an
    /// interface, or those declared for a declared type. A pointer to a
    /// declared type has them all, the type itself only those with a value
    /// receiver.
    fn method_set(&self, id: TypeId, names: &ThreadedRodeo) -> Vec<Method> {
        if let Some(methods) = self.interface_methods(id, names) {
            return methods;
        }
        let (named, through_pointer) = match &*self.get(id) {
            Type::Pointer(elem) => (*elem, true),
            _ => (id, false),
        };
        let Type::Named { id: named, .. } = &*self.get(named) else {
            return Vec::new();
        };
        self.read(|inner| {
            let declared = inner.named[named.0 as usize].methods.iter();
            let usable = declared.filter(|m| through_pointer || !m.pointer_receiver);
            usable.map(|m| m.method.clone()).collect()
        })
    }

    /// Whether the type `id` has every method of the interface `iface`.
    pub fn implements(&self, id: TypeId, iface: TypeId, names: &ThreadedRodeo) -> bool {
        let Some(wanted) = self.interface_methods(iface, names) else {
            return false;
        };
        if wanted.is_empty() {
            return true;
        }
        let has = self.method_set(id, names);
        wanted.iter().all(|method| has.contains(method))
    }

    /// Whether a value of the type `from` may be assigned to a place of
    /// the type `to` in a file of `dialect`. For a constant, `value` is
    /// its value: an untyped constant must fit the type.
    pub fn assignable(
        &self,
        from: TypeId,
        value: Option<&Value>,
        to: TypeId,
        dialect: Dialect,
        names: &ThreadedRodeo,
    ) -> bool {
        if from == Self::INVALID || to == Self::INVALID || self.identical(from, to, names) {
            return true;
        }
        let (from_under, to_under) = (self.underlying(from), self.underlying(to));
        let (from_type, to_type) = (self.get(from_under), self.get(to_under));
        if let Type::Basic(kind) = &*from_type
            && kind.is_untyped()
        {
            return self.untyped_assignable(*kind, value, to, dialect);
        }
        // Identical underlying types, and one side is not a declared type.
        let one_unnamed = !self.is_named(from) || !self.is_named(to);
        if one_unnamed && self.identical(from_under, to_under, names) {
            return true;
        }
        match (&*from_type, &*to_type) {
            (_, Type::Interface { .. }) => self.implements(from, to, names),
            // A channel for both directions is one for either.
            (
                Type::Chan {
                    dir: ChanDir::Both,
                    elem: a,
                },
                Type::Chan { elem: b, .. },
            ) => one_unnamed && a == b,
            // A value is its own option (TYP-2).
            (_, Type::Option(elem)) => self.assignable(from, value, *elem, dialect, names),
            _ => false,
        }
    }

    /// Whether an untyped value of the kind `kind` may be assigned to a
    /// place of the type `to`.
    fn untyped_assignable(
        &self,
        kind: Basic,
        value: Option<&Value>,
        to: TypeId,
        dialect: Dialect,
    ) -> bool {
        if kind == Basic::UntypedNil {
            return self.has_nil(to, dialect);
        }
        match &*self.get(self.underlying(to)) {
            Type::Basic(target) => match value {
                Some(value) => representable(value, *target).is_some(),
                // The result of a comparison, or a shift that is not
                // constant: only its kind is known.
                None => match kind {
                    Basic::UntypedBool => *target == Basic::Bool || *target == Basic::UntypedBool,
                    Basic::UntypedString => {
                        *target == Basic::String || *target == Basic::UntypedString
                    }
                    _ => !matches!(
                        target,
                        Basic::Bool
                            | Basic::String
                            | Basic::UntypedBool
                            | Basic::UntypedString
                            | Basic::UntypedNil
                    ),
                },
            },
            // A value in an interface has a type: only the empty
            // interface takes one that has none yet.
            Type::Interface { methods, embedded } => methods.is_empty() && embedded.is_empty(),
            Type::Option(elem) => self.untyped_assignable(kind, value, *elem, dialect),
            _ => false,
        }
    }

    /// The type that an untyped constant of the kind of `id` has where no
    /// type is asked for.
    pub fn default_type(&self, id: TypeId) -> TypeId {
        match &*self.get(id) {
            Type::Basic(basic) => self.basic(basic.default_type()),
            _ => id,
        }
    }

    // ---- text

    /// The type as a program writes it.
    pub fn display(&self, id: TypeId, names: &ThreadedRodeo) -> String {
        self.show(id, names, false)
    }

    /// The type in one spelling for all identical types: an interface is
    /// written with all its methods, in the order of their names, then its
    /// type terms.
    pub fn canonical(&self, id: TypeId, names: &ThreadedRodeo) -> String {
        self.show(id, names, true)
    }

    /// The methods and the type terms of an interface, each as text, with
    /// those of the interfaces it embeds.
    fn interface_parts(&self, id: TypeId, names: &ThreadedRodeo) -> (Vec<String>, Vec<String>) {
        let (mut methods, mut terms) = (Vec::new(), Vec::new());
        let mut seen = vec![self.underlying(id)];
        let mut next = 0;
        while let Some(&current) = seen.get(next) {
            next += 1;
            let Type::Interface {
                methods: own,
                embedded,
            } = &*self.get(current)
            else {
                continue;
            };
            for method in own.iter() {
                let name = names.resolve(&method.name);
                methods.push(format!("{name}{}", self.signature(method.sig, names, true)));
            }
            for &inner in embedded.iter() {
                let under = self.underlying(inner);
                match &*self.get(under) {
                    Type::Interface { .. } if !seen.contains(&under) => seen.push(under),
                    Type::Interface { .. } => {}
                    _ => terms.push(self.show(inner, names, true)),
                }
            }
        }
        methods.sort();
        methods.dedup();
        terms.sort();
        terms.dedup();
        (methods, terms)
    }

    fn show(&self, id: TypeId, names: &ThreadedRodeo, canonical: bool) -> String {
        let show = |id: TypeId| self.show(id, names, canonical);
        let list = |ids: &[TypeId]| {
            ids.iter()
                .map(|&id| show(id))
                .collect::<Vec<_>>()
                .join(", ")
        };
        match &*self.get(id) {
            Type::Invalid => "invalid type".to_owned(),
            Type::Basic(basic) => basic_name(*basic).to_owned(),
            Type::Pointer(elem) => format!("*{}", show(*elem)),
            Type::Unique(elem) => format!("Unique[{}]", show(*elem)),
            Type::Shared(elem) => format!("Shared[{}]", show(*elem)),
            Type::Weak(elem) => format!("Weak[{}]", show(*elem)),
            Type::Option(elem) => format!("Option[{}]", show(*elem)),
            Type::Local(elem) => format!("Local[{}]", show(*elem)),
            Type::Array { len, elem } => format!("[{len}]{}", show(*elem)),
            Type::Slice(elem) => format!("[]{}", show(*elem)),
            Type::Map { key, value } => format!("map[{}]{}", show(*key), show(*value)),
            Type::Chan { dir, elem } => match dir {
                ChanDir::Both => format!("chan {}", show(*elem)),
                ChanDir::Send => format!("chan<- {}", show(*elem)),
                ChanDir::Recv => format!("<-chan {}", show(*elem)),
            },
            Type::Tuple(types) => format!("({})", list(types)),
            Type::Func { .. } => format!("func{}", self.signature(id, names, canonical)),
            Type::Struct(fields) => {
                let field = |f: &Field| {
                    let name = if f.embedded {
                        String::new()
                    } else {
                        format!("{} ", names.resolve(&f.name))
                    };
                    let required = if f.required { " @Required" } else { "" };
                    let tag = f.tag.as_ref().map_or(String::new(), |tag| {
                        format!(" {:?}", String::from_utf8_lossy(tag))
                    });
                    format!("{name}{}{required}{tag}", show(f.ty))
                };
                let fields: Vec<String> = fields.iter().map(field).collect();
                format!("struct{{{}}}", fields.join("; "))
            }
            Type::Interface { methods, embedded } => {
                if methods.is_empty() && embedded.is_empty() {
                    return "any".to_owned();
                }
                if canonical {
                    let (methods, terms) = self.interface_parts(id, names);
                    let parts: Vec<String> = methods.into_iter().chain(terms).collect();
                    return match parts.is_empty() {
                        true => "any".to_owned(),
                        false => format!("interface{{{}}}", parts.join("; ")),
                    };
                }
                let method = |m: &Method| {
                    let name = names.resolve(&m.name);
                    format!("{name}{}", self.signature(m.sig, names, false))
                };
                let parts: Vec<String> = methods
                    .iter()
                    .map(method)
                    .chain(embedded.iter().map(|&id| show(id)))
                    .collect();
                format!("interface{{{}}}", parts.join("; "))
            }
            Type::Union(terms) => {
                let term = |t: &Term| format!("{}{}", if t.tilde { "~" } else { "" }, show(t.ty));
                terms.iter().map(term).collect::<Vec<_>>().join(" | ")
            }
            Type::Marker(Marker::Comparable) => "comparable".to_owned(),
            Type::Marker(marker) => format!("{marker:?}"),
            Type::Named { id: named, args } => {
                let (name, package) = self.read(|inner| {
                    let named = &inner.named[named.0 as usize];
                    (named.name, named.package)
                });
                let package = package.map_or(String::new(), |p| format!("{}.", names.resolve(&p)));
                let args = if args.is_empty() {
                    String::new()
                } else {
                    format!("[{}]", list(args))
                };
                format!("{package}{}{args}", names.resolve(&name))
            }
            Type::Param(param) => {
                let name = self.read(|inner| inner.params[param.0 as usize]);
                names.resolve(&name).to_owned()
            }
        }
    }

    /// `(int, ...string) (bool, error)`: the parameters and results of the
    /// function type `id`.
    fn signature(&self, id: TypeId, names: &ThreadedRodeo, canonical: bool) -> String {
        let Type::Func {
            params,
            results,
            variadic,
        } = &*self.get(id)
        else {
            return String::new();
        };
        let mut shown: Vec<String> = params
            .iter()
            .map(|&p| self.show(p, names, canonical))
            .collect();
        if *variadic
            && let Some((last, &ty)) = shown.last_mut().zip(params.last())
            && let Type::Slice(elem) = &*self.get(ty)
        {
            *last = format!("...{}", self.show(*elem, names, canonical));
        }
        let results = match &results[..] {
            [] => String::new(),
            [one] => format!(" {}", self.show(*one, names, canonical)),
            many => {
                let many: Vec<String> = many
                    .iter()
                    .map(|&r| self.show(r, names, canonical))
                    .collect();
                format!(" ({})", many.join(", "))
            }
        };
        format!("({}){results}", shown.join(", "))
    }
}

fn basic_name(basic: Basic) -> &'static str {
    match basic {
        Basic::Bool => "bool",
        Basic::Int => "int",
        Basic::Int8 => "int8",
        Basic::Int16 => "int16",
        Basic::Int32 => "int32",
        Basic::Int64 => "int64",
        Basic::Uint => "uint",
        Basic::Uint8 => "uint8",
        Basic::Uint16 => "uint16",
        Basic::Uint32 => "uint32",
        Basic::Uint64 => "uint64",
        Basic::Uintptr => "uintptr",
        Basic::Float32 => "float32",
        Basic::Float64 => "float64",
        Basic::Complex64 => "complex64",
        Basic::Complex128 => "complex128",
        Basic::String => "string",
        Basic::UntypedBool => "untyped bool",
        Basic::UntypedInt => "untyped int",
        Basic::UntypedRune => "untyped rune",
        Basic::UntypedFloat => "untyped float",
        Basic::UntypedComplex => "untyped complex",
        Basic::UntypedString => "untyped string",
        Basic::UntypedNil => "untyped nil",
    }
}

#[cfg(test)]
mod tests;
